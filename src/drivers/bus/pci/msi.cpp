#include <drivers/apic/ioapic.h>
#include <drivers/bus/pci/msi.h>
#include <drivers/bus/pci/pci.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/debug.h>
#include <kernel/irq.h>
#include <kernel/mm/vmm.h>

namespace {

// PCI_CAP_ID_MSI (0x05) and PCI_CAP_ID_MSIX (0x11) come from msi.h as macros.
static constexpr uint16_t MSI_CTRL_ENABLE = 0x0001u;
static constexpr uint16_t MSI_CTRL_MULTI_MSG_ENABLE_MASK = 0x0070u;
static constexpr uint16_t MSI_CTRL_64BIT = 0x0080u;
static constexpr uint16_t MSIX_CTRL_ENABLE_MASK = MSIX_CTRL_ENABLE;

static bool pci_capabilities_present(const PciDevice *dev)
{
    if (!dev)
        return false;
    return (pci_config_read16(dev->bus, dev->device, dev->function, PCI_STATUS) & PCI_STATUS_CAP_LIST) != 0;
}

static bool pci_cap_offset_valid(uint8_t cap_ptr)
{
    // cap_ptr is uint8_t so it is always < 0x100; only the lower bound matters.
    return cap_ptr >= 0x40u;
}

static uint16_t pci_read_msi_control(const PciDevice *dev, uint8_t cap_offset)
{
    return pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + 2u));
}

static void pci_write_msi_control(const PciDevice *dev, uint8_t cap_offset, uint16_t value)
{
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + 2u), value);
}

static bool pci_msix_enabled(const PciDevice *dev)
{
    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!cap_offset)
        return false;
    uint16_t ctrl = pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL));
    return (ctrl & MSIX_CTRL_ENABLE_MASK) != 0;
}

static void pci_disable_msi_if_present(const PciDevice *dev)
{
    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (!cap_offset)
        return;

    uint16_t ctrl = pci_read_msi_control(dev, cap_offset);
    ctrl &= (uint16_t)~MSI_CTRL_ENABLE;
    ctrl &= (uint16_t)~MSI_CTRL_MULTI_MSG_ENABLE_MASK;
    pci_write_msi_control(dev, cap_offset, ctrl);
}

static void pci_disable_msix_if_present(const PciDevice *dev)
{
    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!cap_offset)
        return;

    uint16_t ctrl = pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL));
    ctrl &= (uint16_t) ~(MSIX_CTRL_ENABLE | MSIX_CTRL_FUNC_MASK);
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL), ctrl);
}

static volatile MsixTableEntry *msix_table_entry(const MsixState *state, uint32_t index)
{
    if (!state || index >= state->table_size)
        return nullptr;
    return reinterpret_cast<volatile MsixTableEntry *>(state->table_virt + (uint64_t)index * sizeof(MsixTableEntry));
}

} // namespace

uint8_t pci_find_capability(const PciDevice *dev, uint8_t cap_id)
{
    if (!dev || !pci_capabilities_present(dev))
        return 0;

    uint8_t cap_ptr = pci_config_read8(dev->bus, dev->device, dev->function, PCI_CAPABILITY_LIST) & 0xFCu;
    uint32_t iterations = 0;
    while (cap_ptr != 0u && iterations < 48u) {
        if (!pci_cap_offset_valid(cap_ptr))
            return 0;

        uint8_t id = pci_config_read8(dev->bus, dev->device, dev->function, cap_ptr);
        if (id == cap_id)
            return cap_ptr;

        uint8_t next = pci_config_read8(dev->bus, dev->device, dev->function, (uint8_t)(cap_ptr + 1u));
        if ((next & 0xFCu) == cap_ptr)
            return 0;
        cap_ptr = next & 0xFCu;
        ++iterations;
    }

    return 0;
}

bool pci_has_msix(const PciDevice *dev)
{
    return pci_find_capability(dev, PCI_CAP_ID_MSIX) != 0;
}

uint16_t pci_msix_table_size(const PciDevice *dev)
{
    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!cap_offset)
        return 0;

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL));
    return (uint16_t)((msg_ctrl & MSIX_CTRL_TABLE_SIZE) + 1u);
}

