#include "xhci.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include "usb.h" 
#include "debug.h"
#include "timer.h"
#include <stddef.h>

// Global xHCI controller instance
static XhciController xhci;
static bool xhci_initialized = false;

// Debug flag for verbose logging
static bool xhci_debug = false;

// Endpoint failure tracking for stuck detection
static uint8_t endpoint_failures[256][32] = {{0}};
#define MAX_ENDPOINT_FAILURES 5

// Forward declarations
static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target);
static bool xhci_send_command(Trb* trb, Trb* result);
static bool xhci_wait_command_completion(Trb* result, uint32_t timeout_ms);
static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_index, Trb* trb);
static bool xhci_wait_transfer_event(uint8_t slot_id, Trb* result, uint32_t iterations);

// Check if xHCI is initialized
bool xhci_is_initialized() {
    return xhci_initialized;
}

// Get max ports
uint8_t xhci_get_max_ports() {
    return xhci_initialized ? xhci.max_ports : 0;
}


// Zero memory at virtual address
static void zero_memory(void* virt, uint64_t size) {
    uint8_t* p = (uint8_t*)virt;
    for (uint64_t i = 0; i < size; i++) {
        p[i] = 0;
    }
}

// Perform BIOS handoff
static void xhci_bios_handoff() {
    uint32_t hccparams1 = xhci.cap->hccparams1;
    uint32_t xecp_offset = HCCPARAMS1_XECP(hccparams1) << 2;
    
    if (!xecp_offset) return; // No extended capabilities
    
    // Base of extended capabilities
    uint64_t base = (uint64_t)xhci.cap;
    volatile uint32_t* xecp = (volatile uint32_t*)(base + xecp_offset);
    
    while (true) {
        uint32_t cap_header = *xecp;
        uint8_t cap_id = cap_header & 0xFF;
        
        if (cap_id == XECP_ID_LEGACY) {
            // Found USB Legacy Support capability
            // Check if BIOS owns it
            if (cap_header & USBLEGSUP_BIOS_SEM) {
                // Request ownership
                *xecp |= USBLEGSUP_OS_SEM;
                
                // Wait for BIOS to release (timeout ~1s)
                uint32_t timeout = 1000000;
                while ((*xecp & USBLEGSUP_BIOS_SEM) && timeout--) {
                    io_wait();
                }
                
                // Disable legacy SMIs (next dword)
                volatile uint32_t* legctlsts = xecp + 1;
                *legctlsts &= ~USBLEGCTLSTS_SMI_ENABLE; // Clear SMI enable bits
                *legctlsts |= 0xE0000000; // Write-1-to-clear status bits
            }
            return;
        }
        
        // Next capability
        uint8_t next = (cap_header >> 8) & 0xFF;
        if (!next) break;
        xecp = (volatile uint32_t*)(base + (next << 2));
    }
}

