#include "rtl8139.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "io.h"
#include "debug.h"

// Global RTL8139 device
static RTL8139Device g_rtl8139;

// I/O helpers
static inline uint8_t rtl_inb(uint16_t reg) {
    return inb(g_rtl8139.io_base + reg);
}

static inline uint16_t rtl_inw(uint16_t reg) {
    return inw(g_rtl8139.io_base + reg);
}

static inline uint32_t rtl_inl(uint16_t reg) {
    return inl(g_rtl8139.io_base + reg);
}

static inline void rtl_outb(uint16_t reg, uint8_t val) {
    outb(g_rtl8139.io_base + reg, val);
}

static inline void rtl_outw(uint16_t reg, uint16_t val) {
    outw(g_rtl8139.io_base + reg, val);
}

static inline void rtl_outl(uint16_t reg, uint32_t val) {
    outl(g_rtl8139.io_base + reg, val);
}

// Read MAC address
static void rtl8139_read_mac() {
    uint32_t mac0 = rtl_inl(RTL_REG_MAC0);
    uint16_t mac4 = rtl_inw(RTL_REG_MAC4);
    
    g_rtl8139.mac[0] = mac0 & 0xFF;
    g_rtl8139.mac[1] = (mac0 >> 8) & 0xFF;
    g_rtl8139.mac[2] = (mac0 >> 16) & 0xFF;
    g_rtl8139.mac[3] = (mac0 >> 24) & 0xFF;
    g_rtl8139.mac[4] = mac4 & 0xFF;
    g_rtl8139.mac[5] = (mac4 >> 8) & 0xFF;
    
    DEBUG_INFO("rtl8139: MAC: %02x:%02x:%02x:%02x:%02x:%02x",
        g_rtl8139.mac[0], g_rtl8139.mac[1], g_rtl8139.mac[2],
        g_rtl8139.mac[3], g_rtl8139.mac[4], g_rtl8139.mac[5]);
}

bool rtl8139_available() {
    return g_rtl8139.initialized;
}

bool rtl8139_init() {
    if (g_rtl8139.initialized) {
        return true;
    }
    
    DEBUG_INFO("rtl8139: Scanning for Realtek NIC...");
    
    // Scan PCI for RTL8139
    PciDevice nic;
    bool found = false;
    
    for (uint8_t bus = 0; bus < 8 && !found; bus++) {
        for (uint8_t dev = 0; dev < 32 && !found; dev++) {
            uint16_t vendor = pci_config_read16(bus, dev, 0, PCI_VENDOR_ID);
            if (vendor != RTL8139_VENDOR_ID) continue;
            
            uint8_t header_type = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;
            
            for (uint8_t func = 0; func < max_func && !found; func++) {
                vendor = pci_config_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vendor != RTL8139_VENDOR_ID) continue;
                
                uint16_t device_id = pci_config_read16(bus, dev, func, PCI_DEVICE_ID);
                uint8_t class_code = pci_config_read8(bus, dev, func, PCI_CLASS);
                uint8_t subclass = pci_config_read8(bus, dev, func, PCI_SUBCLASS);
                
                // Check for RTL8139 or compatible (class 02:00 = network controller)
                if (class_code == 0x02 && subclass == 0x00 &&
                    (device_id == 0x8139 || device_id == 0x8138 || device_id == 0x8136)) {
                    
                    nic.bus = bus;
                    nic.device = dev;
                    nic.function = func;
                    nic.vendor_id = vendor;
                    nic.device_id = device_id;
                    nic.irq_line = pci_config_read8(bus, dev, func, PCI_INTERRUPT_LINE);
                    found = true;
                    
                    DEBUG_INFO("rtl8139: Found Realtek NIC %04x:%04x at %d:%d.%d",
                        vendor, device_id, bus, dev, func);
                }
            }
        }
    }
    
    if (!found) {
        DEBUG_INFO("rtl8139: No Realtek NIC found");
        return false;
    }
    
    // Enable bus mastering and I/O space
    pci_enable_bus_mastering(&nic);
    pci_enable_io_space(&nic);
    
    // Get BAR0 (I/O port)
    uint64_t bar_size;
    uint64_t bar0 = pci_get_bar(&nic, 0, &bar_size);
    
    // RTL8139 uses I/O ports
    g_rtl8139.io_base = bar0 & 0xFFFFFFFC;
    DEBUG_INFO("rtl8139: I/O base at 0x%x", g_rtl8139.io_base);
    
    // Power on
    rtl_outb(RTL_REG_CONFIG1, 0x00);
    
    // Software reset
    rtl_outb(RTL_REG_CMD, RTL_CMD_RST);
    
    // Wait for reset to complete
    for (int i = 0; i < 10000; i++) {
        if (!(rtl_inb(RTL_REG_CMD) & RTL_CMD_RST)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    
    // Read MAC address
    rtl8139_read_mac();
    
    // Allocate RX buffer (8K + 16 bytes header + 1500 for wrap)
    void* rx_phys = pmm_alloc_frames(3);  // 12K
    if (!rx_phys) {
        DEBUG_ERROR("rtl8139: Failed to allocate RX buffer");
        return false;
    }
    g_rtl8139.rx_buffer_phys = (uint64_t)rx_phys;
    g_rtl8139.rx_buffer = (uint8_t*)vmm_phys_to_virt((uint64_t)rx_phys);
    g_rtl8139.rx_offset = 0;
    
    // Clear RX buffer
    for (int i = 0; i < RTL_RX_BUFFER_SIZE; i++) {
        g_rtl8139.rx_buffer[i] = 0;
    }
    
    // Set RX buffer address
    rtl_outl(RTL_REG_RXBUF, (uint32_t)g_rtl8139.rx_buffer_phys);
    
    // Allocate TX buffers
    for (int i = 0; i < 4; i++) {
        void* tx_phys = pmm_alloc_frame();
        if (!tx_phys) {
            DEBUG_ERROR("rtl8139: Failed to allocate TX buffer %d", i);
            return false;
        }
        g_rtl8139.tx_buffers_phys[i] = (uint64_t)tx_phys;
        g_rtl8139.tx_buffers[i] = (uint8_t*)vmm_phys_to_virt((uint64_t)tx_phys);
    }
    g_rtl8139.tx_cur = 0;
    
    // Disable all interrupts (we use polling)
    rtl_outw(RTL_REG_IMR, 0x0000);
    
    // Configure RX: Accept broadcast + matching MAC + wrap buffer
    rtl_outl(RTL_REG_RCR, RTL_RCR_AB | RTL_RCR_AM | RTL_RCR_APM | RTL_RCR_WRAP);
    
    // Configure TX
    rtl_outl(RTL_REG_TCR, 0x03000000);  // Default DMA burst, IFG
    
    // Enable RX and TX
    rtl_outb(RTL_REG_CMD, RTL_CMD_TE | RTL_CMD_RE);
    
    // Check link status
    g_rtl8139.link_up = true;  // RTL8139 doesn't have easy link status check
    
    g_rtl8139.initialized = true;
    DEBUG_INFO("rtl8139: Initialization complete");
    return true;
}

bool rtl8139_send(const void* data, uint16_t length) {
    if (!g_rtl8139.initialized || !data || length == 0 || length > 1500) {
        return false;
    }
    
    uint8_t cur = g_rtl8139.tx_cur;
    
    // Wait for previous TX to complete
    int timeout = 10000;
    while (!(rtl_inl(RTL_REG_TXSTATUS0 + cur * 4) & (RTL_TX_OWN | RTL_TX_TOK)) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }
    
    // Copy data to TX buffer
    uint8_t* tx_buf = g_rtl8139.tx_buffers[cur];
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < length; i++) {
        tx_buf[i] = src[i];
    }
    
    // Pad to minimum 60 bytes
    while (length < 60) {
        tx_buf[length++] = 0;
    }
    
    // Set TX address
    rtl_outl(RTL_REG_TXADDR0 + cur * 4, (uint32_t)g_rtl8139.tx_buffers_phys[cur]);
    
    // Start transmission (length in bits 0-12, clear OWN bit)
    rtl_outl(RTL_REG_TXSTATUS0 + cur * 4, length);
    
    // Wait for completion
    timeout = 10000;
    while (!(rtl_inl(RTL_REG_TXSTATUS0 + cur * 4) & RTL_TX_TOK) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }
    
    // Move to next descriptor
    g_rtl8139.tx_cur = (cur + 1) % 4;
    
    return (rtl_inl(RTL_REG_TXSTATUS0 + cur * 4) & RTL_TX_TOK) != 0;
}