bool pci_enable_msix(const PciDevice *dev, MsixState *state)
{
    if (!dev || !state)
        return false;

    *state = {};

    if (!apic_is_enabled()) {
        DEBUG_WARN("MSI-X requires a working local APIC; using routed interrupts instead");
        return false;
    }

    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (!cap_offset) {
        DEBUG_WARN("device does not support MSI-X");
        return false;
    }

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL));
    uint16_t table_size = (uint16_t)((msg_ctrl & MSIX_CTRL_TABLE_SIZE) + 1u);
    if (table_size == 0u || table_size > MSIX_MAX_VECTORS) {
        DEBUG_ERROR("MSI-X table size %u is unsupported", table_size);
        return false;
    }

    uint32_t table_info =
        pci_config_read32(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_TABLE_OFFSET));
    uint32_t table_bar = table_info & MSIX_BIR_MASK;
    uint32_t table_offset = table_info & MSIX_OFFSET_MASK;
    if (table_bar > 5u || !pci_bar_is_mmio(dev, (int)table_bar)) {
        DEBUG_ERROR("MSI-X table BAR%u is invalid or not MMIO", table_bar);
        return false;
    }

    uint64_t bar_size = 0;
    uint64_t bar_phys = pci_get_bar(dev, (int)table_bar, &bar_size);
    if (!bar_phys || !bar_size) {
        DEBUG_ERROR("failed to resolve BAR%u for MSI-X table", table_bar);
        return false;
    }

    uint64_t table_bytes = (uint64_t)table_size * sizeof(MsixTableEntry);
    if ((uint64_t)table_offset >= bar_size || table_bytes > (bar_size - (uint64_t)table_offset)) {
        DEBUG_ERROR("MSI-X table overflows BAR%u (offset=0x%x, bytes=%lu, bar=%lu)", table_bar, table_offset,
                    table_bytes, bar_size);
        return false;
    }

    uint64_t bar_virt = vmm_map_mmio(bar_phys, bar_size);
    if (!bar_virt) {
        DEBUG_ERROR("failed to map MSI-X BAR%u", table_bar);
        return false;
    }

    pci_disable_msi_if_present(dev);

    state->cap_offset = cap_offset;
    state->table_size = table_size;
    state->table_bar = static_cast<uint8_t>(table_bar & 0x07u);
    state->table_offset = table_offset;
    state->table_virt = bar_virt + table_offset;

    uint32_t pba_info =
        pci_config_read32(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_PBA_OFFSET));
    state->pba_bar = pba_info & MSIX_BIR_MASK;
    state->pba_offset = pba_info & MSIX_OFFSET_MASK;

    DEBUG_INFO("MSI-X capability at 0x%x, table size=%u, table=BAR%u+0x%x", cap_offset, state->table_size,
               state->table_bar, state->table_offset);

    msg_ctrl |= MSIX_CTRL_FUNC_MASK;
    msg_ctrl &= (uint16_t)~MSIX_CTRL_ENABLE;
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL), msg_ctrl);

    for (uint16_t i = 0; i < state->table_size; ++i) {
        volatile MsixTableEntry *entry = msix_table_entry(state, i);
        entry->vector_ctrl = MSIX_ENTRY_CTRL_MASK;
    }
    asm volatile("mfence" ::: "memory");

    msg_ctrl |= MSIX_CTRL_ENABLE;
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + MSIX_MSG_CTRL), msg_ctrl);

    pci_disable_interrupts(dev);
    state->enabled = true;
    DEBUG_INFO("MSI-X enabled for device %x:%x.%x", dev->bus, dev->device, dev->function);
    return true;
}

void pci_disable_msix(const PciDevice *dev, MsixState *state)
{
    if (!dev || !state || !state->enabled)
        return;

    uint16_t msg_ctrl =
        pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(state->cap_offset + MSIX_MSG_CTRL));
    msg_ctrl &= (uint16_t) ~(MSIX_CTRL_ENABLE | MSIX_CTRL_FUNC_MASK);
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(state->cap_offset + MSIX_MSG_CTRL), msg_ctrl);

    pci_enable_interrupts(dev);
    state->enabled = false;
    DEBUG_INFO("MSI-X disabled for device %x:%x.%x", dev->bus, dev->device, dev->function);
}

