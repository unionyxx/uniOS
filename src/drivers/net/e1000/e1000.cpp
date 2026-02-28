#include <drivers/net/e1000/e1000.h>
#include <drivers/bus/pci/pci.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>

static E1000Device g_e1000;

[[nodiscard]] static inline uint32_t e1000_read_reg(uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t*>(g_e1000.mmio_base + reg);
}

static inline void e1000_write_reg(uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t*>(g_e1000.mmio_base + reg) = value;
    asm volatile("mfence" ::: "memory");
}

[[nodiscard]] static uint16_t e1000_eeprom_read(uint8_t addr) {
    e1000_write_reg(E1000_REG_EERD, (static_cast<uint32_t>(addr) << E1000_EERD_ADDR_SHIFT) | E1000_EERD_START);
    for (int i = 0; i < 10000; i++) {
        const uint32_t val = e1000_read_reg(E1000_REG_EERD);
        if (val & E1000_EERD_DONE) return static_cast<uint16_t>(val >> E1000_EERD_DATA_SHIFT);
        for (volatile int j = 0; j < 100; j++);
    }
    DEBUG_WARN("e1000: EEPROM read timeout at %d", addr);
    return 0;
}

static void e1000_read_mac() {
    const uint32_t ral = e1000_read_reg(E1000_REG_RAL0);
    const uint32_t rah = e1000_read_reg(E1000_REG_RAH0);
    
    if (ral != 0 || (rah & 0xFFFF) != 0) {
        g_e1000.mac[0] = ral & 0xFF;
        g_e1000.mac[1] = (ral >> 8) & 0xFF;
        g_e1000.mac[2] = (ral >> 16) & 0xFF;
        g_e1000.mac[3] = (ral >> 24) & 0xFF;
        g_e1000.mac[4] = rah & 0xFF;
        g_e1000.mac[5] = (rah >> 8) & 0xFF;
        KLOG(LogModule::Net, LogLevel::Trace, "e1000: MAC from RAL/RAH: %02x:%02x:%02x:%02x:%02x:%02x", g_e1000.mac[0], g_e1000.mac[1], g_e1000.mac[2], g_e1000.mac[3], g_e1000.mac[4], g_e1000.mac[5]);
        return;
    }
    
    const uint16_t w0 = e1000_eeprom_read(0), w1 = e1000_eeprom_read(1), w2 = e1000_eeprom_read(2);
    g_e1000.mac[0] = w0 & 0xFF; g_e1000.mac[1] = (w0 >> 8) & 0xFF;
    g_e1000.mac[2] = w1 & 0xFF; g_e1000.mac[3] = (w1 >> 8) & 0xFF;
    g_e1000.mac[4] = w2 & 0xFF; g_e1000.mac[5] = (w2 >> 8) & 0xFF;
    
    KLOG(LogModule::Net, LogLevel::Trace, "e1000: MAC from EEPROM: %02x:%02x:%02x:%02x:%02x:%02x", g_e1000.mac[0], g_e1000.mac[1], g_e1000.mac[2], g_e1000.mac[3], g_e1000.mac[4], g_e1000.mac[5]);
    e1000_write_reg(E1000_REG_RAL0, static_cast<uint32_t>(g_e1000.mac[0]) | (static_cast<uint32_t>(g_e1000.mac[1]) << 8) | (static_cast<uint32_t>(g_e1000.mac[2]) << 16) | (static_cast<uint32_t>(g_e1000.mac[3]) << 24));
    e1000_write_reg(E1000_REG_RAH0, static_cast<uint32_t>(g_e1000.mac[4]) | (static_cast<uint32_t>(g_e1000.mac[5]) << 8) | (1u << 31));
}

