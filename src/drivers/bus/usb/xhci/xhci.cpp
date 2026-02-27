/**
 * @file xhci.cpp
 * @brief xHCI (USB 3.0) Host Controller Driver for uniOS
 *
 * Implements Intel xHCI specification for USB 3.0 controllers.
 * Based on xHCI spec Rev 1.2 and Linux kernel driver patterns.
 */

#include <drivers/bus/usb/xhci/xhci.h>
#include <drivers/bus/pci/pci.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/debug.h>
#include <kernel/time/timer.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>
#include <stddef.h>

// Global controller instance
static XhciController xhci;
static bool xhci_initialized = false;
static Spinlock xhci_lock = SPINLOCK_INIT;

// Per-endpoint DMA buffers for interrupt transfers
static DMAAllocation intr_buffer_dma[32][32] = {{{0, 0, 0}}};

// IRQ line
static uint8_t xhci_irq = 0;

uint8_t xhci_get_irq() {
    return xhci_irq;
}

// Endpoint failure tracking
static uint8_t ep_failures[256][32] = {{0}};
#define MAX_EP_FAILURES 5

// Forward declarations
static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target);
static bool xhci_send_command(Trb* trb, Trb* result);
static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_idx, Trb* trb);

using kstring::zero_memory;

// =============================================================================
// Completion code to string (for debugging)
// =============================================================================

const char* xhci_completion_code_str(uint8_t code) {
    switch (code) {
        case TRB_COMP_SUCCESS: return "Success";
        case TRB_COMP_DATA_BUFFER: return "Data Buffer Error";
        case TRB_COMP_BABBLE: return "Babble";
        case TRB_COMP_USB_TRANSACTION: return "USB Transaction Error";
        case TRB_COMP_TRB: return "TRB Error";
        case TRB_COMP_STALL: return "Stall";
        case TRB_COMP_SHORT_PACKET: return "Short Packet";
        case TRB_COMP_SLOT_NOT_ENABLED: return "Slot Not Enabled";
        case TRB_COMP_EP_NOT_ENABLED: return "Endpoint Not Enabled";
        default: return "Unknown";
    }
}

// =============================================================================
// BIOS Handoff - Take ownership from BIOS/UEFI
// =============================================================================

static void xhci_bios_handoff() {
    uint32_t hccparams1 = xhci.cap->hccparams1;
    uint32_t xecp_off = HCCPARAMS1_XECP(hccparams1) << 2;
    if (!xecp_off) return;
    
    uint64_t base = (uint64_t)xhci.cap;
    volatile uint32_t* xecp = (volatile uint32_t*)(base + xecp_off);
    
    // Walk extended capability list (max 256 iterations for safety)
    for (int i = 0; i < 256; i++) {
        uint32_t header = mmio_read32((void*)xecp);
        uint8_t cap_id = header & 0xFF;
        
        if (cap_id == XECP_ID_LEGACY) {
            // USB Legacy Support capability found
            if (header & USBLEGSUP_BIOS_SEM) {
                KLOG(MOD_USB, LOG_INFO, "BIOS owns xHCI, requesting handoff...");
                
                mmio_write32((void*)xecp, header | USBLEGSUP_OS_SEM);
                
                uint64_t start = timer_get_ticks();
                while (mmio_read32((void*)xecp) & USBLEGSUP_BIOS_SEM) {
                    if (timer_get_ticks() - start > 1000) {
                        DEBUG_WARN("BIOS handoff timeout, forcing ownership");
                        break;
                    }
                    for (volatile int j = 0; j < 1000; j++);
                }
                
                volatile uint32_t* legctlsts = xecp + 1;
                uint32_t ctl = mmio_read32((void*)legctlsts);
                ctl &= ~0xFFFF;
                ctl |= 0xE0000000;
                mmio_write32((void*)legctlsts, ctl);
                
                KLOG(MOD_USB, LOG_SUCCESS, "BIOS handoff complete");
            }
            return;
        }
        
        uint8_t next = (header >> 8) & 0xFF;
        if (!next) break;
        xecp = (volatile uint32_t*)((uint64_t)xecp + (next << 2));
    }
}

// =============================================================================
// Parse Supported Protocol Capabilities (USB2/USB3 port mapping)
// =============================================================================

static void xhci_parse_protocols() {
    uint32_t hccparams1 = xhci.cap->hccparams1;
    uint32_t xecp_off = HCCPARAMS1_XECP(hccparams1) << 2;
    if (!xecp_off) return;
    
    uint64_t base = (uint64_t)xhci.cap;
    volatile uint32_t* xecp = (volatile uint32_t*)(base + xecp_off);
    
    for (int i = 0; i < 256; i++) {
        uint32_t header = mmio_read32((void*)xecp);
        uint8_t cap_id = header & 0xFF;
        
        if (cap_id == XECP_ID_PROTOCOLS) {
            uint32_t rev = mmio_read32((void*)(xecp + 0));
            uint32_t ports = mmio_read32((void*)(xecp + 2));
            
            uint8_t major = (rev >> 24) & 0xFF;
            uint8_t port_off = ports & 0xFF;
            uint8_t port_cnt = (ports >> 8) & 0xFF;
            
            if (major == 2) {
                xhci.usb2_port_start = port_off;
                xhci.usb2_port_count = port_cnt;
            } else if (major == 3) {
                xhci.usb3_port_start = port_off;
                xhci.usb3_port_count = port_cnt;
            }
        }
        
        uint8_t next = (header >> 8) & 0xFF;
        if (!next) break;
        xecp = (volatile uint32_t*)((uint64_t)xecp + (next << 2));
    }
}

