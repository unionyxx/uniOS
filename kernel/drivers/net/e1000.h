#pragma once
#include <stdint.h>
#include <stdbool.h>

// Intel e1000/e1000e Vendor ID
#define E1000_VENDOR_ID         0x8086

// Classic e1000 device IDs
#define E1000_DEV_ID_82540EM    0x100E  // QEMU default
#define E1000_DEV_ID_82545EM    0x100F
#define E1000_DEV_ID_82546EB    0x1010
#define E1000_DEV_ID_82541GI    0x1076
#define E1000_DEV_ID_82543GC    0x1004
#define E1000_DEV_ID_82544EI    0x1008
#define E1000_DEV_ID_82574L     0x10D3
#define E1000_DEV_ID_82583V     0x150C

// I210/I211 device IDs
#define E1000_DEV_ID_I210       0x1533
#define E1000_DEV_ID_I211       0x1539

// I217 device IDs (4th/5th Gen Intel)
#define E1000_DEV_ID_I217_LM    0x153A
#define E1000_DEV_ID_I217_V     0x153B

// I218 device IDs
#define E1000_DEV_ID_I218_LM    0x155A
#define E1000_DEV_ID_I218_V     0x1559
#define E1000_DEV_ID_I218_LM2   0x15A0
#define E1000_DEV_ID_I218_V2    0x15A1
#define E1000_DEV_ID_I218_LM3   0x15A2
#define E1000_DEV_ID_I218_V3    0x15A3

// I219 device IDs (6th-12th Gen Intel - many variants!)
#define E1000_DEV_ID_I219_LM    0x156F  // 6th Gen (Skylake)
#define E1000_DEV_ID_I219_V     0x1570  // 6th Gen (Skylake)
#define E1000_DEV_ID_I219_LM2   0x15B7  // 7th Gen (Kaby Lake)
#define E1000_DEV_ID_I219_V2    0x15B8  // 7th Gen (Kaby Lake)
#define E1000_DEV_ID_I219_LM3   0x15BB  // 8th Gen (Coffee Lake)
#define E1000_DEV_ID_I219_V3    0x15BC  // 8th Gen (Coffee Lake)
#define E1000_DEV_ID_I219_LM4   0x15BD  // 9th Gen
#define E1000_DEV_ID_I219_V4    0x15BE  // 9th Gen
#define E1000_DEV_ID_I219_LM5   0x15D7  // Cannon Lake
#define E1000_DEV_ID_I219_V5    0x15D8  // Cannon Lake
#define E1000_DEV_ID_I219_LM6   0x15E3  // 10th Gen (Ice Lake)
#define E1000_DEV_ID_I219_V6    0x15D6  // 10th Gen (Ice Lake)
#define E1000_DEV_ID_I219_LM7   0x0D4C  // Tiger Lake
#define E1000_DEV_ID_I219_V7    0x0D4D  // Tiger Lake
#define E1000_DEV_ID_I219_LM8   0x0D4E  // Tiger Lake
#define E1000_DEV_ID_I219_V8    0x0D4F  // Tiger Lake
#define E1000_DEV_ID_I219_LM9   0x0D53  // Alder Lake
#define E1000_DEV_ID_I219_V9    0x0D55  // Alder Lake
#define E1000_DEV_ID_I219_LM10  0x1A1C  // Raptor Lake
#define E1000_DEV_ID_I219_V10   0x1A1D  // Raptor Lake

// I225/I226 device IDs (2.5GbE)
#define E1000_DEV_ID_I225_LM    0x15F2
#define E1000_DEV_ID_I225_V     0x15F3
#define E1000_DEV_ID_I225_I     0x15F8
#define E1000_DEV_ID_I225_K     0x3100
#define E1000_DEV_ID_I226_LM    0x125B
#define E1000_DEV_ID_I226_V     0x125C

// e1000 Register Offsets
#define E1000_REG_CTRL      0x0000  // Device Control
#define E1000_REG_STATUS    0x0008  // Device Status
#define E1000_REG_EECD      0x0010  // EEPROM Control
#define E1000_REG_EERD      0x0014  // EEPROM Read
#define E1000_REG_ICR       0x00C0  // Interrupt Cause Read
#define E1000_REG_IMS       0x00D0  // Interrupt Mask Set
#define E1000_REG_IMC       0x00D8  // Interrupt Mask Clear
#define E1000_REG_RCTL      0x0100  // Receive Control
#define E1000_REG_TCTL      0x0400  // Transmit Control
#define E1000_REG_TIPG      0x0410  // Transmit IPG
#define E1000_REG_RDBAL     0x2800  // RX Descriptor Base Low
#define E1000_REG_RDBAH     0x2804  // RX Descriptor Base High
#define E1000_REG_RDLEN     0x2808  // RX Descriptor Length
#define E1000_REG_RDH       0x2810  // RX Descriptor Head
#define E1000_REG_RDT       0x2818  // RX Descriptor Tail
#define E1000_REG_TDBAL     0x3800  // TX Descriptor Base Low
#define E1000_REG_TDBAH     0x3804  // TX Descriptor Base High
#define E1000_REG_TDLEN     0x3808  // TX Descriptor Length
#define E1000_REG_TDH       0x3810  // TX Descriptor Head
#define E1000_REG_TDT       0x3818  // TX Descriptor Tail
#define E1000_REG_RAL0      0x5400  // Receive Address Low
#define E1000_REG_RAH0      0x5404  // Receive Address High
#define E1000_REG_MTA       0x5200  // Multicast Table Array