[[nodiscard]] static bool e1000_init_rx() {
    void* rx_ring_phys = pmm_alloc_frame();
    if (!rx_ring_phys) { DEBUG_ERROR("e1000: Failed to allocate RX ring"); return false; }
    
    g_e1000.rx_descs_phys = reinterpret_cast<uintptr_t>(rx_ring_phys);
    g_e1000.rx_descs = reinterpret_cast<e1000_rx_desc*>(vmm_phys_to_virt(g_e1000.rx_descs_phys));
    kstring::zero_memory(g_e1000.rx_descs, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc));
    
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        void* buf_phys = pmm_alloc_frame();
        if (!buf_phys) { DEBUG_ERROR("e1000: Failed to allocate RX buffer %d", i); return false; }
        g_e1000.rx_buffers_phys[i] = reinterpret_cast<uintptr_t>(buf_phys);
        g_e1000.rx_buffers[i] = reinterpret_cast<uint8_t*>(vmm_phys_to_virt(g_e1000.rx_buffers_phys[i]));
        g_e1000.rx_descs[i].addr = g_e1000.rx_buffers_phys[i];
    }
    
    e1000_write_reg(E1000_REG_RDBAL, static_cast<uint32_t>(g_e1000.rx_descs_phys & 0xFFFFFFFF));
    e1000_write_reg(E1000_REG_RDBAH, static_cast<uint32_t>(g_e1000.rx_descs_phys >> 32));
    e1000_write_reg(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc));
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    g_e1000.rx_cur = 0;
    e1000_write_reg(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC | E1000_RCTL_LBM_NONE);
    KLOG(LogModule::Net, LogLevel::Trace, "e1000: RX initialized with %d descriptors", E1000_NUM_RX_DESC);
    return true;
}

[[nodiscard]] static bool e1000_init_tx() {
    void* tx_ring_phys = pmm_alloc_frame();
    if (!tx_ring_phys) { DEBUG_ERROR("e1000: Failed to allocate TX ring"); return false; }
    
    g_e1000.tx_descs_phys = reinterpret_cast<uintptr_t>(tx_ring_phys);
    g_e1000.tx_descs = reinterpret_cast<e1000_tx_desc*>(vmm_phys_to_virt(g_e1000.tx_descs_phys));
    kstring::zero_memory(g_e1000.tx_descs, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc));
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) g_e1000.tx_descs[i].status = E1000_TXD_STAT_DD;
    
    e1000_write_reg(E1000_REG_TDBAL, static_cast<uint32_t>(g_e1000.tx_descs_phys & 0xFFFFFFFF));
    e1000_write_reg(E1000_REG_TDBAH, static_cast<uint32_t>(g_e1000.tx_descs_phys >> 32));
    e1000_write_reg(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc));
    e1000_write_reg(E1000_REG_TDH, 0);
    e1000_write_reg(E1000_REG_TDT, 0);
    g_e1000.tx_cur = 0;
    e1000_write_reg(E1000_REG_TIPG, (10 << 0) | (10 << 10) | (10 << 20));
    e1000_write_reg(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (15 << E1000_TCTL_CT_SHIFT) | (64 << E1000_TCTL_COLD_SHIFT));
    KLOG(LogModule::Net, LogLevel::Trace, "e1000: TX initialized with %d descriptors", E1000_NUM_TX_DESC);
    return true;
}

