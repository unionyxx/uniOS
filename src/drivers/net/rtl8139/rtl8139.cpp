#include <drivers/net/rtl8139/rtl8139.h>
#include <drivers/bus/pci/pci.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>
#include <libk/kstring.h>

static RTL8139Device g_rtl8139;

[[nodiscard]] static inline uint8_t rtl_inb(uint16_t reg) { return inb(g_rtl8139.io_base + reg); }
[[nodiscard]] static inline uint16_t rtl_inw(uint16_t reg) { return inw(g_rtl8139.io_base + reg); }
[[nodiscard]] static inline uint32_t rtl_inl(uint16_t reg) { return inl(g_rtl8139.io_base + reg); }
static inline void rtl_outb(uint16_t reg, uint8_t val) { outb(g_rtl8139.io_base + reg, val); }
static inline void rtl_outw(uint16_t reg, uint16_t val) { outw(g_rtl8139.io_base + reg, val); }
static inline void rtl_outl(uint16_t reg, uint32_t val) { outl(g_rtl8139.io_base + reg, val); }

static void rtl8139_read_mac() {
    const uint32_t mac0 = rtl_inl(RTL_REG_MAC0);
    const uint16_t mac4 = rtl_inw(RTL_REG_MAC4);
    g_rtl8139.mac[0] = mac0 & 0xFF; g_rtl8139.mac[1] = (mac0 >> 8) & 0xFF;
    g_rtl8139.mac[2] = (mac0 >> 16) & 0xFF; g_rtl8139.mac[3] = (mac0 >> 24) & 0xFF;
    g_rtl8139.mac[4] = mac4 & 0xFF; g_rtl8139.mac[5] = (mac4 >> 8) & 0xFF;
    DEBUG_INFO("rtl8139: MAC %02x:%02x:%02x:%02x:%02x:%02x", g_rtl8139.mac[0], g_rtl8139.mac[1], g_rtl8139.mac[2], g_rtl8139.mac[3], g_rtl8139.mac[4], g_rtl8139.mac[5]);
}

bool rtl8139_available() { return g_rtl8139.initialized; }