// =============================================================================
// Controller Initialization
// =============================================================================

bool xhci_init() {
    if (xhci_initialized) return true;
    DEBUG_INFO("Initializing xHCI controller...");
    
    PciDevice pci_dev;
    if (!pci_find_xhci(&pci_dev)) {
        DEBUG_ERROR("xHCI controller not found");
        return false;
    }
    DEBUG_INFO("Found xHCI at %d:%d.%d", pci_dev.bus, pci_dev.device, pci_dev.function);
    
    pci_enable_memory_space(&pci_dev);
    pci_enable_bus_mastering(&pci_dev);
    pci_enable_interrupts(&pci_dev);
    xhci_irq = pci_dev.irq_line;

    if (xhci_irq > 0 && xhci_irq < 16) {
        if (xhci_irq >= 8) pic_clear_mask(2);
        pic_clear_mask(xhci_irq);
        DEBUG_INFO("xHCI IRQ %d unmasked", xhci_irq);
    } else {
        DEBUG_WARN("xHCI: Invalid or unsupported IRQ line %d", xhci_irq);
    }
    
    uint64_t bar_size;
    uint64_t bar_phys = pci_get_bar(&pci_dev, 0, &bar_size);
    if (!bar_phys) {
        DEBUG_ERROR("Invalid BAR0");
        return false;
    }
    
    uint64_t bar_virt = vmm_map_mmio(bar_phys, bar_size);
    if (!bar_virt) {
        DEBUG_ERROR("MMIO mapping failed");
        return false;
    }
    
    // Setup register pointers
    xhci.cap = (volatile XhciCapRegs*)bar_virt;
    
    // BIOS handoff first
    xhci_bios_handoff();
    
    // Setup remaining register pointers
    uint8_t cap_len = xhci.cap->caplength;
    xhci.op = (volatile XhciOpRegs*)(bar_virt + cap_len);
    xhci.runtime = (volatile XhciRuntimeRegs*)(bar_virt + xhci.cap->rtsoff);
    xhci.doorbell = (volatile uint32_t*)(bar_virt + xhci.cap->dboff);
    xhci.ports = (volatile XhciPortRegs*)(bar_virt + cap_len + 0x400);
    
    // Parse capabilities
    uint32_t hcsparams1 = xhci.cap->hcsparams1;
    uint32_t hcsparams2 = xhci.cap->hcsparams2;
    uint32_t hccparams1 = xhci.cap->hccparams1;
    
    xhci.max_slots = HCSPARAMS1_MAX_SLOTS(hcsparams1);
    xhci.max_ports = HCSPARAMS1_MAX_PORTS(hcsparams1);
    xhci.max_intrs = HCSPARAMS1_MAX_INTRS(hcsparams1);
    xhci.context_size_64 = HCCPARAMS1_CSZ(hccparams1);
    xhci.page_size = mmio_read32((void*)&xhci.op->pagesize) << 12;
    xhci.num_scratchpad = HCSPARAMS2_MAX_SCRATCHPAD(hcsparams2);
    
    KLOG(MOD_USB, LOG_TRACE, "MaxSlots=%d MaxPorts=%d PageSize=%d CSZ=%d",
         xhci.max_slots, xhci.max_ports, xhci.page_size,
         xhci.context_size_64 ? 64 : 32);
    
    // Parse protocol capabilities
    xhci_parse_protocols();
    
    // Reset controller
    if (!xhci_reset()) {
        DEBUG_ERROR("Controller reset failed");
        return false;
    }
    
    // Wait for CNR to clear
    uint32_t timeout = 500000;
    while ((mmio_read32((void*)&xhci.op->usbsts) & USBSTS_CNR) && timeout--) {
        io_wait();
    }
    if (!timeout) {
        DEBUG_ERROR("Controller not ready");
        return false;
    }
    
    // Configure max slots
    mmio_write32((void*)&xhci.op->config, xhci.max_slots);
    
    // Allocate DCBAA
    size_t dcbaa_size = (xhci.max_slots + 1) * sizeof(uint64_t);
    DMAAllocation dcbaa_dma = vmm_alloc_dma((dcbaa_size + 4095) / 4096);
    if (!dcbaa_dma.phys) return false;
    
    xhci.dcbaa = (uint64_t*)dcbaa_dma.virt;
    xhci.dcbaa_phys = dcbaa_dma.phys;
    zero_memory(xhci.dcbaa, dcbaa_size);
    
    // Allocate scratchpad if needed
    if (xhci.num_scratchpad > 0) {
        DMAAllocation arr_dma = vmm_alloc_dma(1);
        if (!arr_dma.phys) return false;
        
        xhci.scratchpad_array = (uint64_t*)arr_dma.virt;
        xhci.scratchpad_array_phys = arr_dma.phys;
        
        for (uint32_t i = 0; i < xhci.num_scratchpad; i++) {
            DMAAllocation pg = vmm_alloc_dma(1);
            if (!pg.phys) return false;
            xhci.scratchpad_array[i] = pg.phys;
        }
        xhci.dcbaa[0] = xhci.scratchpad_array_phys;
    }
    
    // Write DCBAAP
    mmio_write64((void*)&xhci.op->dcbaap, xhci.dcbaa_phys);
    
    // Allocate Command Ring
    DMAAllocation cmd_dma = vmm_alloc_dma(1);
    if (!cmd_dma.phys) return false;
    
    xhci.cmd_ring = (Trb*)cmd_dma.virt;
    xhci.cmd_ring_phys = cmd_dma.phys;
    zero_memory(xhci.cmd_ring, XHCI_RING_SIZE * sizeof(Trb));
    xhci.cmd_enqueue = 0;
    xhci.cmd_cycle = 1;
    
    // Setup Link TRB
    Trb* link = &xhci.cmd_ring[XHCI_RING_SIZE - 1];
    link->parameter = xhci.cmd_ring_phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
    
    // Write CRCR
    mmio_write64((void*)&xhci.op->crcr, xhci.cmd_ring_phys | CRCR_RCS);
    
    // Allocate Event Ring
    DMAAllocation evt_dma = vmm_alloc_dma(1);
    if (!evt_dma.phys) return false;
    
    xhci.event_ring = (Trb*)evt_dma.virt;
    xhci.event_ring_phys = evt_dma.phys;
    zero_memory(xhci.event_ring, XHCI_EVENT_RING_SIZE * sizeof(Trb));
    xhci.event_dequeue = 0;
    xhci.event_cycle = 1;
    
    // Allocate ERST
    DMAAllocation erst_dma = vmm_alloc_dma(1);
    if (!erst_dma.phys) return false;
    
    xhci.erst = (ErstEntry*)erst_dma.virt;
    xhci.erst_phys = erst_dma.phys;
    xhci.erst[0].ring_segment_base = xhci.event_ring_phys;
    xhci.erst[0].ring_segment_size = XHCI_EVENT_RING_SIZE;
    xhci.erst[0].reserved = 0;
    
    // Setup interrupter 0
    volatile XhciInterrupterRegs* ir = 
        (volatile XhciInterrupterRegs*)((uint64_t)xhci.runtime + 0x20);
    
    mmio_write32((void*)&ir->iman, 0);
    mmio_write32((void*)&ir->erstsz, 1);
    mmio_write64((void*)&ir->erdp, xhci.event_ring_phys);
    mmio_write64((void*)&ir->erstba, xhci.erst_phys);
    mmio_write32((void*)&ir->imod, 4000);  // 1ms moderation
    mmio_write32((void*)&ir->iman, IMAN_IE);
    
    // Initialize slot arrays
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        xhci.device_contexts[i] = nullptr;
        xhci.input_contexts[i] = nullptr;
        for (int j = 0; j < XHCI_MAX_ENDPOINTS; j++) {
            xhci.transfer_rings[i][j] = nullptr;
            xhci.intr_pending[i][j] = false;
            xhci.intr_complete[i][j] = false;
        }
    }
    
    // Power on ports
    for (uint8_t i = 0; i < xhci.max_ports; i++) {
        uint32_t portsc = mmio_read32((void*)&xhci.ports[i]);
        if (!(portsc & PORTSC_PP)) {
            mmio_write32((void*)&xhci.ports[i], 
                        (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_PP);
        }
    }
    
    // Wait for power stabilization
    sleep(100);
    
    // Start controller
    if (!xhci_start()) {
        DEBUG_ERROR("Controller start failed");
        return false;
    }
    
    xhci_initialized = true;
    DEBUG_INFO("xHCI initialized successfully");
    return true;
}

