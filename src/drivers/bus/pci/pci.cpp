#include <drivers/acpi/acpi.h>
#include <drivers/bus/pci/pci.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>
#include <kernel/mm/vmm.h>

struct EcamEntry
{
    uint64_t base;
    uint8_t start_bus;
    uint8_t end_bus;
};

static EcamEntry g_ecam_entries[4];
static int g_num_ecam_entries = 0;

namespace {

static constexpr uint8_t k_max_standard_bars = 6;
static constexpr uint8_t k_max_bridge_bars = 2;
static constexpr uint8_t k_max_cardbus_bars = 1;
static constexpr uint8_t k_header_type_mask = 0x7Fu;
static constexpr uint8_t k_header_type_standard = 0x00u;
static constexpr uint8_t k_header_type_bridge = 0x01u;
static constexpr uint8_t k_header_type_cardbus = 0x02u;
static constexpr uint16_t k_command_decode_bits = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;

static bool pci_bdf_valid(uint8_t device, uint8_t func)
{
    return device < PCI_MAX_DEVICE && func < PCI_MAX_FUNC;
}

static bool pci_offset_valid(uint16_t offset, uint16_t width)
{
    return width != 0 && offset < 0x1000u && offset <= (0x1000u - width);
}

static uint32_t pci_make_address(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset)
{
    return (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)(device & 0x1Fu) << 11) | ((uint32_t)(func & 0x07u) << 8) |
           (offset & 0xFCu);
}

static const EcamEntry *pci_find_ecam_entry(uint8_t bus)
{
    for (int i = 0; i < g_num_ecam_entries; ++i) {
        if (bus >= g_ecam_entries[i].start_bus && bus <= g_ecam_entries[i].end_bus)
            return &g_ecam_entries[i];
    }
    return nullptr;
}

static const EcamEntry *g_last_ecam_entry = nullptr;

template <typename T>
static volatile T *pci_ecam_ptr(uint8_t bus, uint8_t device, uint8_t func, uint16_t offset)
{
    if (!pci_bdf_valid(device, func) || !pci_offset_valid(offset, (uint16_t)sizeof(T)))
        return nullptr;

    const EcamEntry *entry = g_last_ecam_entry;
    if (!entry || bus < entry->start_bus || bus > entry->end_bus) {
        entry = pci_find_ecam_entry(bus);
        if (!entry)
            return nullptr;
        g_last_ecam_entry = entry;
    }

    uint64_t addr = entry->base + (((uint64_t)bus - entry->start_bus) << 20) + ((uint64_t)device << 15) +
                    ((uint64_t)func << 12) + (uint64_t)offset;
    return reinterpret_cast<volatile T *>(addr);
}

static bool pci_device_exists(uint8_t bus, uint8_t device, uint8_t func)
{
    return pci_config_read16(bus, device, func, PCI_VENDOR_ID) != 0xFFFFu;
}

static uint8_t pci_bar_count_for_header(uint8_t header_layout)
{
    switch (header_layout) {
        case k_header_type_standard:
            return k_max_standard_bars;
        case k_header_type_bridge:
            return k_max_bridge_bars;
        case k_header_type_cardbus:
            return k_max_cardbus_bars;
        default:
            return 0;
    }
}

static bool pci_bar_index_valid(const PciDevice *dev, int bar_num)
{
    if (!dev || bar_num < 0)
        return false;

    uint8_t header_layout = dev->header_type & k_header_type_mask;
    return (uint8_t)bar_num < pci_bar_count_for_header(header_layout);
}

static uint8_t pci_max_functions(uint8_t bus, uint8_t device)
{
    uint8_t header_type = pci_config_read8(bus, device, 0, PCI_HEADER_TYPE);
    return (header_type & 0x80u) ? PCI_MAX_FUNC : 1u;
}

static void pci_enum_function(uint8_t bus, uint8_t device, uint8_t func, PciDevice *out)
{
    if (!out)
        return;

    out->bus = bus;
    out->device = device;
    out->function = func;
    out->vendor_id = pci_config_read16(bus, device, func, PCI_VENDOR_ID);
    out->device_id = pci_config_read16(bus, device, func, PCI_DEVICE_ID);
    out->class_code = pci_config_read8(bus, device, func, PCI_CLASS);
    out->subclass = pci_config_read8(bus, device, func, PCI_SUBCLASS);
    out->prog_if = pci_config_read8(bus, device, func, PCI_PROG_IF);
    out->header_type = pci_config_read8(bus, device, func, PCI_HEADER_TYPE);
    out->irq_line = pci_config_read8(bus, device, func, PCI_INTERRUPT_LINE);
}

static uint16_t pci_read_command(const PciDevice *dev)
{
    return dev ? pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND) : 0u;
}