int rtl8139_receive(void* buffer, uint16_t max_length) {
    if (!g_rtl8139.initialized || !buffer) {
        return 0;
    }
    
    // Check if buffer is empty
    if (rtl_inb(RTL_REG_CMD) & RTL_CMD_BUFE) {
        return 0;
    }
    
    // Read packet header (4 bytes)
    uint8_t* pkt = g_rtl8139.rx_buffer + g_rtl8139.rx_offset;
    uint16_t status = pkt[0] | (pkt[1] << 8);
    uint16_t length = pkt[2] | (pkt[3] << 8);
    
    // Validate
    if (length == 0 || length > 1518 || !(status & 0x01)) {
        // Reset RX
        g_rtl8139.rx_offset = 0;
        rtl_outw(RTL_REG_CAPR, 0);
        return 0;
    }
    
    // Actual data length (minus 4 byte CRC)
    uint16_t data_len = length - 4;
    if (data_len > max_length) {
        data_len = max_length;
    }
    
    // Copy data (after 4+byte header)
    uint8_t* dst = (uint8_t*)buffer;
    uint8_t* src = pkt + 4;
    for (uint16_t i = 0; i < data_len; i++) {
        dst[i] = src[i];
    }
    
    // Update offset (4-byte aligned, wrap around buffer)
    g_rtl8139.rx_offset = (g_rtl8139.rx_offset + length + 4 + 3) & ~3;
    g_rtl8139.rx_offset %= 8192;
    
    // Update CAPR
    rtl_outw(RTL_REG_CAPR, g_rtl8139.rx_offset - 16);
    
    return data_len;
}

void rtl8139_get_mac(uint8_t* out_mac) {
    if (!out_mac) return;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_rtl8139.mac[i];
    }
}

bool rtl8139_link_up() {
    return g_rtl8139.initialized && g_rtl8139.link_up;
}

void rtl8139_poll() {
    if (!g_rtl8139.initialized) return;
    
    // Clear any pending interrupts
    rtl_inw(RTL_REG_ISR);
}
