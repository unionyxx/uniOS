#include <drivers/bus/pci/pci.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>

// Build PCI config address for Mechanism 1
static uint32_t pci_make_address(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    return (1u << 31)                    // Enable bit
         | ((uint32_t)bus << 16)
         | ((uint32_t)(device & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | (offset & 0xFC);              // Align to 32-bit
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    return (uint16_t)(inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    return (uint8_t)(inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8));
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    tmp &= ~(0xFFFF << shift);
    tmp |= ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, tmp);
}

void pci_config_write8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t value) {
    outl(PCI_CONFIG_ADDR, pci_make_address(bus, device, func, offset));
    uint32_t tmp = inl(PCI_CONFIG_DATA);
    int shift = (offset & 3) * 8;
    tmp &= ~(0xFF << shift);
    tmp |= ((uint32_t)value << shift);
    outl(PCI_CONFIG_DATA, tmp);
}

// Check if a device exists at given BDF (bus/device/function)
static bool pci_device_exists(uint8_t bus, uint8_t device, uint8_t func) {
    uint16_t vendor = pci_config_read16(bus, device, func, PCI_VENDOR_ID);
    return vendor != 0xFFFF;
}

// Enumerate a function and fill PciDevice structure
static void pci_enum_function(uint8_t bus, uint8_t device, uint8_t func, PciDevice* out) {
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

// Find a device by class/subclass
bool pci_find_device_by_class(uint8_t class_code, uint8_t subclass, PciDevice* out) {
    for (uint16_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            if (!pci_device_exists(bus, dev, 0)) continue;

            uint8_t header_type = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? PCI_MAX_FUNC : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                if (!pci_device_exists(bus, dev, func)) continue;

                uint8_t cls = pci_config_read8(bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8(bus, dev, func, PCI_SUBCLASS);

                if (cls == class_code && sub == subclass) {
                    pci_enum_function(bus, dev, func, out);
                    DEBUG_INFO("Found PCI device %02x:%02x.%x (Class %02x:%02x) Vendor=%04x Device=%04x",
                               bus, dev, func, cls, sub, out->vendor_id, out->device_id);
                    return true;
                }
            }
        }
    }
    return false;
}

// Find a device by class/subclass/prog_if
bool pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, PciDevice* out) {
    for (uint16_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            if (!pci_device_exists(bus, dev, 0)) continue;
            
            uint8_t header_type = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? PCI_MAX_FUNC : 1;
            
            for (uint8_t func = 0; func < max_func; func++) {
                if (!pci_device_exists(bus, dev, func)) continue;
                
                uint8_t cls = pci_config_read8(bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8(bus, dev, func, PCI_SUBCLASS);
                uint8_t pif = pci_config_read8(bus, dev, func, PCI_PROG_IF);
                
                if (cls == class_code && sub == subclass && pif == prog_if) {
                    pci_enum_function(bus, dev, func, out);
                    return true;
                }
            }
        }
    }
    return false;
}

// Find xHCI controller
bool pci_find_xhci(PciDevice* out) {
    return pci_find_device_by_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_XHCI, out);
}

bool pci_find_ac97(PciDevice* out) {
    return pci_find_device_by_class(PCI_CLASS_AUDIO, PCI_SUBCLASS_AC97, out);
}

