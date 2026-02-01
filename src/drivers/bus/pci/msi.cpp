#include <drivers/bus/pci/msi.h>
#include <drivers/bus/pci/pci.h>

#include <kernel/debug.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/io.h>

uint8_t pci_find_capability(const PciDevice* dev, uint8_t cap_id) {
    uint16_t status = pci_config_read16(dev->bus, dev->device, dev->function, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        return 0;
    }

    uint8_t cap_ptr = pci_config_read8(dev->bus, dev->device, dev->function, PCI_CAPABILITY_LIST);
    cap_ptr &= 0xFC;

    uint32_t iterations = 0;
    while (cap_ptr != 0 && iterations < 48) {
        uint8_t id = pci_config_read8(dev->bus, dev->device, dev->function, cap_ptr + 0);
        
        if (id == cap_id) {
            return cap_ptr;
        }

        cap_ptr = pci_config_read8(dev->bus, dev->device, dev->function, cap_ptr + 1);
        cap_ptr &= 0xFC;
        iterations++;
    }

    return 0;
}

bool pci_has_msix(const PciDevice* dev) {
    return pci_find_capability(dev, PCI_CAP_ID_MSIX) != 0;
}

uint16_t pci_msix_table_size(const PciDevice* dev) {
    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (cap_offset == 0) {
        return 0;
    }

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function, 
                                          cap_offset + MSIX_MSG_CTRL);
    
    return (msg_ctrl & MSIX_CTRL_TABLE_SIZE) + 1;
}

bool pci_enable_msix(const PciDevice* dev, MsixState* state) {
    if (!dev || !state) {
        return false;
    }

    uint8_t cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    if (cap_offset == 0) {
        DEBUG_WARN("device does not support MSI-X");
        return false;
    }

    state->cap_offset = cap_offset;

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function, 
                                          cap_offset + MSIX_MSG_CTRL);
    state->table_size = (msg_ctrl & MSIX_CTRL_TABLE_SIZE) + 1;

    DEBUG_INFO("MSI-X capability at 0x%x, table size: %d", cap_offset, state->table_size);

    uint32_t table_info = pci_config_read32(dev->bus, dev->device, dev->function,
                                            cap_offset + MSIX_TABLE_OFFSET);
    state->table_bar = table_info & MSIX_BIR_MASK;
    state->table_offset = table_info & MSIX_OFFSET_MASK;

    uint32_t pba_info = pci_config_read32(dev->bus, dev->device, dev->function,
                                          cap_offset + MSIX_PBA_OFFSET);
    state->pba_bar = pba_info & MSIX_BIR_MASK;
    state->pba_offset = pba_info & MSIX_OFFSET_MASK;

    DEBUG_INFO("MSI-X table: BAR%d + 0x%x", state->table_bar, state->table_offset);

    uint64_t bar_size = 0;
    uint64_t bar_phys = pci_get_bar(dev, state->table_bar, &bar_size);
    
    if (bar_phys == 0 || bar_size == 0) {
        DEBUG_ERROR("failed to get BAR%d for MSI-X table", state->table_bar);
        return false;
    }

    uint64_t bar_virt = vmm_map_mmio(bar_phys, bar_size);
    if (bar_virt == 0) {
        DEBUG_ERROR("failed to map MSI-X table BAR");
        return false;
    }

    state->table_virt = bar_virt + state->table_offset;

    msg_ctrl |= MSIX_CTRL_FUNC_MASK;
    pci_config_write16(dev->bus, dev->device, dev->function, 
                       cap_offset + MSIX_MSG_CTRL, msg_ctrl);

    for (uint16_t i = 0; i < state->table_size && i < MSIX_MAX_VECTORS; i++) {
        volatile MsixTableEntry* entry = (volatile MsixTableEntry*)(state->table_virt + i * sizeof(MsixTableEntry));
        entry->vector_ctrl = MSIX_ENTRY_CTRL_MASK;
    }

    msg_ctrl |= MSIX_CTRL_ENABLE;
    pci_config_write16(dev->bus, dev->device, dev->function,
                       cap_offset + MSIX_MSG_CTRL, msg_ctrl);

    pci_disable_interrupts(dev);

    state->enabled = true;
    DEBUG_INFO("MSI-X enabled for device %02x:%02x.%x", dev->bus, dev->device, dev->function);

    return true;
}

void pci_disable_msix(const PciDevice* dev, MsixState* state) {
    if (!dev || !state || !state->enabled) {
        return;
    }

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function,
                                          state->cap_offset + MSIX_MSG_CTRL);
    msg_ctrl &= ~MSIX_CTRL_ENABLE;
    pci_config_write16(dev->bus, dev->device, dev->function,
                       state->cap_offset + MSIX_MSG_CTRL, msg_ctrl);

    pci_enable_interrupts(dev);

    state->enabled = false;
    DEBUG_INFO("MSI-X disabled for device %02x:%02x.%x", dev->bus, dev->device, dev->function);
}

void msix_set_entry(MsixState* state, uint32_t index, uint8_t vector, uint8_t dest_apic_id) {
    if (!state || !state->enabled || index >= state->table_size) {
        DEBUG_WARN("invalid MSI-X entry setup: state=%p, index=%d", state, index);
        return;
    }

    volatile MsixTableEntry* entry = (volatile MsixTableEntry*)(state->table_virt + index * sizeof(MsixTableEntry));

    entry->msg_addr_lo = msix_make_address(dest_apic_id);
    entry->msg_addr_hi = 0;
    entry->msg_data = msix_make_data(vector, 0);

    asm volatile("mfence" ::: "memory");

    DEBUG_INFO("MSI-X entry %d: vector=0x%x, dest_apic=%d", index, vector, dest_apic_id);
}

void msix_mask_vector(MsixState* state, uint32_t index) {
    if (!state || !state->enabled || index >= state->table_size) {
        return;
    }

    volatile MsixTableEntry* entry = (volatile MsixTableEntry*)(state->table_virt + index * sizeof(MsixTableEntry));
    entry->vector_ctrl |= MSIX_ENTRY_CTRL_MASK;
    asm volatile("mfence" ::: "memory");
}

void msix_unmask_vector(MsixState* state, uint32_t index) {
    if (!state || !state->enabled || index >= state->table_size) {
        return;
    }

    volatile MsixTableEntry* entry = (volatile MsixTableEntry*)(state->table_virt + index * sizeof(MsixTableEntry));
    entry->vector_ctrl &= ~MSIX_ENTRY_CTRL_MASK;
    asm volatile("mfence" ::: "memory");
}

void msix_mask_all(const PciDevice* dev, MsixState* state) {
    if (!dev || !state || !state->enabled) {
        return;
    }

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function,
                                          state->cap_offset + MSIX_MSG_CTRL);
    msg_ctrl |= MSIX_CTRL_FUNC_MASK;
    pci_config_write16(dev->bus, dev->device, dev->function,
                       state->cap_offset + MSIX_MSG_CTRL, msg_ctrl);
}

void msix_unmask_all(const PciDevice* dev, MsixState* state) {
    if (!dev || !state || !state->enabled) {
        return;
    }

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function,
                                          state->cap_offset + MSIX_MSG_CTRL);
    msg_ctrl &= ~MSIX_CTRL_FUNC_MASK;
    pci_config_write16(dev->bus, dev->device, dev->function,
                       state->cap_offset + MSIX_MSG_CTRL, msg_ctrl);
}
