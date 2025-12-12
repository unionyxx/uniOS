#pragma once
#include <stdint.h>
#include <stdbool.h>

// Realtek Vendor ID
#define RTL8139_VENDOR_ID       0x10EC
#define RTL8139_DEVICE_ID       0x8139

// RTL8139 Register Offsets
#define RTL_REG_MAC0            0x00    // MAC address bytes 0-3
#define RTL_REG_MAC4            0x04    // MAC address bytes 4-5
#define RTL_REG_TXSTATUS0       0x10    // TX status (4 registers, 4 bytes each)
#define RTL_REG_TXADDR0         0x20    // TX address (4 registers, 4 bytes each)
#define RTL_REG_RXBUF           0x30    // RX buffer start address
#define RTL_REG_CMD             0x37    // Command register
#define RTL_REG_CAPR            0x38    // Current Address of Packet Read
#define RTL_REG_CBR             0x3A    // Current Buffer Address
#define RTL_REG_IMR             0x3C    // Interrupt Mask Register
#define RTL_REG_ISR             0x3E    // Interrupt Status Register
#define RTL_REG_TCR             0x40    // Transmit Configuration Register
#define RTL_REG_RCR             0x44    // Receive Configuration Register
#define RTL_REG_CONFIG1         0x52    // Configuration Register 1

// Command Register bits
#define RTL_CMD_BUFE            0x01    // Buffer Empty
#define RTL_CMD_TE              0x04    // Transmitter Enable
#define RTL_CMD_RE              0x08    // Receiver Enable
#define RTL_CMD_RST             0x10    // Reset

// RCR bits
#define RTL_RCR_AAP             0x01    // Accept All Packets
#define RTL_RCR_APM             0x02    // Accept Physical Match
#define RTL_RCR_AM              0x04    // Accept Multicast
#define RTL_RCR_AB              0x08    // Accept Broadcast
#define RTL_RCR_WRAP            0x80    // Wrap buffer

// TX Status bits
#define RTL_TX_OWN              0x2000  // DMA completed
#define RTL_TX_TOK              0x8000  // Transmit OK

// Buffer sizes
#define RTL_RX_BUFFER_SIZE      (8192 + 16 + 1500)  // 8K + header + max packet
#define RTL_TX_BUFFER_SIZE      1536

// RTL8139 device structure
struct RTL8139Device {
    uint32_t io_base;               // I/O base address
    uint8_t mac[6];                 // MAC address
    
    uint8_t* rx_buffer;             // RX ring buffer
    uint64_t rx_buffer_phys;        // Physical address
    uint32_t rx_offset;             // Current position in RX buffer
    
    uint8_t* tx_buffers[4];         // TX buffers (4 descriptors)
    uint64_t tx_buffers_phys[4];
    uint8_t tx_cur;                 // Current TX descriptor
    
    bool link_up;
    bool initialized;
};

// Public API
bool rtl8139_init();
bool rtl8139_send(const void* data, uint16_t length);
int rtl8139_receive(void* buffer, uint16_t max_length);
void rtl8139_get_mac(uint8_t* out_mac);
bool rtl8139_link_up();
void rtl8139_poll();

// Check if RTL8139 is available
bool rtl8139_available();