void msix_set_entry(MsixState *state, uint32_t index, uint8_t vector, uint8_t dest_apic_id)
{
    if (!state || !state->enabled || index >= state->table_size) {
        DEBUG_WARN("invalid MSI-X entry setup: state=%p, index=%u", state, index);
        return;
    }
    if (vector < 32u) {
        DEBUG_WARN("refusing to program MSI-X vector %u", vector);
        return;
    }

    volatile MsixTableEntry *entry = msix_table_entry(state, index);
    entry->vector_ctrl = MSIX_ENTRY_CTRL_MASK;
    asm volatile("mfence" ::: "memory");

    entry->msg_addr_lo = msix_make_address(dest_apic_id);
    entry->msg_addr_hi = 0u;
    entry->msg_data = msix_make_data(vector, 0);
    asm volatile("mfence" ::: "memory");

    DEBUG_INFO("MSI-X entry %u prepared: vector=0x%x dest_apic=%u", index, vector, dest_apic_id);
}

void msix_mask_vector(MsixState *state, uint32_t index)
{
    volatile MsixTableEntry *entry = msix_table_entry(state, index);
    if (!state || !state->enabled || !entry)
        return;
    entry->vector_ctrl |= MSIX_ENTRY_CTRL_MASK;
    asm volatile("mfence" ::: "memory");
}

void msix_unmask_vector(MsixState *state, uint32_t index)
{
    volatile MsixTableEntry *entry = msix_table_entry(state, index);
    if (!state || !state->enabled || !entry)
        return;
    entry->vector_ctrl &= (uint32_t)~MSIX_ENTRY_CTRL_MASK;
    asm volatile("mfence" ::: "memory");
}

void msix_mask_all(const PciDevice *dev, MsixState *state)
{
    if (!dev || !state || !state->enabled)
        return;

    uint16_t msg_ctrl =
        pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(state->cap_offset + MSIX_MSG_CTRL));
    msg_ctrl |= MSIX_CTRL_FUNC_MASK;
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(state->cap_offset + MSIX_MSG_CTRL), msg_ctrl);
}

void msix_unmask_all(const PciDevice *dev, MsixState *state)
{
    if (!dev || !state || !state->enabled)
        return;

    uint16_t msg_ctrl =
        pci_config_read16(dev->bus, dev->device, dev->function, (uint8_t)(state->cap_offset + MSIX_MSG_CTRL));
    msg_ctrl &= (uint16_t)~MSIX_CTRL_FUNC_MASK;
    pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(state->cap_offset + MSIX_MSG_CTRL), msg_ctrl);
}

bool pci_has_msi(const PciDevice *dev)
{
    return pci_find_capability(dev, PCI_CAP_ID_MSI) != 0;
}

bool pci_enable_msi(const PciDevice *dev, uint8_t *out_vector)
{
    if (!dev || !out_vector)
        return false;

    if (!apic_is_enabled()) {
        DEBUG_WARN("MSI requires a working local APIC; using routed interrupts instead");
        return false;
    }

    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (!cap_offset) {
        DEBUG_WARN("Device does not support standard MSI");
        return false;
    }

    if (pci_msix_enabled(dev)) {
        DEBUG_WARN("Device already has MSI-X enabled");
        return false;
    }

    uint16_t msg_ctrl = pci_read_msi_control(dev, cap_offset);
    bool is_64bit = (msg_ctrl & MSI_CTRL_64BIT) != 0;

    uint8_t allocated_vector = idt_allocate_free_vector();
    if (allocated_vector < 32u) {
        DEBUG_ERROR("Failed to allocate MSI vector");
        return false;
    }

    pci_disable_msix_if_present(dev);

    uint32_t lapic_id = apic_get_current_id();
    uint32_t msg_addr = msix_make_address((uint8_t)lapic_id);
    uint16_t msg_data = (uint16_t)msix_make_data(allocated_vector, 0);

    pci_config_write32(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + 4u), msg_addr);
    if (is_64bit) {
        pci_config_write32(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + 8u), 0u);
        pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + 0x0Cu), msg_data);
    } else {
        pci_config_write16(dev->bus, dev->device, dev->function, (uint8_t)(cap_offset + 8u), msg_data);
    }

    msg_ctrl &= (uint16_t)~MSI_CTRL_MULTI_MSG_ENABLE_MASK;
    msg_ctrl |= MSI_CTRL_ENABLE;
    pci_write_msi_control(dev, cap_offset, msg_ctrl);

    pci_disable_interrupts(dev);

    *out_vector = allocated_vector;
    DEBUG_INFO("Standard MSI enabled for device %x:%x.%x on vector %u", dev->bus, dev->device, dev->function,
               allocated_vector);
    return true;
}