// Find HD Audio controller.
bool pci_find_hda(PciDevice* out) {
    for (uint16_t bus = 0; bus < PCI_MAX_BUS; bus++) {
        for (uint8_t dev = 0; dev < PCI_MAX_DEVICE; dev++) {
            if (!pci_device_exists(bus, dev, 0)) continue;

            uint8_t header_type = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? PCI_MAX_FUNC : 1;

            for (uint8_t func = 0; func < max_func; func++) {
                if (!pci_device_exists(bus, dev, func)) continue;

                uint8_t cls = pci_config_read8(bus, dev, func, PCI_CLASS);
                uint8_t sub = pci_config_read8(bus, dev, func, PCI_SUBCLASS);
                uint16_t ven = pci_config_read16(bus, dev, func, PCI_VENDOR_ID);

                if (cls == PCI_CLASS_AUDIO && sub == PCI_SUBCLASS_HDA) {
                    // 8086 - Intel
                    // 1022 - AMD
                    // 1106 - VIA

                    // IMPORTANT NOTE: OSDev tells us to use 1002 vendor ID for AMD but only iGPU HD Audio controller has 1002 ID, while Ryzen HD Audio vendor ID always seems to be 1022. Some systems may require to change this.
                    if (ven == 0x8086 || ven == 0x1022 || ven == 0x1106) {
                        pci_enum_function(bus, dev, func, out);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// Get BAR value and optionally size
uint64_t pci_get_bar(const PciDevice* dev, int bar_num, uint64_t* size_out) {
    if (bar_num < 0 || bar_num > 5) return 0;
    
    uint8_t offset = PCI_BAR0 + bar_num * 4;
    uint32_t bar_low = pci_config_read32(dev->bus, dev->device, dev->function, offset);
    
    uint64_t bar_addr;
    bool is_64bit = false;
    
    if ((bar_low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_IO) {
        // I/O space BAR
        bar_addr = bar_low & ~0x3;
    } else {
        // Memory space BAR
        is_64bit = ((bar_low & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64);
        
        if (is_64bit && bar_num < 5) {
            uint32_t bar_high = pci_config_read32(dev->bus, dev->device, dev->function, offset + 4);
            bar_addr = ((uint64_t)bar_high << 32) | (bar_low & ~0xF);
        } else {
            bar_addr = bar_low & ~0xF;
        }
    }
    
    // Calculate size if requested
    if (size_out) {
        // Save original value
        uint32_t orig_low = bar_low;
        uint32_t orig_high = 0;
        if (is_64bit) {
            orig_high = pci_config_read32(dev->bus, dev->device, dev->function, offset + 4);
        }
        
        // Write all 1s to get size
        pci_config_write32(dev->bus, dev->device, dev->function, offset, 0xFFFFFFFF);
        uint32_t size_low = pci_config_read32(dev->bus, dev->device, dev->function, offset);
        
        uint64_t size;
        if (is_64bit) {
            pci_config_write32(dev->bus, dev->device, dev->function, offset + 4, 0xFFFFFFFF);
            uint32_t size_high = pci_config_read32(dev->bus, dev->device, dev->function, offset + 4);
            pci_config_write32(dev->bus, dev->device, dev->function, offset + 4, orig_high);
            
            size = ((uint64_t)size_high << 32) | (size_low & ~0xF);
            size = ~size + 1;
        } else {
            if ((bar_low & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_IO) {
                size = (size_low & ~0x3);
            } else {
                size = (size_low & ~0xF);
            }
            size = ~size + 1;
            size &= 0xFFFFFFFF;
        }
        
        // Restore original value
        pci_config_write32(dev->bus, dev->device, dev->function, offset, orig_low);
        
        *size_out = size;
    }
    
    return bar_addr;
}

bool pci_bar_is_mmio(const PciDevice* dev, int bar_num) {
    if (bar_num < 0 || bar_num > 5) return false;
    uint8_t offset = PCI_BAR0 + bar_num * 4;
    uint32_t bar = pci_config_read32(dev->bus, dev->device, dev->function, offset);
    return (bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_MEM;
}

bool pci_bar_is_64bit(const PciDevice* dev, int bar_num) {
    if (bar_num < 0 || bar_num > 5) return false;
    uint8_t offset = PCI_BAR0 + bar_num * 4;
    uint32_t bar = pci_config_read32(dev->bus, dev->device, dev->function, offset);
    if ((bar & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_IO) return false;
    return (bar & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64;
}

// Enable bus mastering for DMA
void pci_enable_bus_mastering(const PciDevice* dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_BUS_MASTER;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

// Enable memory space access
void pci_enable_memory_space(const PciDevice* dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_MEMORY;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

// Enable I/O space access
void pci_enable_io_space(const PciDevice* dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_IO;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

// Enable PCI interrupts
void pci_enable_interrupts(const PciDevice* dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= ~PCI_COMMAND_INT_DISABLE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

// Disable PCI interrupts
void pci_disable_interrupts(const PciDevice* dev) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_INT_DISABLE;
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

// Initialize PCI subsystem (currently just a placeholder for future enumeration caching)
void pci_init() {
    // No initialization needed for now - enumeration is done on-demand
}