bool e1000_init() {
    if (g_e1000.initialized) return true;
    KLOG(LogModule::Net, LogLevel::Trace, "e1000: Scanning for Intel NIC...");
    PciDevice nic; bool found = false;
    for (uint16_t b = 0; b < 256 && !found; b++) {
        for (uint8_t d = 0; d < 32 && !found; d++) {
            if (pci_config_read16(b, d, 0, PCI_VENDOR_ID) == 0xFFFF) continue;
            const uint8_t max_f = (pci_config_read8(b, d, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
            for (uint8_t f = 0; f < max_f && !found; f++) {
                if (pci_config_read16(b, d, f, PCI_VENDOR_ID) != E1000_VENDOR_ID) continue;
                if (pci_config_read8(b, d, f, PCI_CLASS) == 0x02 && pci_config_read8(b, d, f, PCI_SUBCLASS) == 0x00) {
                    nic = { static_cast<uint8_t>(b), d, f, E1000_VENDOR_ID, pci_config_read16(b, d, f, PCI_DEVICE_ID), 0x02, 0x00, 0, pci_config_read8(b, d, f, PCI_INTERRUPT_LINE) };
                    found = true;
                    DEBUG_INFO("e1000: Found Intel NIC %04x:%04x at %d:%d.%d", nic.vendor_id, nic.device_id, b, d, f);
                }
            }
        }
    }
    if (!found) { DEBUG_WARN("e1000: No Intel NIC found"); return false; }
    pci_enable_bus_mastering(&nic); pci_enable_memory_space(&nic);
    uint64_t bar_sz; uint64_t bar0 = pci_get_bar(&nic, 0, &bar_sz);
    if (!pci_bar_is_mmio(&nic, 0)) { DEBUG_ERROR("e1000: BAR0 is not MMIO!"); return false; }
    g_e1000.mmio_base = vmm_map_mmio(bar0, bar_sz);
    if (!g_e1000.mmio_base) { DEBUG_ERROR("e1000: Failed to map MMIO"); return false; }
    KLOG(LogModule::Net, LogLevel::Trace, "e1000: MMIO base 0x%lx (phys 0x%lx), size %lu", g_e1000.mmio_base, bar0, bar_sz);
    e1000_write_reg(E1000_REG_CTRL, e1000_read_reg(E1000_REG_CTRL) | E1000_CTRL_RST);
    for (int i = 0; i < 100; i++) {
        for (volatile int j = 0; j < 10000; j++);
        if (!(e1000_read_reg(E1000_REG_CTRL) & E1000_CTRL_RST)) break;
    }
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFF); static_cast<void>(e1000_read_reg(E1000_REG_ICR));
    for (int i = 0; i < 128; i++) e1000_write_reg(E1000_REG_MTA + (i * 4), 0);
    e1000_read_mac();
    if (!e1000_init_rx() || !e1000_init_tx()) return false;
    e1000_write_reg(E1000_REG_CTRL, e1000_read_reg(E1000_REG_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);
    for (int i = 0; i < 100; i++) {
        for (volatile int j = 0; j < 50000; j++);
        if (e1000_read_reg(E1000_REG_STATUS) & E1000_STATUS_LU) { g_e1000.link_up = true; DEBUG_INFO("e1000: Link UP"); break; }
    }
    if (!g_e1000.link_up) DEBUG_WARN("e1000: Link DOWN");
    g_e1000.initialized = true; DEBUG_INFO("e1000: Initialized");
    return true;
}

bool e1000_send(const void* data, uint16_t length) {
    if (!g_e1000.initialized || !data || length == 0 || length > 1500) return false;
    e1000_tx_desc* desc = &g_e1000.tx_descs[g_e1000.tx_cur];
    int timeout = 10000;
    while (!(desc->status & E1000_TXD_STAT_DD) && timeout-- > 0) for (volatile int i = 0; i < 100; i++);
    if (timeout <= 0) { DEBUG_WARN("e1000: TX timeout"); return false; }
    void* buf_phys = pmm_alloc_frame();
    if (!buf_phys) { DEBUG_ERROR("e1000: No TX buf memory"); return false; }
    kstring::memcpy(reinterpret_cast<void*>(vmm_phys_to_virt(reinterpret_cast<uintptr_t>(buf_phys))), data, length);
    desc->addr = reinterpret_cast<uintptr_t>(buf_phys);
    desc->length = length; desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS; desc->status = 0;
    g_e1000.tx_cur = (g_e1000.tx_cur + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(E1000_REG_TDT, g_e1000.tx_cur);
    timeout = 10000;
    while (!(desc->status & E1000_TXD_STAT_DD) && timeout-- > 0) for (volatile int i = 0; i < 100; i++);
    pmm_free_frame(buf_phys);
    return (desc->status & E1000_TXD_STAT_DD) != 0;
}

int e1000_receive(void* buffer, uint16_t max_length) {
    if (!g_e1000.initialized || !buffer) return 0;
    e1000_rx_desc* desc = &g_e1000.rx_descs[g_e1000.rx_cur];
    if (!(desc->status & E1000_RXD_STAT_DD)) return 0;
    if (desc->errors) {
        DEBUG_WARN("e1000: RX error 0x%02x", desc->errors);
        desc->status = 0; g_e1000.rx_cur = (g_e1000.rx_cur + 1) % E1000_NUM_RX_DESC;
        e1000_write_reg(E1000_REG_RDT, (g_e1000.rx_cur + E1000_NUM_RX_DESC - 1) % E1000_NUM_RX_DESC);
        return 0;
    }
    uint16_t len = (desc->length < max_length) ? desc->length : max_length;
    kstring::memcpy(buffer, g_e1000.rx_buffers[g_e1000.rx_cur], len);
    desc->status = 0;
    const uint32_t old_cur = g_e1000.rx_cur;
    g_e1000.rx_cur = (g_e1000.rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write_reg(E1000_REG_RDT, old_cur);
    return len;
}

void e1000_get_mac(uint8_t* out_mac) {
    if (out_mac) for (int i = 0; i < 6; i++) out_mac[i] = g_e1000.mac[i];
}

bool e1000_link_up() {
    if (!g_e1000.initialized) return false;
    g_e1000.link_up = (e1000_read_reg(E1000_REG_STATUS) & E1000_STATUS_LU) != 0;
    return g_e1000.link_up;
}

void e1000_poll() { if (g_e1000.initialized) static_cast<void>(e1000_read_reg(E1000_REG_ICR)); }
E1000Device* e1000_get_device() { return &g_e1000; }
