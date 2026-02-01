#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <drivers/bus/pci/pci.h>

#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11

#define PCI_CAPABILITY_LIST     0x34
#define PCI_STATUS_CAP_LIST     0x10

#define MSIX_CAP_ID             0x00
#define MSIX_CAP_NEXT           0x01
#define MSIX_MSG_CTRL           0x02
#define MSIX_TABLE_OFFSET       0x04
#define MSIX_PBA_OFFSET         0x08

#define MSIX_CTRL_TABLE_SIZE    0x07FF
#define MSIX_CTRL_FUNC_MASK     (1 << 14)
#define MSIX_CTRL_ENABLE        (1 << 15)

#define MSIX_BIR_MASK           0x07
#define MSIX_OFFSET_MASK        0xFFFFFFF8

struct MsixTableEntry {
    uint32_t msg_addr_lo;
    uint32_t msg_addr_hi;
    uint32_t msg_data;
    uint32_t vector_ctrl;
} __attribute__((packed));

#define MSIX_ENTRY_CTRL_MASK    (1 << 0)

#define MSIX_ADDR_BASE          0xFEE00000
#define MSIX_ADDR_DEST_ID(id)   ((uint32_t)(id) << 12)
#define MSIX_ADDR_REDIRECT_HINT (1 << 3)
#define MSIX_ADDR_DEST_LOGICAL  (1 << 2)

#define MSIX_DATA_VECTOR(v)     ((uint32_t)(v) & 0xFF)
#define MSIX_DATA_DELIVERY_FIXED        (0 << 8)
#define MSIX_DATA_DELIVERY_LOWPRI       (1 << 8)
#define MSIX_DATA_DELIVERY_SMI          (2 << 8)
#define MSIX_DATA_DELIVERY_NMI          (4 << 8)
#define MSIX_DATA_DELIVERY_INIT         (5 << 8)
#define MSIX_DATA_DELIVERY_EXTINT       (7 << 8)
#define MSIX_DATA_TRIGGER_EDGE          (0 << 14)
#define MSIX_DATA_TRIGGER_LEVEL         (1 << 14)
#define MSIX_DATA_LEVEL_DEASSERT        (0 << 15)
#define MSIX_DATA_LEVEL_ASSERT          (1 << 15)

static inline uint32_t msix_make_address(uint8_t dest_apic_id) {
    return MSIX_ADDR_BASE | MSIX_ADDR_DEST_ID(dest_apic_id);
}

static inline uint32_t msix_make_data(uint8_t vector, uint8_t delivery_mode) {
    return MSIX_DATA_VECTOR(vector) | (delivery_mode << 8) | MSIX_DATA_TRIGGER_EDGE;
}

#define MSIX_MAX_VECTORS    32

struct MsixState {
    bool enabled;
    uint16_t table_size;
    uint8_t table_bar;
    uint32_t table_offset;
    uint8_t pba_bar;
    uint32_t pba_offset;
    uint64_t table_virt;
    uint8_t cap_offset;
};

uint8_t pci_find_capability(const PciDevice* dev, uint8_t cap_id);
bool pci_has_msix(const PciDevice* dev);
uint16_t pci_msix_table_size(const PciDevice* dev);
bool pci_enable_msix(const PciDevice* dev, MsixState* state);
void pci_disable_msix(const PciDevice* dev, MsixState* state);
void msix_set_entry(MsixState* state, uint32_t index, uint8_t vector, uint8_t dest_apic_id);
void msix_mask_vector(MsixState* state, uint32_t index);
void msix_unmask_vector(MsixState* state, uint32_t index);
void msix_mask_all(const PciDevice* dev, MsixState* state);
void msix_unmask_all(const PciDevice* dev, MsixState* state);