// Initialize xHCI controller
bool xhci_init() {
    DEBUG_LOG("Initializing xHCI...");
    if (xhci_initialized) return true;
    
    // Find xHCI controller via PCI
    PciDevice pci_dev;
    if (!pci_find_xhci(&pci_dev)) {
        DEBUG_ERROR("Error: xHCI Controller not found");
        return false;
    }
    DEBUG_LOG("xHCI found at Bus %d Dev %d Func %d", pci_dev.bus, pci_dev.device, pci_dev.function);
    
    // Enable PCI features
    pci_enable_memory_space(&pci_dev);
    pci_enable_bus_mastering(&pci_dev);
    
    // Get BAR0 (MMIO base)
    uint64_t bar_size;
    uint64_t bar_phys = pci_get_bar(&pci_dev, 0, &bar_size);
    if (bar_phys == 0 || bar_size == 0) {
        DEBUG_ERROR("Error: Invalid BAR0");
        return false;
    }
    DEBUG_LOG("BAR0 Phys: 0x%lx Size: 0x%lx", bar_phys, bar_size);
    
    // Map MMIO region properly
    uint64_t bar_virt = vmm_map_mmio(bar_phys, bar_size);
    if (bar_virt == 0) {
        DEBUG_ERROR("Error: MMIO mapping failed");
        return false;
    }
    
    // Setup capability registers pointer
    xhci.cap = (volatile XhciCapRegs*)bar_virt;
    
    // Perform BIOS Handoff BEFORE accessing other registers
    DEBUG_LOG("Requesting BIOS Handoff...");
    xhci_bios_handoff();
    DEBUG_LOG("BIOS Handoff complete");
    
    // Read capability length and setup other register pointers
    uint8_t cap_length = xhci.cap->caplength;
    xhci.op = (volatile XhciOpRegs*)(bar_virt + cap_length);
    xhci.runtime = (volatile XhciRuntimeRegs*)(bar_virt + xhci.cap->rtsoff);
    xhci.doorbell = (volatile uint32_t*)(bar_virt + xhci.cap->dboff);
    xhci.ports = (volatile XhciPortRegs*)(bar_virt + cap_length + 0x400);
    
    // Parse capability parameters
    uint32_t hcsparams1 = xhci.cap->hcsparams1;
    uint32_t hcsparams2 = xhci.cap->hcsparams2;
    uint32_t hccparams1 = xhci.cap->hccparams1;
    
    xhci.max_slots = HCSPARAMS1_MAX_SLOTS(hcsparams1);
    xhci.max_ports = HCSPARAMS1_MAX_PORTS(hcsparams1);
    xhci.max_intrs = HCSPARAMS1_MAX_INTRS(hcsparams1);
    xhci.context_size_64 = HCCPARAMS1_CSZ(hccparams1);
    
    DEBUG_LOG("Max Slots: %d, Max Ports: %d", xhci.max_slots, xhci.max_ports);
    
    // Reset controller
    DEBUG_LOG("Resetting Controller...");
    if (!xhci_reset()) {
        DEBUG_ERROR("Error: Controller reset failed");
        return false;
    }
    
    // Wait for controller ready
    uint32_t timeout = 100000;
    while ((mmio_read32((void*)&xhci.op->usbsts) & USBSTS_CNR) && timeout--) {
        io_wait();
    }
    if (timeout == 0) {
        DEBUG_ERROR("Error: Controller not ready (CNR)");
        return false;
    }
    
    // Configure max slots
    mmio_write32((void*)&xhci.op->config, xhci.max_slots);
    
    // Allocate and setup DCBAA (Device Context Base Address Array)
    uint64_t dcbaa_size = (xhci.max_slots + 1) * sizeof(uint64_t);
    size_t dcbaa_pages = (dcbaa_size + 4095) / 4096;
    DMAAllocation dcbaa_dma = vmm_alloc_dma(dcbaa_pages);
    if (!dcbaa_dma.phys) return false;
    
    xhci.dcbaa_phys = dcbaa_dma.phys;
    xhci.dcbaa = (uint64_t*)dcbaa_dma.virt;
    zero_memory(xhci.dcbaa, dcbaa_size);
    
    // Setup scratchpad buffers if required
    uint32_t scratchpad_hi = HCSPARAMS2_MAX_SCRATCHPAD_HI(hcsparams2);
    uint32_t scratchpad_lo = HCSPARAMS2_MAX_SCRATCHPAD_LO(hcsparams2);
    uint32_t num_scratchpad = (scratchpad_hi << 5) | scratchpad_lo;
    
    if (num_scratchpad > 0) {
        // Allocate scratchpad buffer array
        uint64_t array_size = num_scratchpad * sizeof(uint64_t);
        size_t array_pages = (array_size + 4095) / 4096;
        DMAAllocation scratch_arr_dma = vmm_alloc_dma(array_pages);
        if (!scratch_arr_dma.phys) return false;
        
        xhci.scratchpad_array_phys = scratch_arr_dma.phys;
        xhci.scratchpad_array = (uint64_t*)scratch_arr_dma.virt;
        
        // Allocate individual scratchpad pages
        for (uint32_t i = 0; i < num_scratchpad; i++) {
            DMAAllocation page_dma = vmm_alloc_dma(1);
            if (!page_dma.phys) return false;
            xhci.scratchpad_array[i] = page_dma.phys;
        }
        
        // Point DCBAA[0] to scratchpad array
        xhci.dcbaa[0] = xhci.scratchpad_array_phys;
    }
    
    // Write DCBAAP
    mmio_write64((void*)&xhci.op->dcbaap, xhci.dcbaa_phys);
    
    // Allocate and setup Command Ring
    uint64_t cmd_ring_size = XHCI_RING_SIZE * sizeof(Trb);
    size_t cmd_ring_pages = (cmd_ring_size + 4095) / 4096;
    DMAAllocation cmd_ring_dma = vmm_alloc_dma(cmd_ring_pages);
    if (!cmd_ring_dma.phys) return false;
    
    xhci.cmd_ring_phys = cmd_ring_dma.phys;
    xhci.cmd_ring = (Trb*)cmd_ring_dma.virt;
    zero_memory(xhci.cmd_ring, cmd_ring_size);
    xhci.cmd_enqueue = 0;
    xhci.cmd_cycle = 1;
    
    // Setup Link TRB at end of command ring
    Trb* link_trb = &xhci.cmd_ring[XHCI_RING_SIZE - 1];
    link_trb->parameter = xhci.cmd_ring_phys;
    link_trb->status = 0;
    // Link TRB needs cycle bit set to match producer cycle, plus Toggle Cycle
    link_trb->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | xhci.cmd_cycle;
    
    // Write CRCR (Command Ring Control Register) - just the physical address, cycle bit is in bit 0
    uint64_t crcr = xhci.cmd_ring_phys | xhci.cmd_cycle;
    mmio_write64((void*)&xhci.op->crcr, crcr);
    
    // Allocate and setup Event Ring
    uint64_t event_ring_size = XHCI_EVENT_RING_SIZE * sizeof(Trb);
    size_t event_ring_pages = (event_ring_size + 4095) / 4096;
    DMAAllocation event_ring_dma = vmm_alloc_dma(event_ring_pages);
    if (!event_ring_dma.phys) return false;
    
    xhci.event_ring_phys = event_ring_dma.phys;
    xhci.event_ring = (Trb*)event_ring_dma.virt;
    zero_memory(xhci.event_ring, event_ring_size);
    xhci.event_dequeue = 0;
    xhci.event_cycle = 1;
    
    // Allocate Event Ring Segment Table
    DMAAllocation erst_dma = vmm_alloc_dma(1);
    if (!erst_dma.phys) return false;
    
    xhci.erst_phys = erst_dma.phys;
    xhci.erst = (ErstEntry*)erst_dma.virt;
    xhci.erst[0].ring_segment_base = xhci.event_ring_phys;
    xhci.erst[0].ring_segment_size = XHCI_EVENT_RING_SIZE;
    xhci.erst[0].reserved = 0;
    
    // Setup primary interrupter (interrupter 0)
    volatile XhciInterrupterRegs* ir = (volatile XhciInterrupterRegs*)
        ((uint64_t)xhci.runtime + 0x20);  // Offset 0x20 for interrupter 0
    
    // Disable interrupter while setting up
    mmio_write32((void*)&ir->iman, 0);
    
    // Set Event Ring Segment Table Size (must be done before ERSTBA)
    mmio_write32((void*)&ir->erstsz, 1);
    
    // Set Event Ring Dequeue Pointer (must be done before ERSTBA, no EHB yet)
    mmio_write64((void*)&ir->erdp, xhci.event_ring_phys);
    
    // Set Event Ring Segment Table Base Address (writing this enables the ring)
    mmio_write64((void*)&ir->erstba, xhci.erst_phys);
    
    // Set interrupt moderation (4000 = 1ms)
    mmio_write32((void*)&ir->imod, 4000);
    
    // Enable interrupter
    mmio_write32((void*)&ir->iman, IMAN_IE);
    
    // Initialize device context arrays
    for (int i = 0; i < 256; i++) {
        xhci.device_contexts[i] = nullptr;
        xhci.input_contexts[i] = nullptr;
        for (int j = 0; j < 32; j++) {
            xhci.transfer_rings[i][j] = nullptr;
            xhci.intr_pending[i][j] = false;
            xhci.intr_complete[i][j] = false;
            xhci.intr_start_time[i][j] = 0;
        }
    }
    
    // Power on all ports
    DEBUG_LOG("Powering on ports...");
    for (uint8_t i = 0; i < xhci.max_ports; i++) {
        uint32_t portsc = mmio_read32((void*)&xhci.ports[i].portsc);
        if (!(portsc & PORTSC_PP)) {
            mmio_write32((void*)&xhci.ports[i].portsc, portsc | PORTSC_PP);
        }
    }
    
    // Wait for power to stabilize (increased to ~500ms for real hardware)
    DEBUG_LOG("Waiting for ports to power up...");
    for (volatile int i = 0; i < 50000000; i++);
    
    // Start controller
    DEBUG_LOG("Starting Controller...");
    if (!xhci_start()) {
        DEBUG_ERROR("Error: Controller start failed");
        return false;
    }
    
    DEBUG_INFO("xHCI Initialized Successfully");
    
    // Log initial port status (Raw)
    DEBUG_LOG("Scanning ports...");
    bool any_connected = false;
    for (uint8_t i = 0; i < xhci.max_ports; i++) {
        uint32_t portsc = mmio_read32((void*)&xhci.ports[i].portsc);
        
        // Log first 4 ports always, or any port with status bits set
        if (i < 4 || portsc != 0x2A0) { // 0x2A0 is typical empty state (PP=1, LWS=0, etc) - adjust as needed
             // Log raw PORTSC for debugging
             if (xhci_debug && (i < 4 || (portsc & PORTSC_CCS))) {
                 DEBUG_LOG("Port %d: SC=0x%x PP=%d CCS=%d", i+1, portsc, (portsc & PORTSC_PP) ? 1 : 0, (portsc & PORTSC_CCS) ? 1 : 0);
             }
        }
        
        if (portsc & PORTSC_CCS) {
            any_connected = true;
            if (xhci_debug) DEBUG_LOG("  -> Connected! Speed: %d", (portsc & PORTSC_SPEED_MASK) >> 10);
        }
    }
    
    if (!any_connected) {
        DEBUG_WARN("Warning: No devices detected on any port!");
    }
    
    xhci_initialized = true;
    return true;
}

