#include <drivers/net/e1000/e1000.h>
#include <drivers/bus/pci/pci.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>
#include <kernel/mm/heap.h>

// Global e1000 device
static E1000Device g_e1000;

// Helper to read/write MMIO registers
static inline uint32_t e1000_read_reg(uint32_t reg) {
    return *(volatile uint32_t*)(g_e1000.mmio_base + reg);
}

static inline void e1000_write_reg(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(g_e1000.mmio_base + reg) = value;
    asm volatile("mfence" ::: "memory");
}

// Read word from EEPROM
static uint16_t e1000_eeprom_read(uint8_t addr) {
    uint32_t val;
    
    // Start EEPROM read
    e1000_write_reg(E1000_REG_EERD, ((uint32_t)addr << E1000_EERD_ADDR_SHIFT) | E1000_EERD_START);
    
    // Wait for done (with timeout)
    for (int i = 0; i < 10000; i++) {
        val = e1000_read_reg(E1000_REG_EERD);
        if (val & E1000_EERD_DONE) {
            return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
        }
        for (volatile int j = 0; j < 100; j++);
    }
    
    DEBUG_WARN("e1000: EEPROM read timeout for addr %d", addr);
    return 0;
}

// Read MAC address from EEPROM or RAL/RAH
static void e1000_read_mac() {
    // Try reading from RAL0/RAH0 first (might be set by firmware)
    uint32_t ral = e1000_read_reg(E1000_REG_RAL0);
    uint32_t rah = e1000_read_reg(E1000_REG_RAH0);
    
    if (ral != 0 || (rah & 0xFFFF) != 0) {
        g_e1000.mac[0] = ral & 0xFF;
        g_e1000.mac[1] = (ral >> 8) & 0xFF;
        g_e1000.mac[2] = (ral >> 16) & 0xFF;
        g_e1000.mac[3] = (ral >> 24) & 0xFF;
        g_e1000.mac[4] = rah & 0xFF;
        g_e1000.mac[5] = (rah >> 8) & 0xFF;
        KLOG(MOD_NET, LOG_TRACE, "e1000: MAC from RAL/RAH: %02x:%02x:%02x:%02x:%02x:%02x",
            g_e1000.mac[0], g_e1000.mac[1], g_e1000.mac[2],
            g_e1000.mac[3], g_e1000.mac[4], g_e1000.mac[5]);
        return;
    }
    
    // Try EEPROM
    uint16_t word0 = e1000_eeprom_read(0);
    uint16_t word1 = e1000_eeprom_read(1);
    uint16_t word2 = e1000_eeprom_read(2);
    
    g_e1000.mac[0] = word0 & 0xFF;
    g_e1000.mac[1] = (word0 >> 8) & 0xFF;
    g_e1000.mac[2] = word1 & 0xFF;
    g_e1000.mac[3] = (word1 >> 8) & 0xFF;
    g_e1000.mac[4] = word2 & 0xFF;
    g_e1000.mac[5] = (word2 >> 8) & 0xFF;
    
    KLOG(MOD_NET, LOG_TRACE, "e1000: MAC from EEPROM: %02x:%02x:%02x:%02x:%02x:%02x",
        g_e1000.mac[0], g_e1000.mac[1], g_e1000.mac[2],
        g_e1000.mac[3], g_e1000.mac[4], g_e1000.mac[5]);
    
    // Write MAC to RAL0/RAH0
    e1000_write_reg(E1000_REG_RAL0, 
        (uint32_t)g_e1000.mac[0] | ((uint32_t)g_e1000.mac[1] << 8) |
        ((uint32_t)g_e1000.mac[2] << 16) | ((uint32_t)g_e1000.mac[3] << 24));
    e1000_write_reg(E1000_REG_RAH0,
        (uint32_t)g_e1000.mac[4] | ((uint32_t)g_e1000.mac[5] << 8) | (1 << 31)); // AV bit
}