// =============================================================================
// Reset, Start, Stop
// =============================================================================

bool xhci_reset() {
    // Stop controller first
    uint32_t cmd = mmio_read32((void*)&xhci.op->usbcmd);
    cmd &= ~USBCMD_RS;
    mmio_write32((void*)&xhci.op->usbcmd, cmd);
    
    // Wait for halt
    uint32_t timeout = 500000;
    while (!(mmio_read32((void*)&xhci.op->usbsts) & USBSTS_HCH) && timeout--) {
        io_wait();
    }
    if (!timeout) return false;
    
    // Issue reset
    cmd = mmio_read32((void*)&xhci.op->usbcmd);
    mmio_write32((void*)&xhci.op->usbcmd, cmd | USBCMD_HCRST);
    
    // Wait for reset complete
    timeout = 500000;
    while ((mmio_read32((void*)&xhci.op->usbcmd) & USBCMD_HCRST) && timeout--) {
        io_wait();
    }
    return timeout > 0;
}

bool xhci_start() {
    uint32_t cmd = mmio_read32((void*)&xhci.op->usbcmd);
    mmio_write32((void*)&xhci.op->usbcmd, cmd | USBCMD_RS | USBCMD_INTE);
    
    uint32_t timeout = 100000;
    while ((mmio_read32((void*)&xhci.op->usbsts) & USBSTS_HCH) && timeout--) {
        io_wait();
    }
    return timeout > 0;
}