// Control Register bits
#define E1000_CTRL_FD       (1 << 0)   // Full Duplex
#define E1000_CTRL_LRST     (1 << 3)   // Link Reset
#define E1000_CTRL_ASDE     (1 << 5)   // Auto-Speed Detection Enable
#define E1000_CTRL_SLU      (1 << 6)   // Set Link Up
#define E1000_CTRL_RST      (1 << 26)  // Device Reset
#define E1000_CTRL_VME      (1 << 30)  // VLAN Mode Enable
#define E1000_CTRL_PHY_RST  (1 << 31)  // PHY Reset

// Status Register bits
#define E1000_STATUS_FD     (1 << 0)   // Full Duplex
#define E1000_STATUS_LU     (1 << 1)   // Link Up

// RCTL bits
#define E1000_RCTL_EN       (1 << 1)   // Receiver Enable
#define E1000_RCTL_SBP      (1 << 2)   // Store Bad Packets
#define E1000_RCTL_UPE      (1 << 3)   // Unicast Promiscuous
#define E1000_RCTL_MPE      (1 << 4)   // Multicast Promiscuous
#define E1000_RCTL_LPE      (1 << 5)   // Long Packet Enable
#define E1000_RCTL_LBM_NONE (0 << 6)   // Loopback Mode: None
#define E1000_RCTL_BAM      (1 << 15)  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 (0 << 16) // Buffer Size 2048
#define E1000_RCTL_BSIZE_4096 ((3 << 16) | (1 << 25)) // Buffer Size 4096
#define E1000_RCTL_SECRC    (1 << 26)  // Strip Ethernet CRC

// TCTL bits
#define E1000_TCTL_EN       (1 << 1)   // Transmitter Enable
#define E1000_TCTL_PSP      (1 << 3)   // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4          // Collision Threshold
#define E1000_TCTL_COLD_SHIFT 12       // Collision Distance

// TX Descriptor Command bits
#define E1000_TXD_CMD_EOP   (1 << 0)   // End of Packet
#define E1000_TXD_CMD_IFCS  (1 << 1)   // Insert FCS
#define E1000_TXD_CMD_RS    (1 << 3)   // Report Status

// TX Descriptor Status bits
#define E1000_TXD_STAT_DD   (1 << 0)   // Descriptor Done

// RX Descriptor Status bits
#define E1000_RXD_STAT_DD   (1 << 0)   // Descriptor Done
#define E1000_RXD_STAT_EOP  (1 << 1)   // End of Packet

// EEPROM
#define E1000_EERD_START    (1 << 0)
#define E1000_EERD_DONE     (1 << 4)
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16

// Descriptor counts (must be multiple of 8, max 65536)
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32
#define E1000_RX_BUFFER_SIZE 2048

// TX Descriptor (Legacy)
struct e1000_tx_desc {
    uint64_t addr;      // Buffer address
    uint16_t length;    // Data length
    uint8_t cso;        // Checksum offset
    uint8_t cmd;        // Command
    uint8_t status;     // Status
    uint8_t css;        // Checksum start
    uint16_t special;   // Special field
} __attribute__((packed));

// RX Descriptor (Legacy)
struct e1000_rx_desc {
    uint64_t addr;      // Buffer address
    uint16_t length;    // Received length
    uint16_t checksum;  // Packet checksum
    uint8_t status;     // Status
    uint8_t errors;     // Errors
    uint16_t special;   // Special field
} __attribute__((packed));

// e1000 device structure
struct E1000Device {
    uint64_t mmio_base;             // MMIO base address (virtual)
    uint8_t mac[6];                 // MAC address
    
    e1000_rx_desc* rx_descs;        // RX descriptor ring
    e1000_tx_desc* tx_descs;        // TX descriptor ring
    uint64_t rx_descs_phys;         // Physical address of RX ring
    uint64_t tx_descs_phys;         // Physical address of TX ring
    
    uint8_t* rx_buffers[E1000_NUM_RX_DESC];  // RX packet buffers
    uint64_t rx_buffers_phys[E1000_NUM_RX_DESC];
    
    uint32_t rx_cur;                // Current RX descriptor
    uint32_t tx_cur;                // Current TX descriptor
    
    bool link_up;                   // Link status
    bool initialized;               // Device initialized
};

// Public API
bool e1000_init();
bool e1000_send(const void* data, uint16_t length);
int e1000_receive(void* buffer, uint16_t max_length);
void e1000_get_mac(uint8_t* out_mac);
bool e1000_link_up();
void e1000_poll();

// Get the device (for internal use)
E1000Device* e1000_get_device();