static void pci_write_command(const PciDevice *dev, uint16_t cmd)
{
    if (!dev)
        return;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

static void pci_update_command_bits(const PciDevice *dev, uint16_t set_bits, uint16_t clear_bits)
{
    if (!dev)
        return;

    uint16_t cmd = pci_read_command(dev);
    cmd = (uint16_t)((cmd | set_bits) & (uint16_t)~clear_bits);
    pci_write_command(dev, cmd);
}

static uint64_t pci_bar_address_from_raw(uint32_t bar_low, uint32_t bar_high)
{
    if ((bar_low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_IO)
        return (uint64_t)(bar_low & ~0x3u);
    return ((uint64_t)bar_high << 32) | (uint64_t)(bar_low & ~0xFu);
}

static uint64_t pci_bar_size_from_probe(uint32_t probe_low, uint32_t probe_high, bool is_io, bool is_64bit)
{
    uint64_t mask;
    if (is_io) {
        mask = (uint64_t)(probe_low & ~0x3u);
    } else {
        mask = (uint64_t)(probe_low & ~0xFu);
        if (is_64bit) {
            mask |= (uint64_t)probe_high << 32;
        } else {
            mask &= 0xFFFFFFFFull;
        }
    }

    if (mask == 0)
        return 0;

    // Find the alignment/size: the lowest set bit determines the size.
    uint64_t size = mask & (~mask + 1ULL);

    // Sanity check: 32-bit memory BARs cannot be larger than 4GB.
    if (!is_64bit && !is_io && size > 0x100000000ULL) {
        return 0;
    }

    return size;
}

} // namespace

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset)
{
    if (volatile uint32_t *ptr = pci_ecam_ptr<uint32_t>(bus, device, func, offset & 0xFCu))
        return *ptr;

    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset)
{
    if (volatile uint16_t *ptr = pci_ecam_ptr<uint16_t>(bus, device, func, offset))
        return *ptr;

    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((offset & 2u) * 8u));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset)
{
    if (volatile uint8_t *ptr = pci_ecam_ptr<uint8_t>(bus, device, func, offset))
        return *ptr;

    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((offset & 3u) * 8u));
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value)
{
    if (volatile uint32_t *ptr = pci_ecam_ptr<uint32_t>(bus, device, func, offset & 0xFCu)) {
        *ptr = value;
        return;
    }

    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t value)
{
    if (volatile uint16_t *ptr = pci_ecam_ptr<uint16_t>(bus, device, func, offset)) {
        *ptr = value;
        return;
    }

    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    uint32_t shift = (offset & 2u) * 8u;
    tmp &= ~(0xFFFFu << shift);
    tmp |= (uint32_t)value << shift;
    outl(PCI_CONFIG_DATA, tmp);
}

void pci_config_write8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t value)
{
    if (volatile uint8_t *ptr = pci_ecam_ptr<uint8_t>(bus, device, func, offset)) {
        *ptr = value;
        return;
    }

    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    uint32_t shift = (offset & 3u) * 8u;
    tmp &= ~(0xFFu << shift);
    tmp |= (uint32_t)value << shift;
    outl(PCI_CONFIG_DATA, tmp);
}

bool pci_find_device_by_class(uint8_t class_code, uint8_t subclass, PciDevice *out)
{
    if (!out)
        return false;

    for (uint16_t bus = 0; bus < PCI_MAX_BUS; ++bus) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; ++dev) {
            if (!pci_device_exists((uint8_t)bus, dev, 0))
                continue;

            for (uint8_t func = 0; func < pci_max_functions((uint8_t)bus, dev); ++func) {
                if (!pci_device_exists((uint8_t)bus, dev, func))
                    continue;

                uint8_t cls = pci_config_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                if (cls != class_code || sub != subclass)
                    continue;

                pci_enum_function((uint8_t)bus, dev, func, out);
                BOOT_LOG("Found device %02x:%02x.%x (Class %02x:%02x) Vendor=%04x Device=%04x", bus, dev, func, cls,
                         sub, out->vendor_id, out->device_id);
                return true;
            }
        }
    }

    return false;
}

bool pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, PciDevice *out)
{
    if (!out)
        return false;

    for (uint16_t bus = 0; bus < PCI_MAX_BUS; ++bus) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; ++dev) {
            if (!pci_device_exists((uint8_t)bus, dev, 0))
                continue;

            for (uint8_t func = 0; func < pci_max_functions((uint8_t)bus, dev); ++func) {
                if (!pci_device_exists((uint8_t)bus, dev, func))
                    continue;

                uint8_t cls = pci_config_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint8_t pif = pci_config_read8((uint8_t)bus, dev, func, PCI_PROG_IF);
                if (cls != class_code || sub != subclass || pif != prog_if)
                    continue;

                pci_enum_function((uint8_t)bus, dev, func, out);
                BOOT_LOG("Found device %02x:%02x.%x (Class %02x:%02x:%02x) Vendor=%04x Device=%04x", bus, dev, func,
                         cls, sub, pif, out->vendor_id, out->device_id);
                return true;
            }
        }
    }

    return false;
}