void xhci_stop() {
    uint32_t cmd = mmio_read32((void*)&xhci.op->usbcmd);
    mmio_write32((void*)&xhci.op->usbcmd, cmd & ~USBCMD_RS);
}

bool xhci_is_initialized() { return xhci_initialized; }
uint8_t xhci_get_max_ports() { return xhci_initialized ? xhci.max_ports : 0; }

uint8_t xhci_get_port_protocol(uint8_t port) {
    if (port >= xhci.usb3_port_start && 
        port < xhci.usb3_port_start + xhci.usb3_port_count)
        return 3;
    return 2;
}

// =============================================================================
// Port Operations
// =============================================================================

uint8_t xhci_get_port_speed(uint8_t port) {
    if (port == 0 || port > xhci.max_ports) return 0;
    uint32_t portsc = mmio_read32((void*)&xhci.ports[port - 1]);
    return (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
}

bool xhci_port_connected(uint8_t port) {
    if (port == 0 || port > xhci.max_ports) return false;
    uint32_t portsc = mmio_read32((void*)&xhci.ports[port - 1]);
    return (portsc & PORTSC_CCS) != 0;
}

bool xhci_reset_port(uint8_t port) {
    if (port == 0 || port > xhci.max_ports) return false;
    
    volatile XhciPortRegs* p = &xhci.ports[port - 1];
    uint32_t portsc = mmio_read32((void*)p);
    
    if (!(portsc & PORTSC_CCS)) return false;
    
    // Clear change bits first
    mmio_write32((void*)p, (portsc & PORTSC_CHANGE_MASK) | PORTSC_PP);
    
    // Determine reset type based on port protocol
    bool is_usb3 = xhci_get_port_protocol(port) == 3;
    
    // Issue reset
    portsc = mmio_read32((void*)p);
    if (is_usb3 && (portsc & PORTSC_CCS) && !(portsc & PORTSC_PED)) {
        // USB3: warm reset if not enabled
        mmio_write32((void*)p, 
                    (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_WPR | PORTSC_PP);
    } else {
        // Normal reset
        mmio_write32((void*)p,
                    (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_PR | PORTSC_PP);
    }
    
    // Wait for reset complete (PRC set)
    uint32_t timeout = 500000;
    while (timeout--) {
        portsc = mmio_read32((void*)p);
        if (portsc & PORTSC_PRC) break;
        io_wait();
    }
    
    if (!timeout) {
        DEBUG_ERROR("Port %d reset timeout", port);
        return false;
    }
    
    // Clear change bits
    mmio_write32((void*)p, (portsc & PORTSC_CHANGE_MASK) | PORTSC_PP);
    
    // Check enabled
    portsc = mmio_read32((void*)p);
    return (portsc & PORTSC_PED) != 0;
}

// =============================================================================
// Ring Management
// =============================================================================

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target) {
    mmio_write32((void*)&xhci.doorbell[slot_id], target);
}

static void xhci_enqueue_command(Trb* trb) {
    Trb* dest = &xhci.cmd_ring[xhci.cmd_enqueue];
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | xhci.cmd_cycle;
    
    xhci.cmd_enqueue++;
    if (xhci.cmd_enqueue >= XHCI_RING_SIZE - 1) {
        Trb* link = &xhci.cmd_ring[XHCI_RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | xhci.cmd_cycle;
        xhci.cmd_cycle ^= 1;
        xhci.cmd_enqueue = 0;
    }
}

static bool xhci_wait_command(Trb* result, uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms * 1000;
    
    while (timeout--) {
        Trb* evt = &xhci.event_ring[xhci.event_dequeue];
        uint32_t ctrl = evt->control;
        
        if ((ctrl & TRB_CYCLE) == xhci.event_cycle) {
            uint8_t type = TRB_GET_TYPE(ctrl);
            
            xhci.event_dequeue++;
            if (xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
                xhci.event_dequeue = 0;
                xhci.event_cycle ^= 1;
            }
            
            // Update ERDP
            volatile XhciInterrupterRegs* ir = 
                (volatile XhciInterrupterRegs*)((uint64_t)xhci.runtime + 0x20);
            uint64_t erdp = xhci.event_ring_phys + xhci.event_dequeue * sizeof(Trb);
            mmio_write64((void*)&ir->erdp, erdp | ERDP_EHB);
            
            if (type == TRB_TYPE_COMMAND_COMPLETION) {
                if (result) *result = *evt;
                return ((evt->status >> 24) & 0xFF) == TRB_COMP_SUCCESS;
            }
        }
        io_wait();
    }
    return false;
}

static bool xhci_send_command(Trb* trb, Trb* result) {
    xhci_enqueue_command(trb);
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(0, 0);
    return xhci_wait_command(result, 1000);
}

static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_idx, Trb* trb) {
    Trb* ring = xhci.transfer_rings[slot_id][ep_idx];
    uint32_t idx = xhci.transfer_enqueue[slot_id][ep_idx];
    
    if (idx >= XHCI_RING_SIZE - 1) {
        Trb* link = &ring[XHCI_RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | xhci.transfer_cycle[slot_id][ep_idx];
        xhci.transfer_cycle[slot_id][ep_idx] ^= 1;
        xhci.transfer_enqueue[slot_id][ep_idx] = 0;
        idx = 0;
    }
    
    Trb* dest = &ring[idx];
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | xhci.transfer_cycle[slot_id][ep_idx];
    
    xhci.transfer_enqueue[slot_id][ep_idx]++;
}

// =============================================================================
// Slot Operations
// =============================================================================

int xhci_enable_slot() {
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_ENABLE_SLOT)};
    Trb result;
    
    if (!xhci_send_command(&cmd, &result)) {
        DEBUG_ERROR("Enable Slot command failed");
        return -1;
    }
    
    return (result.control >> 24) & 0xFF;
}

bool xhci_disable_slot(uint8_t slot_id) {
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_DISABLE_SLOT) | ((uint32_t)slot_id << 24)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_address_device(uint8_t slot_id, uint8_t port, uint8_t speed) {
    // Allocate device context
    size_t ctx_size = xhci.context_size_64 ? 64 : 32;
    size_t dev_ctx_bytes = ctx_size * 32;
    
    DMAAllocation dev_dma = vmm_alloc_dma((dev_ctx_bytes + 4095) / 4096);
    if (!dev_dma.phys) return false;
    
    xhci.device_contexts[slot_id] = (DeviceContext*)dev_dma.virt;
    xhci.device_context_phys[slot_id] = dev_dma.phys;
    zero_memory(xhci.device_contexts[slot_id], dev_ctx_bytes);
    
    // Point DCBAA entry
    xhci.dcbaa[slot_id] = dev_dma.phys;
    
    // Allocate input context
    size_t input_ctx_bytes = ctx_size * 33;
    DMAAllocation input_dma = vmm_alloc_dma((input_ctx_bytes + 4095) / 4096);
    if (!input_dma.phys) return false;
    
    xhci.input_contexts[slot_id] = (InputContext*)input_dma.virt;
    xhci.input_context_phys[slot_id] = input_dma.phys;
    zero_memory(xhci.input_contexts[slot_id], input_ctx_bytes);
    
    InputContext* input = xhci.input_contexts[slot_id];
    
    // Add slot and EP0
    input->control.drop_flags = 0;
    input->control.add_flags = (1 << 0) | (1 << 1);
    
    // Slot context
    input->slot.route_speed_entries = ((uint32_t)speed << 20) | (1 << 27);
    input->slot.latency_hub_port = ((uint32_t)port << 16);
    
    // Allocate EP0 transfer ring
    DMAAllocation tr_dma = vmm_alloc_dma(1);
    if (!tr_dma.phys) return false;
    
    xhci.transfer_rings[slot_id][0] = (Trb*)tr_dma.virt;
    xhci.transfer_ring_phys[slot_id][0] = tr_dma.phys;
    zero_memory(xhci.transfer_rings[slot_id][0], XHCI_RING_SIZE * sizeof(Trb));
    xhci.transfer_enqueue[slot_id][0] = 0;
    xhci.transfer_cycle[slot_id][0] = 1;
    
    // EP0 Link TRB
    Trb* link = &xhci.transfer_rings[slot_id][0][XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
    
    // EP0 max packet size based on speed
    uint16_t max_pkt;
    switch (speed) {
        case XHCI_SPEED_LOW: max_pkt = 8; break;
        case XHCI_SPEED_FULL: max_pkt = 8; break;
        case XHCI_SPEED_HIGH: max_pkt = 64; break;
        case XHCI_SPEED_SUPER:
        case XHCI_SPEED_SUPER_PLUS: max_pkt = 512; break;
        default: max_pkt = 8; break;
    }
    
    // EP0 context
    input->endpoints[0].ep_state = 0;
    input->endpoints[0].ep_info = (3 << 1) | (EP_TYPE_CONTROL << 3) | ((uint32_t)max_pkt << 16);
    input->endpoints[0].tr_dequeue = tr_dma.phys | 1;
    input->endpoints[0].avg_trb_length = 8;
    
    // Send Address Device command
    Trb cmd = {xhci.input_context_phys[slot_id], 0, 
               TRB_TYPE(TRB_TYPE_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24)};
    Trb result;
    
    if (!xhci_send_command(&cmd, &result)) {
        uint8_t cc = (result.status >> 24) & 0xFF;
        DEBUG_ERROR("Address Device failed: %s", xhci_completion_code_str(cc));
        return false;
    }
    
    return true;
}

bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_num, uint8_t ep_type,
                             uint16_t max_packet, uint8_t interval) {
    if (!xhci.input_contexts[slot_id]) return false;
    
    InputContext* input = xhci.input_contexts[slot_id];
    DeviceContext* dev = xhci.device_contexts[slot_id];
    
    uint8_t dci = ep_num;
    uint8_t ep_idx = dci - 1;
    
    // Setup input control
    input->control.drop_flags = 0;
    input->control.add_flags = (1 << 0) | (1 << dci);
    
    // Copy current slot context
    input->slot = dev->slot;
    
    // Update context entries
    uint8_t entries = (dev->slot.route_speed_entries >> 27) & 0x1F;
    if (dci > entries) {
        input->slot.route_speed_entries = 
            (dev->slot.route_speed_entries & 0x07FFFFFF) | ((uint32_t)dci << 27);
    }
    
    // Allocate transfer ring
    DMAAllocation tr_dma = vmm_alloc_dma(1);
    if (!tr_dma.phys) return false;
    
    xhci.transfer_rings[slot_id][dci] = (Trb*)tr_dma.virt;
    xhci.transfer_ring_phys[slot_id][dci] = tr_dma.phys;
    zero_memory(xhci.transfer_rings[slot_id][dci], XHCI_RING_SIZE * sizeof(Trb));
    xhci.transfer_enqueue[slot_id][dci] = 0;
    xhci.transfer_cycle[slot_id][dci] = 1;
    
    // Link TRB
    Trb* link = &xhci.transfer_rings[slot_id][dci][XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
    
    // Calculate interval for xHCI (different per speed)
    uint8_t speed = (dev->slot.route_speed_entries >> 20) & 0xF;
    uint8_t xhci_interval;
    if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
        xhci_interval = (interval < 1) ? 3 : (interval + 2);
    } else {
        xhci_interval = (interval < 1) ? 0 : (interval - 1);
    }
    if (xhci_interval > 15) xhci_interval = 15;
    
    // Endpoint context
    input->endpoints[ep_idx].ep_state = ((uint32_t)xhci_interval << 16);
    input->endpoints[ep_idx].ep_info = 
        (3 << 1) | ((uint32_t)ep_type << 3) | ((uint32_t)max_packet << 16);
    input->endpoints[ep_idx].tr_dequeue = tr_dma.phys | 1;
    input->endpoints[ep_idx].avg_trb_length = max_packet;
    
    // Send Configure Endpoint command
    Trb cmd = {xhci.input_context_phys[slot_id], 0,
               TRB_TYPE(TRB_TYPE_CONFIG_EP) | ((uint32_t)slot_id << 24)};
    Trb result;
    
    return xhci_send_command(&cmd, &result);
}

bool xhci_reset_endpoint(uint8_t slot_id, uint8_t ep_num) {
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_RESET_EP) | 
               ((uint32_t)slot_id << 24) | ((uint32_t)ep_num << 16)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_set_tr_dequeue(uint8_t slot_id, uint8_t ep_num) {
    uint64_t new_ptr = xhci.transfer_ring_phys[slot_id][ep_num] | 
                       xhci.transfer_cycle[slot_id][ep_num];
    
    Trb cmd = {new_ptr, 0, TRB_TYPE(TRB_TYPE_SET_TR_DEQUEUE) |
               ((uint32_t)slot_id << 24) | ((uint32_t)ep_num << 16)};
    Trb result;
    
    if (xhci_send_command(&cmd, &result)) {
        xhci.transfer_enqueue[slot_id][ep_num] = 0;
        return true;
    }
    return false;
}

// =============================================================================
// Transfer Operations
// =============================================================================

static bool xhci_wait_transfer(uint8_t slot_id, Trb* result, uint32_t iterations) {
    while (iterations--) {
        Trb* evt = &xhci.event_ring[xhci.event_dequeue];
        uint32_t ctrl = evt->control;
        
        if ((ctrl & TRB_CYCLE) == xhci.event_cycle) {
            uint8_t type = TRB_GET_TYPE(ctrl);
            
            xhci.event_dequeue++;
            if (xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
                xhci.event_dequeue = 0;
                xhci.event_cycle ^= 1;
            }
            
            volatile XhciInterrupterRegs* ir = 
                (volatile XhciInterrupterRegs*)((uint64_t)xhci.runtime + 0x20);
            uint64_t erdp = xhci.event_ring_phys + xhci.event_dequeue * sizeof(Trb);
            mmio_write64((void*)&ir->erdp, erdp | ERDP_EHB);
            
            if (type == TRB_TYPE_TRANSFER_EVENT) {
                if (result) *result = *evt;
                uint8_t cc = (evt->status >> 24) & 0xFF;
                return cc == TRB_COMP_SUCCESS || cc == TRB_COMP_SHORT_PACKET;
            }
        }
        io_wait();
    }
    return false;
}

bool xhci_control_transfer(uint8_t slot_id, uint8_t request_type, uint8_t request,
                           uint16_t value, uint16_t index, uint16_t length,
                           void* data, uint16_t* transferred) {
    if (!xhci.transfer_rings[slot_id][0]) return false;
    
    spinlock_acquire(&xhci_lock);
    
    // Allocate DMA buffer if needed
    DMAAllocation dma = {0, 0, 0};
    if (data && length > 0) {
        dma = vmm_alloc_dma(1);
        if (!dma.phys) {
            spinlock_release(&xhci_lock);
            return false;
        }
        
        // Copy OUT data
        if (!(request_type & 0x80)) {
            uint8_t* src = (uint8_t*)data;
            uint8_t* dst = (uint8_t*)dma.virt;
            for (uint16_t i = 0; i < length; i++) dst[i] = src[i];
        }
    }
    
    // Setup Stage TRB
    Trb setup = {0, 0, 0};
    setup.parameter = (uint64_t)request_type | ((uint64_t)request << 8) |
                      ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                      ((uint64_t)length << 48);
    setup.status = 8;
    setup.control = TRB_TYPE(TRB_TYPE_SETUP) | TRB_IDT;
    if (length > 0) {
        setup.control |= (request_type & 0x80) ? TRB_TRT_IN : TRB_TRT_OUT;
    }
    xhci_enqueue_transfer(slot_id, 0, &setup);
    
    // Data Stage TRB
    if (length > 0) {
        Trb data_trb = {dma.phys, length, TRB_TYPE(TRB_TYPE_DATA)};
        if (request_type & 0x80) data_trb.control |= TRB_DIR_IN;
        xhci_enqueue_transfer(slot_id, 0, &data_trb);
    }
    
    // Status Stage TRB
    Trb status_trb = {0, 0, TRB_TYPE(TRB_TYPE_STATUS) | TRB_IOC};
    if (!(request_type & 0x80) || length == 0) status_trb.control |= TRB_DIR_IN;
    xhci_enqueue_transfer(slot_id, 0, &status_trb);
    
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, DB_EP0_IN);
    
    Trb result;
    bool success = xhci_wait_transfer(slot_id, &result, 5000);
    
    // Copy IN data
    if (success && data && length > 0 && (request_type & 0x80)) {
        uint32_t actual = length - (result.status & 0xFFFFFF);
        uint8_t* src = (uint8_t*)dma.virt;
        uint8_t* dst = (uint8_t*)data;
        for (uint32_t i = 0; i < actual; i++) dst[i] = src[i];
        if (transferred) *transferred = actual;
    }
    
    if (dma.phys) vmm_free_dma(dma);
    spinlock_release(&xhci_lock);
    return success;
}