// Initialize RX descriptors
static bool e1000_init_rx() {
    // Allocate descriptor ring (aligned to 16 bytes, we use page alignment)
    void* rx_ring_phys = pmm_alloc_frame();
    if (!rx_ring_phys) {
        DEBUG_ERROR("e1000: Failed to allocate RX descriptor ring");
        return false;
    }
    
    g_e1000.rx_descs_phys = (uint64_t)rx_ring_phys;
    g_e1000.rx_descs = (e1000_rx_desc*)vmm_phys_to_virt(g_e1000.rx_descs_phys);
    
    // Clear descriptors
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        g_e1000.rx_descs[i].addr = 0;
        g_e1000.rx_descs[i].length = 0;
        g_e1000.rx_descs[i].checksum = 0;
        g_e1000.rx_descs[i].status = 0;
        g_e1000.rx_descs[i].errors = 0;
        g_e1000.rx_descs[i].special = 0;
    }
    
    // Allocate RX buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        void* buf_phys = pmm_alloc_frame();
        if (!buf_phys) {
            DEBUG_ERROR("e1000: Failed to allocate RX buffer %d", i);
            return false;
        }
        g_e1000.rx_buffers_phys[i] = (uint64_t)buf_phys;
        g_e1000.rx_buffers[i] = (uint8_t*)vmm_phys_to_virt((uint64_t)buf_phys);
        g_e1000.rx_descs[i].addr = (uint64_t)buf_phys;
    }
    
    // Set up RX descriptor ring
    e1000_write_reg(E1000_REG_RDBAL, (uint32_t)(g_e1000.rx_descs_phys & 0xFFFFFFFF));
    e1000_write_reg(E1000_REG_RDBAH, (uint32_t)(g_e1000.rx_descs_phys >> 32));
    e1000_write_reg(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc));
    e1000_write_reg(E1000_REG_RDH, 0);
    e1000_write_reg(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    
    g_e1000.rx_cur = 0;
    
    // Enable receiver
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | 
                    E1000_RCTL_SECRC | E1000_RCTL_LBM_NONE;
    e1000_write_reg(E1000_REG_RCTL, rctl);
    
    KLOG(MOD_NET, LOG_TRACE, "e1000: RX initialized with %d descriptors", E1000_NUM_RX_DESC);
    return true;
}

// Initialize TX descriptors
static bool e1000_init_tx() {
    // Allocate descriptor ring
    void* tx_ring_phys = pmm_alloc_frame();
    if (!tx_ring_phys) {
        DEBUG_ERROR("e1000: Failed to allocate TX descriptor ring");
        return false;
    }
    
    g_e1000.tx_descs_phys = (uint64_t)tx_ring_phys;
    g_e1000.tx_descs = (e1000_tx_desc*)vmm_phys_to_virt(g_e1000.tx_descs_phys);
    
    // Clear descriptors
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        g_e1000.tx_descs[i].addr = 0;
        g_e1000.tx_descs[i].length = 0;
        g_e1000.tx_descs[i].cso = 0;
        g_e1000.tx_descs[i].cmd = 0;
        g_e1000.tx_descs[i].status = E1000_TXD_STAT_DD; // Mark as done initially
        g_e1000.tx_descs[i].css = 0;
        g_e1000.tx_descs[i].special = 0;
    }
    
    // Set up TX descriptor ring
    e1000_write_reg(E1000_REG_TDBAL, (uint32_t)(g_e1000.tx_descs_phys & 0xFFFFFFFF));
    e1000_write_reg(E1000_REG_TDBAH, (uint32_t)(g_e1000.tx_descs_phys >> 32));
    e1000_write_reg(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc));
    e1000_write_reg(E1000_REG_TDH, 0);
    e1000_write_reg(E1000_REG_TDT, 0);
    
    g_e1000.tx_cur = 0;
    
    // Set Inter Packet Gap
    // Recommended values: IPGT=10, IPGR1=10, IPGR2=10 (for full duplex)
    e1000_write_reg(E1000_REG_TIPG, (10 << 0) | (10 << 10) | (10 << 20));
    
    // Enable transmitter
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (15 << E1000_TCTL_CT_SHIFT) |    // Collision threshold
                    (64 << E1000_TCTL_COLD_SHIFT);   // Collision distance
    e1000_write_reg(E1000_REG_TCTL, tctl);
    
    KLOG(MOD_NET, LOG_TRACE, "e1000: TX initialized with %d descriptors", E1000_NUM_TX_DESC);
    return true;
}