bool xhci_reset() {
    // Stop controller first
    uint32_t cmd = mmio_read32((void*)&xhci.op->usbcmd);
    cmd &= ~USBCMD_RS;
    mmio_write32((void*)&xhci.op->usbcmd, cmd);
    
    // Wait for halt
    uint32_t timeout = 100000;
    while (!(mmio_read32((void*)&xhci.op->usbsts) & USBSTS_HCH) && timeout--) {
        io_wait();
    }
    if (timeout == 0) return false;
    
    // Reset controller
    cmd = mmio_read32((void*)&xhci.op->usbcmd);
    cmd |= USBCMD_HCRST;
    mmio_write32((void*)&xhci.op->usbcmd, cmd);
    
    // Wait for reset complete
    timeout = 100000;
    while ((mmio_read32((void*)&xhci.op->usbcmd) & USBCMD_HCRST) && timeout--) {
        io_wait();
    }
    return timeout > 0;
}

bool xhci_start() {
    uint32_t cmd = mmio_read32((void*)&xhci.op->usbcmd);
    cmd |= USBCMD_RS | USBCMD_INTE;
    mmio_write32((void*)&xhci.op->usbcmd, cmd);
    
    // Wait for running
    uint32_t timeout = 100000;
    while ((mmio_read32((void*)&xhci.op->usbsts) & USBSTS_HCH) && timeout--) {
        io_wait();
    }
    return timeout > 0;
}

void xhci_stop() {
    uint32_t cmd = mmio_read32((void*)&xhci.op->usbcmd);
    cmd &= ~USBCMD_RS;
    mmio_write32((void*)&xhci.op->usbcmd, cmd);
}

// Ring the doorbell for a slot/endpoint
static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target) {
    mmio_write32((void*)&xhci.doorbell[slot_id], target);
}