bool rtl8139_init() {
    if (g_rtl8139.initialized) return true;
    DEBUG_INFO("rtl8139: Scanning for Realtek NIC...");
    PciDevice nic; bool found = false;
    for (uint16_t b = 0; b < 256 && !found; b++) {
        for (uint8_t d = 0; d < 32 && !found; d++) {
            if (pci_config_read16(b, d, 0, PCI_VENDOR_ID) != RTL8139_VENDOR_ID) continue;
            const uint8_t max_f = (pci_config_read8(b, d, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
            for (uint8_t f = 0; f < max_f && !found; f++) {
                if (pci_config_read16(b, d, f, PCI_VENDOR_ID) != RTL8139_VENDOR_ID) continue;
                const uint16_t dev_id = pci_config_read16(b, d, f, PCI_DEVICE_ID);
                if (pci_config_read8(b, d, f, PCI_CLASS) == 0x02 && pci_config_read8(b, d, f, PCI_SUBCLASS) == 0x00 && (dev_id == 0x8139 || dev_id == 0x8138 || dev_id == 0x8136)) {
                    nic = { static_cast<uint8_t>(b), d, f, RTL8139_VENDOR_ID, dev_id, 0x02, 0x00, 0, pci_config_read8(b, d, f, PCI_INTERRUPT_LINE) };
                    found = true;
                    DEBUG_INFO("rtl8139: Found NIC %04x:%04x at %d:%d.%d", RTL8139_VENDOR_ID, dev_id, b, d, f);
                }
            }
        }
    }
    if (!found) return false;
    pci_enable_bus_mastering(&nic); pci_enable_io_space(&nic);
    uint64_t bar_sz; uint64_t bar0 = pci_get_bar(&nic, 0, &bar_sz);
    g_rtl8139.io_base = static_cast<uint16_t>(bar0 & 0xFFFFFFFC);
    rtl_outb(RTL_REG_CONFIG1, 0x00); rtl_outb(RTL_REG_CMD, RTL_CMD_RST);
    for (int i = 0; i < 10000; i++) {
        if (!(rtl_inb(RTL_REG_CMD) & RTL_CMD_RST)) break;
        for (volatile int j = 0; j < 1000; j++);
    }
    rtl8139_read_mac();
    void* rx_phys = pmm_alloc_frames(3);
    if (!rx_phys) { DEBUG_ERROR("rtl8139: RX allocation failed"); return false; }
    g_rtl8139.rx_buffer_phys = reinterpret_cast<uintptr_t>(rx_phys);
    g_rtl8139.rx_buffer = reinterpret_cast<uint8_t*>(vmm_phys_to_virt(g_rtl8139.rx_buffer_phys));
    g_rtl8139.rx_offset = 0;
    kstring::zero_memory(g_rtl8139.rx_buffer, RTL_RX_BUFFER_SIZE);
    rtl_outl(RTL_REG_RXBUF, static_cast<uint32_t>(g_rtl8139.rx_buffer_phys));
    for (int i = 0; i < 4; i++) {
        void* tx_phys = pmm_alloc_frame();
        if (!tx_phys) { DEBUG_ERROR("rtl8139: TX %d allocation failed", i); return false; }
        g_rtl8139.tx_buffers_phys[i] = reinterpret_cast<uintptr_t>(tx_phys);
        g_rtl8139.tx_buffers[i] = reinterpret_cast<uint8_t*>(vmm_phys_to_virt(g_rtl8139.tx_buffers_phys[i]));
    }
    g_rtl8139.tx_cur = 0;
    rtl_outw(RTL_REG_IMR, 0x0000);
    rtl_outl(RTL_REG_RCR, RTL_RCR_AB | RTL_RCR_AM | RTL_RCR_APM | RTL_RCR_WRAP);
    rtl_outl(RTL_REG_TCR, 0x03000000);
    rtl_outb(RTL_REG_CMD, RTL_CMD_TE | RTL_CMD_RE);
    g_rtl8139.link_up = true; g_rtl8139.initialized = true;
    DEBUG_INFO("rtl8139: Initialized");
    return true;
}

bool rtl8139_send(const void* data, uint16_t length) {
    if (!g_rtl8139.initialized || !data || length == 0 || length > 1500) return false;
    const uint8_t cur = g_rtl8139.tx_cur;
    int timeout = 10000;
    while (!(rtl_inl(RTL_REG_TXSTATUS0 + cur * 4) & (RTL_TX_OWN | RTL_TX_TOK)) && timeout-- > 0) for (volatile int i = 0; i < 100; i++);
    uint8_t* tx_buf = g_rtl8139.tx_buffers[cur];
    kstring::memcpy(tx_buf, data, length);
    while (length < 60) tx_buf[length++] = 0;
    rtl_outl(RTL_REG_TXADDR0 + cur * 4, static_cast<uint32_t>(g_rtl8139.tx_buffers_phys[cur]));
    rtl_outl(RTL_REG_TXSTATUS0 + cur * 4, length);
    timeout = 10000;
    while (!(rtl_inl(RTL_REG_TXSTATUS0 + cur * 4) & RTL_TX_TOK) && timeout-- > 0) for (volatile int i = 0; i < 100; i++);
    g_rtl8139.tx_cur = (cur + 1) % 4;
    return (rtl_inl(RTL_REG_TXSTATUS0 + cur * 4) & RTL_TX_TOK) != 0;
}

int rtl8139_receive(void* buffer, uint16_t max_length) {
    if (!g_rtl8139.initialized || !buffer || (rtl_inb(RTL_REG_CMD) & RTL_CMD_BUFE)) return 0;
    const uint8_t* pkt = g_rtl8139.rx_buffer + g_rtl8139.rx_offset;
    const uint16_t status = pkt[0] | (pkt[1] << 8);
    const uint16_t length = pkt[2] | (pkt[3] << 8);
    if (length == 0 || length > 1518 || !(status & 0x01)) { g_rtl8139.rx_offset = 0; rtl_outw(RTL_REG_CAPR, 0); return 0; }
    uint16_t dlen = (length - 4 < max_length) ? length - 4 : max_length;
    kstring::memcpy(buffer, pkt + 4, dlen);
    g_rtl8139.rx_offset = (g_rtl8139.rx_offset + length + 4 + 3) & ~3;
    g_rtl8139.rx_offset %= 8192;
    rtl_outw(RTL_REG_CAPR, static_cast<uint16_t>((g_rtl8139.rx_offset + 8192 - 16) % 8192));
    return dlen;
}

void rtl8139_get_mac(uint8_t* out_mac) { if (out_mac) for (int i = 0; i < 6; i++) out_mac[i] = g_rtl8139.mac[i]; }
bool rtl8139_link_up() { return g_rtl8139.initialized && g_rtl8139.link_up; }
void rtl8139_poll() { if (g_rtl8139.initialized) static_cast<void>(rtl_inw(RTL_REG_ISR)); }