bool pci_find_xhci(PciDevice *out)
{
    if (!out)
        return false;

    bool found = false;
    for (uint16_t bus = 0; bus < PCI_MAX_BUS; ++bus) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; ++dev) {
            if (!pci_device_exists((uint8_t)bus, dev, 0))
                continue;

            for (uint8_t func = 0; func < pci_max_functions((uint8_t)bus, dev); ++func) {
                if (!pci_device_exists((uint8_t)bus, dev, func))
                    continue;

                uint8_t cls = pci_config_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint8_t pif = pci_config_read8((uint8_t)bus, dev, func, PCI_PROG_IF);
                if (cls == PCI_CLASS_SERIAL_BUS && sub == PCI_SUBCLASS_USB && pif == PCI_PROGIF_XHCI) {
                    BOOT_LOG("Found xHCI Controller at %02x:%02x.%x Vendor=%04x Device=%04x", bus, dev, func,
                             pci_config_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID),
                             pci_config_read16((uint8_t)bus, dev, func, PCI_DEVICE_ID));
                    if (!found) {
                        pci_enum_function((uint8_t)bus, dev, func, out);
                        found = true;
                    }
                }
            }
        }
    }

    return found;
}

bool pci_find_ac97(PciDevice *out)
{
    return pci_find_device_by_class(PCI_CLASS_AUDIO, PCI_SUBCLASS_AC97, out);
}

bool pci_find_hda(PciDevice *out)
{
    if (!out)
        return false;

    for (uint16_t bus = 0; bus < PCI_MAX_BUS; ++bus) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; ++dev) {
            if (!pci_device_exists((uint8_t)bus, dev, 0))
                continue;

            for (uint8_t func = 0; func < pci_max_functions((uint8_t)bus, dev); ++func) {
                if (!pci_device_exists((uint8_t)bus, dev, func))
                    continue;

                uint8_t cls = pci_config_read8((uint8_t)bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                uint16_t ven = pci_config_read16((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                if (cls == PCI_CLASS_AUDIO && sub == PCI_SUBCLASS_HDA &&
                    (ven == 0x8086u || ven == 0x1022u || ven == 0x1106u)) {
                    pci_enum_function((uint8_t)bus, dev, func, out);
                    return true;
                }
            }
        }
    }

    return false;
}

bool pci_find_display(PciDevice *out)
{
    if (!out)
        return false;

    PciDevice fallback = {};
    bool have_fallback = false;

    for (uint16_t bus = 0; bus < PCI_MAX_BUS; ++bus) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; ++dev) {
            if (!pci_device_exists((uint8_t)bus, dev, 0))
                continue;

            for (uint8_t func = 0; func < pci_max_functions((uint8_t)bus, dev); ++func) {
                if (!pci_device_exists((uint8_t)bus, dev, func))
                    continue;

                if (pci_config_read8((uint8_t)bus, dev, func, PCI_CLASS) != PCI_CLASS_DISPLAY)
                    continue;

                uint8_t sub = pci_config_read8((uint8_t)bus, dev, func, PCI_SUBCLASS);
                PciDevice candidate = {};
                pci_enum_function((uint8_t)bus, dev, func, &candidate);

                if (sub == PCI_SUBCLASS_VGA) {
                    *out = candidate;
                    BOOT_LOG("Found display device %x:%x.%x Vendor=%x Device=%x", bus, dev, func, candidate.vendor_id,
                             candidate.device_id);
                    return true;
                }

                if (!have_fallback) {
                    fallback = candidate;
                    have_fallback = true;
                }
            }
        }
    }

    if (!have_fallback)
        return false;

    *out = fallback;
    BOOT_LOG("Found display device %x:%x.%x Vendor=%x Device=%x", fallback.bus, fallback.device, fallback.function,
             fallback.vendor_id, fallback.device_id);
    return true;
}