// Enqueue a TRB to the command ring
static void xhci_enqueue_command(Trb* trb) {
    Trb* dest = &xhci.cmd_ring[xhci.cmd_enqueue];
    
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | xhci.cmd_cycle;
    
    xhci.cmd_enqueue++;
    
    // Check for link TRB
    if (xhci.cmd_enqueue >= XHCI_RING_SIZE - 1) {
        // Link TRB must have the CURRENT cycle bit to be executed
        Trb* link = &xhci.cmd_ring[XHCI_RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | xhci.cmd_cycle;
        
        xhci.cmd_cycle ^= 1;
        xhci.cmd_enqueue = 0;
    }
}

// Send a command and wait for completion
static bool xhci_send_command(Trb* trb, Trb* result) {
    xhci_enqueue_command(trb);
    
    // Memory barrier to ensure TRB is written before doorbell
    asm volatile("mfence" ::: "memory");
    
    xhci_ring_doorbell(0, 0);  // Host controller doorbell
    return xhci_wait_command_completion(result, 1000);
}

// Wait for command completion event
static bool xhci_wait_command_completion(Trb* result, uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms * 1000;
    
    while (timeout--) {
        Trb* event = &xhci.event_ring[xhci.event_dequeue];
        uint32_t control = event->control;
        
        // Check cycle bit matches expected
        if ((control & TRB_CYCLE) == xhci.event_cycle) {
            uint8_t type = TRB_GET_TYPE(control);
            
            // Advance dequeue pointer for any event
            xhci.event_dequeue++;
            if (xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
                xhci.event_dequeue = 0;
                xhci.event_cycle ^= 1;
            }
            
            // Update ERDP
            volatile XhciInterrupterRegs* ir = (volatile XhciInterrupterRegs*)
                ((uint64_t)xhci.runtime + 0x20);
            uint64_t erdp = xhci.event_ring_phys + xhci.event_dequeue * sizeof(Trb);
            mmio_write64((void*)&ir->erdp, erdp | (1 << 3));  // EHB bit
            
            if (type == TRB_TYPE_COMMAND_COMPLETION) {
                if (result) {
                    result->parameter = event->parameter;
                    result->status = event->status;
                    result->control = event->control;
                }
                
                // Check completion code
                uint8_t comp_code = (event->status >> 24) & 0xFF;
                return comp_code == TRB_COMP_SUCCESS;
            }
            // For other event types (like port status change), continue polling
        }
        
        io_wait();
    }
    
    return false;
}

// Port operations
uint8_t xhci_get_port_speed(uint8_t port) {
    if (port == 0 || port > xhci.max_ports) return 0;
    uint32_t portsc = mmio_read32((void*)&xhci.ports[port - 1].portsc);
    return (portsc & PORTSC_SPEED_MASK) >> 10;
}

bool xhci_port_connected(uint8_t port) {
    if (port == 0 || port > xhci.max_ports) return false;
    uint32_t portsc = mmio_read32((void*)&xhci.ports[port - 1].portsc);
    return (portsc & PORTSC_CCS) != 0;
}

bool xhci_reset_port(uint8_t port) {
    if (port == 0 || port > xhci.max_ports) return false;
    
    volatile XhciPortRegs* p = &xhci.ports[port - 1];
    uint32_t portsc = mmio_read32((void*)&p->portsc);
    
    // Check if port is connected
    if (!(portsc & PORTSC_CCS)) return false;
    
    // 1. Clear all change bits (RW1C) to ensure we catch the NEW reset change
    // We write 1 to the change bits to clear them.
    // Also preserve PP (Port Power).
    uint32_t clear_bits = (portsc & PORTSC_CHANGE_MASK);
    mmio_write32((void*)&p->portsc, clear_bits | PORTSC_PP);
    
    // 2. Initiate Reset (PR = 1)
    // Note: We do NOT write 1 to change bits here, or we might clear a new event.
    // We just want to set PR and keep PP.
    portsc = mmio_read32((void*)&p->portsc);
    mmio_write32((void*)&p->portsc, (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_PR | PORTSC_PP);
    
    // 3. Wait for reset complete (PRC = 1)
    // Real hardware might take some time.
    uint32_t timeout = 1000000;
    while (timeout--) {
        portsc = mmio_read32((void*)&p->portsc);
        if (portsc & PORTSC_PRC) break;
        io_wait();
    }
    
    if (timeout == 0) {
        DEBUG_ERROR("Error: Port %d reset timeout (PORTSC=0x%x)", port, portsc);
        return false;
    }
    
    // 4. Clear PRC (RW1C) and other change bits that might have set
    clear_bits = (portsc & PORTSC_CHANGE_MASK);
    mmio_write32((void*)&p->portsc, clear_bits | PORTSC_PP);
    
    // 5. Check if enabled (PED = 1)
    portsc = mmio_read32((void*)&p->portsc);
    if (portsc & PORTSC_PED) {
        return true;
    } else {
        DEBUG_ERROR("Error: Port %d enabled check failed (PORTSC=0x%x)", port, portsc);
        return false;
    }
}

// Slot operations
int xhci_enable_slot() {
    Trb cmd = {0};
    cmd.control = TRB_TYPE(TRB_TYPE_ENABLE_SLOT);
    
    Trb result;
    if (!xhci_send_command(&cmd, &result)) {
        return -1;
    }
    
    // Slot ID is in bits 24-31 of control
    return (result.control >> 24) & 0xFF;
}

bool xhci_disable_slot(uint8_t slot_id) {
    Trb cmd = {0};
    cmd.control = TRB_TYPE(TRB_TYPE_DISABLE_SLOT) | ((uint32_t)slot_id << 24);
    
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_address_device(uint8_t slot_id, uint8_t port, uint8_t speed) {
    // Allocate device context - use 64-byte contexts if CSZ is set
    uint64_t ctx_size = xhci.context_size_64 ? 64 : 32;
    uint64_t dev_ctx_size = ctx_size * 32;  // Slot + 31 endpoints
    
    size_t dev_ctx_pages = (dev_ctx_size + 4095) / 4096;
    DMAAllocation dev_ctx_dma = vmm_alloc_dma(dev_ctx_pages);
    if (!dev_ctx_dma.phys) return false;
    
    DeviceContext* dev_ctx = (DeviceContext*)dev_ctx_dma.virt;
    zero_memory(dev_ctx, dev_ctx_size);
    xhci.device_contexts[slot_id] = dev_ctx;
    
    // Point DCBAA to device context
    xhci.dcbaa[slot_id] = dev_ctx_dma.phys;
    
    // Allocate input context - Control context + Slot + 31 endpoints
    uint64_t input_ctx_size = ctx_size * 33;
    size_t input_ctx_pages = (input_ctx_size + 4095) / 4096;
    DMAAllocation input_ctx_dma = vmm_alloc_dma(input_ctx_pages);
    if (!input_ctx_dma.phys) return false;
    
    InputContext* input_ctx = (InputContext*)input_ctx_dma.virt;
    zero_memory(input_ctx, input_ctx_size);
    xhci.input_contexts[slot_id] = input_ctx;
    
    // Setup input control context - add slot context (A0) and EP0 (A1)
    input_ctx->control.drop_flags = 0;
    input_ctx->control.add_flags = (1 << 0) | (1 << 1);  // Add slot and EP0
    
    // Setup slot context
    // Bits 0-19: Route String (0 for devices directly connected to root hub)
    // Bits 20-23: Speed
    // Bits 27-31: Context Entries (must be 1 for just EP0)
    uint32_t route_string = 0;  // Direct connection to root hub
    input_ctx->slot.route_speed_entries = 
        (route_string & 0xFFFFF) |         // Route string bits 0-19
        ((uint32_t)speed << 20) |          // Speed bits 20-23
        (1 << 27);                         // Context entries = 1 (just EP0)
    
    // Slot context dword 1: Root Hub Port Number in bits 16-23
    input_ctx->slot.latency_hub_port = ((uint32_t)port << 16);
    
    // Allocate transfer ring for EP0
    uint64_t tr_size = XHCI_RING_SIZE * sizeof(Trb);
    size_t tr_pages = (tr_size + 4095) / 4096;
    DMAAllocation tr_dma = vmm_alloc_dma(tr_pages);
    if (!tr_dma.phys) return false;
    
    Trb* tr = (Trb*)tr_dma.virt;
    zero_memory(tr, tr_size);
    xhci.transfer_rings[slot_id][0] = tr;
    xhci.transfer_ring_phys[slot_id][0] = tr_dma.phys;
    xhci.transfer_enqueue[slot_id][0] = 0;
    xhci.transfer_cycle[slot_id][0] = 1;
    
    // Setup link TRB for transfer ring
    Trb* link = &tr[XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | 1;  // TC + initial cycle bit
    
    // Setup EP0 endpoint context
    // For control endpoints, max packet size depends on speed
    uint16_t max_packet;
    switch (speed) {
        case PORTSC_SPEED_LS: max_packet = 8; break;
        case PORTSC_SPEED_FS: max_packet = 8; break;  // Start with 8, get real size from device descriptor
        case PORTSC_SPEED_HS: max_packet = 64; break;
        case PORTSC_SPEED_SS: max_packet = 512; break;
        default: max_packet = 8; break;
    }
    
    // EP0 Context:
    // ep_state (dword 0): Interval=0 for control, LSA=0, MaxPStreams=0, Mult=0, EPState=0
    input_ctx->endpoints[0].ep_state = 0;
    
    // ep_info (dword 1):
    // Bits 1-2: CErr (error count) = 3
    // Bits 3-5: EP Type = 4 (control bidirectional)
    // Bits 8-15: Max Burst Size = 0
    // Bits 16-31: Max Packet Size
    input_ctx->endpoints[0].ep_info = 
        (3 << 1) |                   // CErr = 3
        (4 << 3) |                   // EP Type = 4 (control bidirectional)
        (0 << 8) |                   // Max Burst Size = 0
        ((uint32_t)max_packet << 16);// Max Packet Size
    
    // TR Dequeue pointer with DCS (Dequeue Cycle State) in bit 0
    input_ctx->endpoints[0].tr_dequeue = tr_dma.phys | 1;  // DCS = 1
    
    // Average TRB length - 8 for control transfers
    input_ctx->endpoints[0].avg_trb_length = 8;
    
    // Send Address Device command
    Trb cmd = {0, 0, 0};
    cmd.parameter = input_ctx_dma.phys;
    cmd.control = TRB_TYPE(TRB_TYPE_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);
    
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_num, uint8_t ep_type, 
                             uint16_t max_packet, uint8_t interval) {
    if (!xhci.input_contexts[slot_id]) return false;
    
    InputContext* input_ctx = xhci.input_contexts[slot_id];
    
    // ep_num is the DCI (Device Context Index)
    // DCI 1 = EP0, DCI 2 = EP1 OUT, DCI 3 = EP1 IN, etc.
    // endpoints[] array is 0-indexed: endpoints[0] = EP0 = DCI 1
    // So to access endpoint for DCI N, use endpoints[N-1]
    uint8_t dci = ep_num;
    uint8_t ep_ctx_idx = dci - 1;  // Array index into endpoints[]
    
    // Clear input context
    input_ctx->control.drop_flags = 0;
    // Add flags: bit 0 = slot, bit N = DCI N
    input_ctx->control.add_flags = (1 << 0) | (1 << dci);
    
    // IMPORTANT: Copy current slot context from device context
    DeviceContext* dev_ctx = xhci.device_contexts[slot_id];
    input_ctx->slot = dev_ctx->slot;
    
    // Update context entries in slot (must be >= DCI of highest endpoint)
    uint8_t entries = (dev_ctx->slot.route_speed_entries >> 27) & 0x1F;
    if (dci > entries) {
        input_ctx->slot.route_speed_entries = 
            (dev_ctx->slot.route_speed_entries & 0x07FFFFFF) | 
            ((uint32_t)dci << 27);
    }
    
    // Allocate transfer ring for this endpoint (indexed by DCI for array)
    uint64_t tr_size = XHCI_RING_SIZE * sizeof(Trb);
    size_t tr_pages = (tr_size + 4095) / 4096;
    DMAAllocation tr_dma = vmm_alloc_dma(tr_pages);
    if (!tr_dma.phys) return false;
    
    Trb* tr = (Trb*)tr_dma.virt;
    zero_memory(tr, tr_size);
    xhci.transfer_rings[slot_id][dci] = tr;
    xhci.transfer_ring_phys[slot_id][dci] = tr_dma.phys;
    xhci.transfer_enqueue[slot_id][dci] = 0;
    xhci.transfer_cycle[slot_id][dci] = 1;
    
    // Setup link TRB
    Trb* link = &tr[XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | 1;  // TC + cycle bit
    
    // Setup endpoint context (use ep_ctx_idx for array access)
    input_ctx->endpoints[ep_ctx_idx].ep_state = 
        ((uint32_t)interval << 16);  // Interval
    input_ctx->endpoints[ep_ctx_idx].ep_info = 
        ((uint32_t)max_packet << 16) | 
        ((uint32_t)ep_type << 3) |
        (3 << 1);  // CErr = 3
    input_ctx->endpoints[ep_ctx_idx].tr_dequeue = 
        tr_dma.phys | xhci.transfer_cycle[slot_id][dci];
    input_ctx->endpoints[ep_ctx_idx].avg_trb_length = max_packet;
    
    // Get input context physical address
    // We need to find the physical address of the input context we allocated earlier.
    // Since we don't store the DMAAllocation for input contexts (only the pointer),
    // we need to use vmm_virt_to_phys.
    uint64_t input_ctx_phys = vmm_virt_to_phys((uint64_t)input_ctx);
    
    // Send Configure Endpoint command
    Trb cmd = {0};
    cmd.parameter = input_ctx_phys;
    cmd.control = TRB_TYPE(TRB_TYPE_CONFIG_EP) | ((uint32_t)slot_id << 24);
    
    Trb result;
    return xhci_send_command(&cmd, &result);
}

// Flush cache line
static void cache_flush(void* addr) {
    asm volatile("clflush (%0)" :: "r"(addr));
}

// Enqueue a TRB to a transfer ring
static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_index, Trb* trb) {
    Trb* ring = xhci.transfer_rings[slot_id][ep_index];
    uint32_t idx = xhci.transfer_enqueue[slot_id][ep_index];
    
    // Check if we need to handle the Link TRB (Wrap around)
    // If we are at the last TRB (Link TRB), we must toggle its cycle bit and wrap
    if (idx == XHCI_RING_SIZE - 1) {
        Trb* link = &ring[idx];
        // Link TRB must have the CURRENT cycle bit to be executed
        uint8_t current_cycle = xhci.transfer_cycle[slot_id][ep_index];
        link->control = (link->control & ~TRB_CYCLE) | current_cycle;
        cache_flush(link); // Ensure Link TRB update is visible
        
        xhci.transfer_cycle[slot_id][ep_index] ^= 1;
        xhci.transfer_enqueue[slot_id][ep_index] = 0;
        idx = 0;
    }
    
    uint8_t cycle = xhci.transfer_cycle[slot_id][ep_index];
    
    Trb* dest = &ring[idx];
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | cycle;
    
    cache_flush(dest); // Ensure TRB is visible to controller
    
    xhci.transfer_enqueue[slot_id][ep_index]++;
}

// Wait for a transfer event (timeout is number of iterations, not ms)
static bool xhci_wait_transfer_event(uint8_t slot_id, Trb* result, uint32_t iterations) {
    uint32_t timeout = iterations;
    
    while (timeout--) {
        Trb* event = &xhci.event_ring[xhci.event_dequeue];
        uint32_t control = event->control;
        
        if ((control & TRB_CYCLE) == xhci.event_cycle) {
            uint8_t type = TRB_GET_TYPE(control);
            
            // Advance dequeue for ANY matching event
            xhci.event_dequeue++;
            if (xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
                xhci.event_dequeue = 0;
                xhci.event_cycle ^= 1;
            }
            
            // Update ERDP
            volatile XhciInterrupterRegs* ir = (volatile XhciInterrupterRegs*)
                ((uint64_t)xhci.runtime + 0x20);
            uint64_t erdp = xhci.event_ring_phys + xhci.event_dequeue * sizeof(Trb);
            mmio_write64((void*)&ir->erdp, erdp | (1 << 3));
            
            if (type == TRB_TYPE_TRANSFER_EVENT) {
                if (result) {
                    result->parameter = event->parameter;
                    result->status = event->status;
                    result->control = event->control;
                }
                
                uint8_t comp_code = (event->status >> 24) & 0xFF;
                return comp_code == TRB_COMP_SUCCESS || comp_code == TRB_COMP_SHORT_PACKET;
            }
            // For non-transfer events, continue polling (event was consumed)
        }
        
        io_wait();
    }
    
    return false;
}

// Control transfer
bool xhci_control_transfer(uint8_t slot_id, uint8_t request_type, uint8_t request,
                          uint16_t value, uint16_t index, uint16_t length,
                          void* data, uint16_t* transferred) {
    if (!xhci.transfer_rings[slot_id][0]) return false;
    
    // Use static buffer to avoid memory leak (max 512 bytes for control transfers)
    static uint64_t static_data_phys = 0;
    static uint8_t* static_data_virt = nullptr;
    const uint16_t MAX_CONTROL_DATA = 512;
    
    uint64_t data_phys = 0;
    if (data && length > 0) {
        if (length > MAX_CONTROL_DATA) {
            return false; // Too large for static buffer
        }
        
        // Allocate static buffer on first use
        if (static_data_phys == 0) {
            size_t pages = (MAX_CONTROL_DATA + 4095) / 4096;
            DMAAllocation dma = vmm_alloc_dma(pages);
            if (!dma.phys) return false;
            
            static_data_phys = dma.phys;
            static_data_virt = (uint8_t*)dma.virt;
        }
        data_phys = static_data_phys;
        
        // Copy data for OUT transfers
        if (!(request_type & 0x80)) {
            uint8_t* src = (uint8_t*)data;
            for (uint16_t i = 0; i < length; i++) {
                static_data_virt[i] = src[i];
            }
        }
    }
    
    // Setup Stage TRB
    Trb setup = {0};
    setup.parameter = 
        ((uint64_t)request_type) |
        ((uint64_t)request << 8) |
        ((uint64_t)value << 16) |
        ((uint64_t)index << 32) |
        ((uint64_t)length << 48);
    setup.status = 8;  // TRB transfer length = 8 (setup packet size)
    setup.control = TRB_TYPE(TRB_TYPE_SETUP) | TRB_IDT;  // Immediate Data
    if (length > 0) {
        setup.control |= (request_type & 0x80) ? (3 << 16) : (2 << 16);  // TRT field
    }
    xhci_enqueue_transfer(slot_id, 0, &setup);
    
    // Data Stage TRB (if needed)
    if (length > 0) {
        Trb data_trb = {0};
        data_trb.parameter = data_phys;
        data_trb.status = length;
        data_trb.control = TRB_TYPE(TRB_TYPE_DATA);
        if (request_type & 0x80) {
            data_trb.control |= TRB_DIR_IN;
        }
        xhci_enqueue_transfer(slot_id, 0, &data_trb);
    }
    
    // Status Stage TRB
    Trb status_trb = {0};
    status_trb.control = TRB_TYPE(TRB_TYPE_STATUS) | TRB_IOC;
    // Direction is opposite of data stage (or IN if no data)
    if (!(request_type & 0x80) || length == 0) {
        status_trb.control |= TRB_DIR_IN;
    }
    xhci_enqueue_transfer(slot_id, 0, &status_trb);
    
    // Ring doorbell (EP0 = target 1)
    xhci_ring_doorbell(slot_id, 1);
    
    // Wait for completion
    Trb result;
    if (!xhci_wait_transfer_event(slot_id, &result, 500)) {
        return false;
    }
    
    // Copy data for IN transfers
    if (data && length > 0 && (request_type & 0x80)) {
        uint32_t actual_len = length - (result.status & 0xFFFFFF);
        for (uint32_t i = 0; i < actual_len; i++) {
            ((uint8_t*)data)[i] = static_data_virt[i];
        }
        if (transferred) *transferred = actual_len;
    }
    
    return true;
}

// Static interrupt transfer buffers - one per slot/endpoint
static DMAAllocation intr_buffer_dma[32][32] = {{{0}}};

// Interrupt transfer - non-blocking with pending state tracking
bool xhci_interrupt_transfer(uint8_t slot_id, uint8_t ep_num, void* data, 
                             uint16_t length, uint16_t* transferred) {
    if (!xhci.transfer_rings[slot_id][ep_num]) return false;
    
    // Check if a transfer just completed
    if (xhci.intr_complete[slot_id][ep_num]) {
        Trb result = xhci.transfer_result[slot_id][ep_num];
        xhci.intr_complete[slot_id][ep_num] = false;
        
        uint8_t comp_code = (result.status >> 24) & 0xFF;
        if (comp_code != TRB_COMP_SUCCESS && comp_code != TRB_COMP_SHORT_PACKET) {
            return false;
        }
        
        // Copy data
        if (intr_buffer_dma[slot_id][ep_num].virt) {
            uint8_t* src = (uint8_t*)intr_buffer_dma[slot_id][ep_num].virt;
            uint8_t* dst = (uint8_t*)data;
            uint32_t actual_len = length - (result.status & 0xFFFFFF);
            for (uint32_t i = 0; i < actual_len; i++) {
                dst[i] = src[i];
            }
            if (transferred) *transferred = actual_len;
            return true;
        }
        return false;
    }
    
    // Check if transfer is still pending
    if (xhci.intr_pending[slot_id][ep_num]) {
        // Check for timeout (e.g. 500ms)
        uint64_t now = timer_get_ticks();
        // 100Hz timer = 10ms per tick. 50 ticks = 500ms.
        if (now - xhci.intr_start_time[slot_id][ep_num] > 50) {
            if (xhci_debug) DEBUG_LOG("EP %d.%d timed out, resetting pending state", slot_id, ep_num);
            xhci.intr_pending[slot_id][ep_num] = false;
            // Fall through to queue new transfer
        } else {
            return false;
        }
    }
    
    // Start new transfer
    // Allocate buffer once per endpoint (reuse on subsequent calls)
    if (intr_buffer_dma[slot_id][ep_num].phys == 0) {
        DMAAllocation dma = vmm_alloc_dma(1); // 4KB is enough for interrupt
        if (!dma.phys) return false;
        intr_buffer_dma[slot_id][ep_num] = dma;
    }
    uint64_t data_phys = intr_buffer_dma[slot_id][ep_num].phys;
    
    Trb trb = {0, 0, 0};
    trb.parameter = data_phys;
    trb.status = length;
    trb.control = TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP;
    xhci_enqueue_transfer(slot_id, ep_num, &trb);
    
    // Removed spam log - was printing thousands of times per second
    // usb_log("Queuing Int Transfer: Slot %d EP %d", slot_id, ep_num);
    
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, ep_num);
    xhci.intr_pending[slot_id][ep_num] = true;
    xhci.intr_start_time[slot_id][ep_num] = timer_get_ticks();
    
    return false;
}

// Poll for events (non-blocking) - Central Event Dispatcher
void xhci_poll_events() {
    // Process up to 64 events per call to avoid starving other tasks
    // Increased from 16 to handle high-rate mouse updates better
    int count = 0;
    
    while (count++ < 64) {
        Trb* event = &xhci.event_ring[xhci.event_dequeue];
        uint32_t control = event->control;
        
        if ((control & TRB_CYCLE) != xhci.event_cycle) {
            break;  // No more events
        }
        
        uint8_t type = TRB_GET_TYPE(control);
        
        // Handle Transfer Events
        if (type == TRB_TYPE_TRANSFER_EVENT) {
            uint8_t slot_id = (control >> 24) & 0xFF;
            uint8_t ep_num = (control >> 16) & 0x1F;
            uint8_t comp_code = (event->status >> 24) & 0xFF;
            
            // Debug log for mouse endpoint (DCI 5) or any error
            if (ep_num == 5 || comp_code != TRB_COMP_SUCCESS) {
                // DEBUG_LOG("Event: Slot %d EP %d Code %d", slot_id, ep_num, comp_code);
            }
            
            // If we are waiting for an interrupt transfer on this endpoint
            if (xhci.intr_pending[slot_id][ep_num]) {
                xhci.transfer_result[slot_id][ep_num] = *event;
                xhci.intr_complete[slot_id][ep_num] = true;
                xhci.intr_pending[slot_id][ep_num] = false;
                
                if (comp_code != TRB_COMP_SUCCESS && comp_code != TRB_COMP_SHORT_PACKET) {
                    endpoint_failures[slot_id][ep_num]++;
                    if (endpoint_failures[slot_id][ep_num] >= MAX_ENDPOINT_FAILURES) {
                        // Mark endpoint as potentially stuck - stop polling
                        if (xhci_debug) DEBUG_LOG("EP %d.%d stuck (code %d)", slot_id, ep_num, comp_code);
                        xhci.intr_pending[slot_id][ep_num] = false;
                    }
                } else {
                    endpoint_failures[slot_id][ep_num] = 0; // Reset on success
                }
            }
            // Note: Control transfers (xhci_wait_transfer_event) handle their own events
            // by polling directly. This dispatcher is mainly for async interrupt transfers.
        }
        else if (type == TRB_TYPE_PORT_STATUS_CHANGE) {
            // Port status change - acknowledge it by clearing change bits
            uint8_t port_id = (event->parameter >> 24) & 0xFF;
            if (port_id > 0 && port_id <= xhci.max_ports) {
                volatile uint32_t* portsc_reg = &xhci.ports[port_id - 1].portsc;
                uint32_t portsc = mmio_read32((void*)portsc_reg);
                
                // Clear change bits (17-23) by writing 1s, preserve PP (9)
                // Bits 17-23: CSC, PEC, WRC, OCC, PRC, PLC, CEC
                uint32_t change_mask = 0x00FE0000;
                
                // Write back with change bits set to clear them, and PP set to preserve power
                mmio_write32((void*)portsc_reg, (portsc & ~change_mask) | (portsc & change_mask) | PORTSC_PP);
            }
        }
        
        // Advance dequeue
        xhci.event_dequeue++;
        if (xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
            xhci.event_dequeue = 0;
            xhci.event_cycle ^= 1;
        }
        
        // Update ERDP
        volatile XhciInterrupterRegs* ir = (volatile XhciInterrupterRegs*)
            ((uint64_t)xhci.runtime + 0x20);
        uint64_t erdp = xhci.event_ring_phys + xhci.event_dequeue * sizeof(Trb);
        mmio_write64((void*)&ir->erdp, erdp | (1 << 3));
    }
}

bool xhci_wait_for_event(uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms * 1000;
    
    while (timeout--) {
        Trb* event = &xhci.event_ring[xhci.event_dequeue];
        if ((event->control & TRB_CYCLE) == xhci.event_cycle) {
            return true;
        }
        io_wait();
    }
    
    return false;
}

void xhci_dump_status() {
    // This would print debug info - not implemented for now
}
