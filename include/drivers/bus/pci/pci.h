#pragma once
#include <stdint.h>

// PCI Configuration Space ports (Mechanism 1)
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

// PCI Configuration Space offsets
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION_ID     0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE_SIZE 0x0C
#define PCI_LATENCY_TIMER   0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_INTERRUPT_PIN   0x3D

// PCI Command register bits
#define PCI_COMMAND_IO          (1 << 0)
#define PCI_COMMAND_MEMORY      (1 << 1)
#define PCI_COMMAND_BUS_MASTER  (1 << 2)
#define PCI_COMMAND_INT_DISABLE (1 << 10)

// PCI Class codes
#define PCI_CLASS_SERIAL_BUS    0x0C
#define PCI_SUBCLASS_USB        0x03

#define PCI_CLASS_AUDIO         0x04
#define PCI_SUBCLASS_AC97       0x01
#define PCI_SUBCLASS_HDA        0x03

#define PCI_PROGIF_UHCI         0x00
#define PCI_PROGIF_OHCI         0x10
#define PCI_PROGIF_EHCI         0x20
#define PCI_PROGIF_XHCI         0x30

// BAR type detection
#define PCI_BAR_TYPE_MASK       0x01
#define PCI_BAR_TYPE_IO         0x01
#define PCI_BAR_TYPE_MEM        0x00
#define PCI_BAR_MEM_TYPE_MASK   0x06
#define PCI_BAR_MEM_TYPE_32     0x00
#define PCI_BAR_MEM_TYPE_64     0x04
#define PCI_BAR_MEM_PREFETCH    0x08

// Maximum values
#define PCI_MAX_BUS     256
#define PCI_MAX_DEVICE  32
#define PCI_MAX_FUNC    8

// PCI Device structure
struct PciDevice {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t irq_line;
};

// PCI functions
void pci_init();
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);
void pci_config_write16(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint16_t value);
void pci_config_write8(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint8_t value);

// Device discovery
bool pci_find_device_by_class(uint8_t class_code, uint8_t subclass, PciDevice* out);
bool pci_find_device_by_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if, PciDevice* out);
bool pci_find_xhci(PciDevice* out);
bool pci_find_ac97(PciDevice* out);
bool pci_find_hda(PciDevice* out);

// BAR handling
uint64_t pci_get_bar(const PciDevice* dev, int bar_num, uint64_t* size_out);
bool pci_bar_is_mmio(const PciDevice* dev, int bar_num);
bool pci_bar_is_64bit(const PciDevice* dev, int bar_num);

// Device control
void pci_enable_bus_mastering(const PciDevice* dev);
void pci_enable_memory_space(const PciDevice* dev);
void pci_enable_io_space(const PciDevice* dev);
void pci_enable_interrupts(const PciDevice* dev);
void pci_disable_interrupts(const PciDevice* dev);