// Find and initialize e1000 device
bool e1000_init() {
    if (g_e1000.initialized) {
        return true;
    }
    
    KLOG(MOD_NET, LOG_TRACE, "e1000: Scanning for Intel NIC...");
    
    // Scan PCI for Intel NICs
    PciDevice nic;
    bool found = false;
    
    // Search all PCI devices for Intel NICs
    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t dev = 0; dev < 32 && !found; dev++) {
            uint16_t vendor = pci_config_read16(bus, dev, 0, PCI_VENDOR_ID);
            if (vendor == 0xFFFF) continue;
            
            uint8_t header_type = pci_config_read8(bus, dev, 0, PCI_HEADER_TYPE);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;
            
            for (uint8_t func = 0; func < max_func && !found; func++) {
                vendor = pci_config_read16(bus, dev, func, PCI_VENDOR_ID);
                if (vendor != E1000_VENDOR_ID) continue;
                
                uint16_t device_id = pci_config_read16(bus, dev, func, PCI_DEVICE_ID);
                uint8_t class_code = pci_config_read8(bus, dev, func, PCI_CLASS);
                uint8_t subclass = pci_config_read8(bus, dev, func, PCI_SUBCLASS);
                
                // Check if it's a network controller
                if (class_code == 0x02 && subclass == 0x00) {
                    // Accept ANY Intel network controller
                    // The e1000/e1000e family is compatible at register level
                    nic.bus = bus;
                    nic.device = dev;
                    nic.function = func;
                    nic.vendor_id = vendor;
                    nic.device_id = device_id;
                    nic.class_code = class_code;
                    nic.subclass = subclass;
                    nic.irq_line = pci_config_read8(bus, dev, func, PCI_INTERRUPT_LINE);
                    found = true;
                    
                    DEBUG_INFO("e1000: Found Intel NIC %04x:%04x at %d:%d.%d",
                        vendor, device_id, bus, dev, func);
                }
            }
        }
    }
    
    if (!found) {
        DEBUG_WARN("e1000: No Intel NIC found");
        return false;
    }
    
    // Enable bus mastering and memory space
    pci_enable_bus_mastering(&nic);
    pci_enable_memory_space(&nic);
    
    // Get BAR0 (MMIO)
    uint64_t bar_size;
    uint64_t bar0 = pci_get_bar(&nic, 0, &bar_size);
    
    if (!pci_bar_is_mmio(&nic, 0)) {
        DEBUG_ERROR("e1000: BAR0 is not MMIO!");
        return false;
    }
    
    // Map MMIO with proper uncacheable flags (critical for real hardware!)
    g_e1000.mmio_base = vmm_map_mmio(bar0, bar_size);
    if (g_e1000.mmio_base == 0) {
        DEBUG_ERROR("e1000: Failed to map MMIO region");
        return false;
    }
    KLOG(MOD_NET, LOG_TRACE, "e1000: MMIO base at 0x%lx (phys 0x%lx), size %lu bytes",
        g_e1000.mmio_base, bar0, bar_size);
    
    // Reset the device
    uint32_t ctrl = e1000_read_reg(E1000_REG_CTRL);
    e1000_write_reg(E1000_REG_CTRL, ctrl | E1000_CTRL_RST);
    
    // Wait for reset to complete (up to 10ms)
    for (int i = 0; i < 100; i++) {
        for (volatile int j = 0; j < 10000; j++);
        ctrl = e1000_read_reg(E1000_REG_CTRL);
        if (!(ctrl & E1000_CTRL_RST)) break;
    }
    
    // Disable interrupts (we'll use polling)
    e1000_write_reg(E1000_REG_IMC, 0xFFFFFFFF);
    e1000_read_reg(E1000_REG_ICR); // Clear pending interrupts
    
    // Clear multicast table
    for (int i = 0; i < 128; i++) {
        e1000_write_reg(E1000_REG_MTA + (i * 4), 0);
    }
    
    // Read MAC address
    e1000_read_mac();
    
    // Initialize RX
    if (!e1000_init_rx()) {
        DEBUG_ERROR("e1000: Failed to initialize RX");
        return false;
    }
    
    // Initialize TX
    if (!e1000_init_tx()) {
        DEBUG_ERROR("e1000: Failed to initialize TX");
        return false;
    }
    
    // Set link up
    ctrl = e1000_read_reg(E1000_REG_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~E1000_CTRL_LRST;
    ctrl &= ~E1000_CTRL_PHY_RST;
    e1000_write_reg(E1000_REG_CTRL, ctrl);
    
    // Wait for link
    for (int i = 0; i < 100; i++) {
        for (volatile int j = 0; j < 50000; j++);
        uint32_t status = e1000_read_reg(E1000_REG_STATUS);
        if (status & E1000_STATUS_LU) {
            g_e1000.link_up = true;
            DEBUG_INFO("e1000: Link is UP");
            break;
        }
    }
    
    if (!g_e1000.link_up) {
        DEBUG_WARN("e1000: Link is DOWN (may come up later)");
    }
    
    g_e1000.initialized = true;
    DEBUG_INFO("e1000: Initialization complete");
    return true;
}