uint64_t pci_get_bar(const PciDevice *dev, int bar_num, uint64_t *size_out)
{
    if (size_out)
        *size_out = 0;
    if (!pci_bar_index_valid(dev, bar_num))
        return 0;

    uint8_t offset = (uint8_t)(PCI_BAR0 + bar_num * 4);
    uint32_t bar_low = pci_config_read32(dev->bus, dev->device, dev->function, offset);
    bool is_io = (bar_low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_IO;
    bool is_64bit = !is_io && ((bar_low & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64);
    if (is_64bit && bar_num + 1 >= pci_bar_count_for_header(dev->header_type & k_header_type_mask))
        return 0;

    uint32_t bar_high = 0;
    if (is_64bit)
        bar_high = pci_config_read32(dev->bus, dev->device, dev->function, (uint8_t)(offset + 4));

    uint64_t bar_addr = pci_bar_address_from_raw(bar_low, bar_high);
    if (!size_out)
        return bar_addr;

    uint16_t saved_cmd = pci_read_command(dev);
    pci_write_command(dev, (uint16_t)(saved_cmd & (uint16_t)~k_command_decode_bits));

    pci_config_write32(dev->bus, dev->device, dev->function, offset, 0xFFFFFFFFu);
    uint32_t probe_low = pci_config_read32(dev->bus, dev->device, dev->function, offset);

    uint32_t probe_high = 0;
    if (is_64bit) {
        pci_config_write32(dev->bus, dev->device, dev->function, (uint8_t)(offset + 4), 0xFFFFFFFFu);
        probe_high = pci_config_read32(dev->bus, dev->device, dev->function, (uint8_t)(offset + 4));
        pci_config_write32(dev->bus, dev->device, dev->function, (uint8_t)(offset + 4), bar_high);
    }

    pci_config_write32(dev->bus, dev->device, dev->function, offset, bar_low);
    pci_write_command(dev, saved_cmd);

    *size_out = pci_bar_size_from_probe(probe_low, probe_high, is_io, is_64bit);
    return bar_addr;
}

bool pci_bar_is_mmio(const PciDevice *dev, int bar_num)
{
    if (!pci_bar_index_valid(dev, bar_num))
        return false;
    uint8_t offset = (uint8_t)(PCI_BAR0 + bar_num * 4);
    uint32_t bar = pci_config_read32(dev->bus, dev->device, dev->function, offset);
    return (bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_MEM;
}

bool pci_bar_is_64bit(const PciDevice *dev, int bar_num)
{
    if (!pci_bar_index_valid(dev, bar_num))
        return false;
    uint8_t offset = (uint8_t)(PCI_BAR0 + bar_num * 4);
    uint32_t bar = pci_config_read32(dev->bus, dev->device, dev->function, offset);
    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_IO)
        return false;
    return (bar & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64;
}

void pci_enable_bus_mastering(const PciDevice *dev)
{
    pci_update_command_bits(dev, PCI_COMMAND_BUS_MASTER, 0);
}

void pci_enable_memory_space(const PciDevice *dev)
{
    pci_update_command_bits(dev, PCI_COMMAND_MEMORY, 0);
}

void pci_enable_io_space(const PciDevice *dev)
{
    pci_update_command_bits(dev, PCI_COMMAND_IO, 0);
}

void pci_enable_interrupts(const PciDevice *dev)
{
    pci_update_command_bits(dev, 0, PCI_COMMAND_INT_DISABLE);
}

void pci_disable_interrupts(const PciDevice *dev)
{
    pci_update_command_bits(dev, PCI_COMMAND_INT_DISABLE, 0);
}

void pci_init()
{
    g_num_ecam_entries = 0;

    AcpiMcfg *mcfg = reinterpret_cast<AcpiMcfg *>(acpi_find_table("MCFG"));
    if (!mcfg) {
        BOOT_LOG("PCI: MCFG not found, using configuration IO ports");
        return;
    }

    if (mcfg->header.length < sizeof(AcpiMcfg)) {
        BOOT_WARN("PCI: malformed MCFG length %u", mcfg->header.length);
        return;
    }

    uint32_t entry_count = (mcfg->header.length - sizeof(AcpiMcfg)) / sizeof(AcpiMcfgEntry);
    for (uint32_t i = 0;
         i < entry_count && g_num_ecam_entries < (int)(sizeof(g_ecam_entries) / sizeof(g_ecam_entries[0])); ++i) {
        AcpiMcfgEntry *entry = &mcfg->entries[i];
        if (entry->end_bus < entry->start_bus) {
            BOOT_WARN("PCI: ignoring malformed MCFG entry %u (bus %u-%u)", i, entry->start_bus, entry->end_bus);
            continue;
        }
        uint64_t bus_count = (uint64_t)entry->end_bus - (uint64_t)entry->start_bus + 1u;
        uint64_t size = bus_count << 20;
        uint64_t base = vmm_map_mmio(entry->base_address, size);
        if (!base) {
            BOOT_WARN("PCI: failed to map ECAM segment for buses %u-%u at 0x%lx", entry->start_bus, entry->end_bus,
                      entry->base_address);
            continue;
        }

        g_ecam_entries[g_num_ecam_entries++] = {base, entry->start_bus, entry->end_bus};
        BOOT_SUCCESS("PCI: ECAM %d initialized at 0x%lx (buses %u-%u)", g_num_ecam_entries - 1, base, entry->start_bus,
                     entry->end_bus);
    }
}