bool xhci_interrupt_transfer(uint8_t slot_id, uint8_t ep_num, void* data,
                             uint16_t length, uint16_t* transferred) {
    if (!xhci.transfer_rings[slot_id][ep_num]) return false;
    
    spinlock_acquire(&xhci_lock);

    // Allocate DMA buffer once per endpoint
    if (intr_buffer_dma[slot_id][ep_num].phys == 0) {
        intr_buffer_dma[slot_id][ep_num] = vmm_alloc_dma(1);
        if (!intr_buffer_dma[slot_id][ep_num].phys) {
            spinlock_release(&xhci_lock);
            return false;
        }
    }
    
    // Check for completion
    if (xhci.intr_complete[slot_id][ep_num]) {
        Trb result = xhci.transfer_result[slot_id][ep_num];
        xhci.intr_complete[slot_id][ep_num] = false;
        
        uint8_t cc = (result.status >> 24) & 0xFF;
        
        // Re-queue transfer immediately
        Trb trb = {intr_buffer_dma[slot_id][ep_num].phys, length,
                   TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP};
        xhci_enqueue_transfer(slot_id, ep_num, &trb);
        asm volatile("mfence" ::: "memory");
        xhci_ring_doorbell(slot_id, ep_num);
        xhci.intr_pending[slot_id][ep_num] = true;
        xhci.intr_start_time[slot_id][ep_num] = timer_get_ticks();
        
        if (cc != TRB_COMP_SUCCESS && cc != TRB_COMP_SHORT_PACKET) {
            ep_failures[slot_id][ep_num]++;
            if (ep_failures[slot_id][ep_num] >= MAX_EP_FAILURES) {
                xhci_reset_endpoint(slot_id, ep_num);
                xhci_set_tr_dequeue(slot_id, ep_num);
                ep_failures[slot_id][ep_num] = 0;
            }
            spinlock_release(&xhci_lock);
            return false;
        }
        
        ep_failures[slot_id][ep_num] = 0;
        
        // Copy data
        uint32_t actual = length - (result.status & 0xFFFFFF);
        uint8_t* src = (uint8_t*)intr_buffer_dma[slot_id][ep_num].virt;
        uint8_t* dst = (uint8_t*)data;
        for (uint32_t i = 0; i < actual; i++) dst[i] = src[i];
        if (transferred) *transferred = actual;
        
        spinlock_release(&xhci_lock);
        return true;
    }
    
    // Check timeout
    if (xhci.intr_pending[slot_id][ep_num]) {
        if (timer_get_ticks() - xhci.intr_start_time[slot_id][ep_num] > 100) {
            xhci.intr_pending[slot_id][ep_num] = false;
        } else {
            spinlock_release(&xhci_lock);
            return false;
        }
    }
    
    // Start new transfer
    Trb trb = {intr_buffer_dma[slot_id][ep_num].phys, length,
               TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP};
    xhci_enqueue_transfer(slot_id, ep_num, &trb);
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, ep_num);
    xhci.intr_pending[slot_id][ep_num] = true;
    xhci.intr_start_time[slot_id][ep_num] = timer_get_ticks();
    
    spinlock_release(&xhci_lock);
    return false;
}