// Send a packet
bool e1000_send(const void* data, uint16_t length) {
    if (!g_e1000.initialized || !data || length == 0 || length > 1500) {
        return false;
    }
    
    uint32_t cur = g_e1000.tx_cur;
    e1000_tx_desc* desc = &g_e1000.tx_descs[cur];
    
    // Wait for previous transmission to complete (if any)
    int timeout = 10000;
    while (!(desc->status & E1000_TXD_STAT_DD) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }
    
    if (timeout <= 0) {
        DEBUG_WARN("e1000: TX timeout waiting for descriptor");
        return false;
    }
    
    // Allocate buffer for this packet
    void* tx_buf_phys = pmm_alloc_frame();
    if (!tx_buf_phys) {
        DEBUG_ERROR("e1000: Failed to allocate TX buffer");
        return false;
    }
    
    uint8_t* tx_buf = (uint8_t*)vmm_phys_to_virt((uint64_t)tx_buf_phys);
    
    // Copy data to TX buffer
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < length; i++) {
        tx_buf[i] = src[i];
    }
    
    // Set up descriptor
    desc->addr = (uint64_t)tx_buf_phys;
    desc->length = length;
    desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    desc->status = 0;
    desc->cso = 0;
    desc->css = 0;
    desc->special = 0;
    
    // Advance tail
    g_e1000.tx_cur = (cur + 1) % E1000_NUM_TX_DESC;
    e1000_write_reg(E1000_REG_TDT, g_e1000.tx_cur);
    
    // Wait for transmission (with short timeout for debug)
    timeout = 10000;
    while (!(desc->status & E1000_TXD_STAT_DD) && timeout-- > 0) {
        for (volatile int i = 0; i < 100; i++);
    }
    
    // Free the TX buffer
    pmm_free_frame(tx_buf_phys);
    
    return (desc->status & E1000_TXD_STAT_DD) != 0;
}

// Receive a packet (returns length, or 0 if no packet)
int e1000_receive(void* buffer, uint16_t max_length) {
    if (!g_e1000.initialized || !buffer) {
        return 0;
    }
    
    uint32_t cur = g_e1000.rx_cur;
    e1000_rx_desc* desc = &g_e1000.rx_descs[cur];
    
    // Check if descriptor is done
    if (!(desc->status & E1000_RXD_STAT_DD)) {
        return 0; // No packet available
    }
    
    // Check for errors
    if (desc->errors) {
        DEBUG_WARN("e1000: RX error 0x%02x", desc->errors);
        // Reset descriptor and continue
        desc->status = 0;
        g_e1000.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
        e1000_write_reg(E1000_REG_RDT, cur);
        return 0;
    }
    
    // Get packet length
    uint16_t length = desc->length;
    if (length > max_length) {
        length = max_length;
    }
    
    // Copy data
    uint8_t* src = g_e1000.rx_buffers[cur];
    uint8_t* dst = (uint8_t*)buffer;
    for (uint16_t i = 0; i < length; i++) {
        dst[i] = src[i];
    }
    
    // Reset descriptor for reuse
    desc->status = 0;
    
    // Advance to next descriptor
    uint32_t old_cur = cur;
    g_e1000.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
    
    // Update tail (give this descriptor back to hardware)
    e1000_write_reg(E1000_REG_RDT, old_cur);
    
    return length;
}

// Get MAC address
void e1000_get_mac(uint8_t* out_mac) {
    if (!out_mac) return;
    for (int i = 0; i < 6; i++) {
        out_mac[i] = g_e1000.mac[i];
    }
}

// Check link status
bool e1000_link_up() {
    if (!g_e1000.initialized) return false;
    
    uint32_t status = e1000_read_reg(E1000_REG_STATUS);
    g_e1000.link_up = (status & E1000_STATUS_LU) != 0;
    return g_e1000.link_up;
}

// Poll for events (for interrupt-less operation)
void e1000_poll() {
    if (!g_e1000.initialized) return;
    
    // Read and clear interrupt cause
    e1000_read_reg(E1000_REG_ICR);
}

// Get device (internal)
E1000Device* e1000_get_device() {
    return &g_e1000;
}