// =============================================================================
// Event Polling
// =============================================================================

void xhci_poll_events() {
    if (!xhci_initialized || !xhci.event_ring) return;
    
    spinlock_acquire(&xhci_lock);

    volatile XhciInterrupterRegs* ir = 
        (volatile XhciInterrupterRegs*)((uint64_t)xhci.runtime + 0x20);

    // Clear Interrupt Pending bit (W1C - Write 1 to Clear)
    uint32_t iman = mmio_read32((void*)&ir->iman);
    if (iman & IMAN_IP) {
        mmio_write32((void*)&ir->iman, iman | IMAN_IP);
    }

    // Clear Event Interrupt bit in USBSTS (W1C)
    uint32_t usbsts = mmio_read32((void*)&xhci.op->usbsts);
    if (usbsts & USBSTS_EINT) {
        mmio_write32((void*)&xhci.op->usbsts, usbsts | USBSTS_EINT);
    }

    int count = 0;
    while (count++ < 64) {
        Trb* evt = &xhci.event_ring[xhci.event_dequeue];
        uint32_t ctrl = evt->control;
        
        if ((ctrl & TRB_CYCLE) != xhci.event_cycle) break;
        
        uint8_t type = TRB_GET_TYPE(ctrl);
        
        if (type == TRB_TYPE_TRANSFER_EVENT) {
            uint8_t slot = (ctrl >> 24) & 0xFF;
            uint8_t ep = (ctrl >> 16) & 0x1F;
            
            if (xhci.intr_pending[slot][ep]) {
                xhci.transfer_result[slot][ep] = *evt;
                xhci.intr_complete[slot][ep] = true;
                xhci.intr_pending[slot][ep] = false;
            }
        } else if (type == TRB_TYPE_PORT_STATUS_CHANGE) {
            uint8_t port = (evt->parameter >> 24) & 0xFF;
            if (port > 0 && port <= xhci.max_ports) {
                volatile uint32_t* portsc = &xhci.ports[port - 1].portsc;
                uint32_t val = mmio_read32((void*)portsc);
                mmio_write32((void*)portsc, (val & PORTSC_CHANGE_MASK) | PORTSC_PP);
            }
        }
        
        xhci.event_dequeue++;
        if (xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
            xhci.event_dequeue = 0;
            xhci.event_cycle ^= 1;
        }
        
        uint64_t erdp = xhci.event_ring_phys + xhci.event_dequeue * sizeof(Trb);
        mmio_write64((void*)&ir->erdp, erdp | ERDP_EHB);
    }

    spinlock_release(&xhci_lock);
}

bool xhci_wait_for_event(uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        Trb* evt = &xhci.event_ring[xhci.event_dequeue];
        if ((evt->control & TRB_CYCLE) == xhci.event_cycle) return true;
        io_wait();
    }
    return false;
}

void xhci_dump_status() {
    if (!xhci_initialized) {
        KLOG(MOD_USB, LOG_WARN, "xHCI not initialized");
        return;
    }
    
    KLOG(MOD_USB, LOG_INFO, "xHCI Status: USBSTS=0x%x", mmio_read32((void*)&xhci.op->usbsts));
    for (uint8_t i = 0; i < xhci.max_ports && i < 8; i++) {
        uint32_t portsc = mmio_read32((void*)&xhci.ports[i]);
        KLOG(MOD_USB, LOG_TRACE, "Port %d: PORTSC=0x%x CCS=%d PED=%d Speed=%d",
             i + 1, portsc, (portsc & PORTSC_CCS) ? 1 : 0,
             (portsc & PORTSC_PED) ? 1 : 0,
             (portsc >> PORTSC_SPEED_SHIFT) & 0xF);
    }
}
