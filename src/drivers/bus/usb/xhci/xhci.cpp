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

static XhciController g_xhci;
static bool g_xhci_initialized = false;
static Spinlock g_xhci_lock = SPINLOCK_INIT;

static DMAAllocation g_intr_buffer_dma[32][32] = {{{0, 0, 0}}};
static uint8_t g_xhci_irq = 0;
static uint8_t g_ep_failures[256][32] = {{0}};
static constexpr uint8_t MAX_EP_FAILURES = 5;

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target);
static bool xhci_send_command(Trb* trb, Trb* result);
static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_idx, Trb* trb);

uint8_t xhci_get_irq() { return g_xhci_irq; }

[[nodiscard]] const char* xhci_completion_code_str(uint8_t code) {
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

static void xhci_bios_handoff() {
    const uint32_t xecp_off = HCCPARAMS1_XECP(g_xhci.cap->hccparams1) << 2;
    if (!xecp_off) return;
    
    auto* xecp = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_xhci.cap) + xecp_off);
    
    for (int i = 0; i < 256; i++) {
        const uint32_t header = mmio_read32(const_cast<uint32_t*>(xecp));
        if ((header & 0xFF) == XECP_ID_LEGACY) {
            if (header & USBLEGSUP_BIOS_SEM) {
                KLOG(LogModule::Usb, LogLevel::Info, "BIOS owns xHCI, requesting handoff...");
                mmio_write32(const_cast<uint32_t*>(xecp), header | USBLEGSUP_OS_SEM);
                
                const uint64_t start = timer_get_ticks();
                while (mmio_read32(const_cast<uint32_t*>(xecp)) & USBLEGSUP_BIOS_SEM) {
                    if (timer_get_ticks() - start > 1000) {
                        DEBUG_WARN("BIOS handoff timeout, forcing ownership");
                        break;
                    }
                    io_wait();
                }
                
                volatile uint32_t* legctlsts = xecp + 1;
                mmio_write32(const_cast<uint32_t*>(legctlsts), (mmio_read32(const_cast<uint32_t*>(legctlsts)) & ~0xFFFF) | 0xE0000000);
                KLOG(LogModule::Usb, LogLevel::Success, "BIOS handoff complete");
            }
            return;
        }
        
        const uint8_t next = (header >> 8) & 0xFF;
        if (!next) break;
        xecp = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(xecp) + (next << 2));
    }
}

static void xhci_parse_protocols() {
    const uint32_t xecp_off = HCCPARAMS1_XECP(g_xhci.cap->hccparams1) << 2;
    if (!xecp_off) return;
    
    auto* xecp = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(g_xhci.cap) + xecp_off);
    
    for (int i = 0; i < 256; i++) {
        const uint32_t header = mmio_read32(const_cast<uint32_t*>(xecp));
        if ((header & 0xFF) == XECP_ID_PROTOCOLS) {
            const uint32_t rev = mmio_read32(const_cast<uint32_t*>(xecp + 0));
            const uint32_t ports = mmio_read32(const_cast<uint32_t*>(xecp + 2));
            
            const uint8_t major = (rev >> 24) & 0xFF;
            const uint8_t port_off = ports & 0xFF;
            const uint8_t port_cnt = (ports >> 8) & 0xFF;
            
            if (major == 2) {
                g_xhci.usb2_port_start = port_off;
                g_xhci.usb2_port_count = port_cnt;
            } else if (major == 3) {
                g_xhci.usb3_port_start = port_off;
                g_xhci.usb3_port_count = port_cnt;
            }
        }
        
        const uint8_t next = (header >> 8) & 0xFF;
        if (!next) break;
        xecp = reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(xecp) + (next << 2));
    }
}

bool xhci_init() {
    if (g_xhci_initialized) return true;
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
    g_xhci_irq = pci_dev.irq_line;

    if (g_xhci_irq > 0 && g_xhci_irq < 16) {
        if (g_xhci_irq >= 8) pic_clear_mask(2);
        pic_clear_mask(g_xhci_irq);
        DEBUG_INFO("xHCI IRQ %d unmasked", g_xhci_irq);
    } else {
        DEBUG_WARN("xHCI: Invalid or unsupported IRQ line %d", g_xhci_irq);
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
    
    g_xhci.cap = reinterpret_cast<volatile XhciCapRegs*>(bar_virt);
    xhci_bios_handoff();
    
    const uint8_t cap_len = g_xhci.cap->caplength;
    g_xhci.op = reinterpret_cast<volatile XhciOpRegs*>(bar_virt + cap_len);
    g_xhci.runtime = reinterpret_cast<volatile XhciRuntimeRegs*>(bar_virt + g_xhci.cap->rtsoff);
    g_xhci.doorbell = reinterpret_cast<volatile uint32_t*>(bar_virt + g_xhci.cap->dboff);
    g_xhci.ports = reinterpret_cast<volatile XhciPortRegs*>(bar_virt + cap_len + 0x400);
    
    const uint32_t hcsparams1 = g_xhci.cap->hcsparams1;
    const uint32_t hcsparams2 = g_xhci.cap->hcsparams2;
    const uint32_t hccparams1 = g_xhci.cap->hccparams1;
    
    g_xhci.max_slots = HCSPARAMS1_MAX_SLOTS(hcsparams1);
    g_xhci.max_ports = HCSPARAMS1_MAX_PORTS(hcsparams1);
    g_xhci.max_intrs = HCSPARAMS1_MAX_INTRS(hcsparams1);
    g_xhci.context_size_64 = HCCPARAMS1_CSZ(hccparams1);
    g_xhci.page_size = mmio_read32(const_cast<uint32_t*>(&g_xhci.op->pagesize)) << 12;
    g_xhci.num_scratchpad = HCSPARAMS2_MAX_SCRATCHPAD(hcsparams2);
    
    KLOG(LogModule::Usb, LogLevel::Trace, "MaxSlots=%d MaxPorts=%d PageSize=%d CSZ=%d",
         g_xhci.max_slots, g_xhci.max_ports, g_xhci.page_size,
         g_xhci.context_size_64 ? 64 : 32);
    
    xhci_parse_protocols();
    
    if (!xhci_reset()) {
        DEBUG_ERROR("Controller reset failed");
        return false;
    }
    
    uint32_t timeout = 500000;
    while ((mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbsts)) & USBSTS_CNR) && timeout--) {
        io_wait();
    }
    if (!timeout) {
        DEBUG_ERROR("Controller not ready");
        return false;
    }
    
    mmio_write32(const_cast<uint32_t*>(&g_xhci.op->config), g_xhci.max_slots);
    
    const size_t dcbaa_size = (g_xhci.max_slots + 1) * sizeof(uint64_t);
    const DMAAllocation dcbaa_dma = vmm_alloc_dma((dcbaa_size + 4095) / 4096);
    if (!dcbaa_dma.phys) return false;
    
    g_xhci.dcbaa = reinterpret_cast<uint64_t*>(dcbaa_dma.virt);
    g_xhci.dcbaa_phys = dcbaa_dma.phys;
    kstring::zero_memory(g_xhci.dcbaa, dcbaa_size);
    
    if (g_xhci.num_scratchpad > 0) {
        const DMAAllocation arr_dma = vmm_alloc_dma(1);
        if (!arr_dma.phys) return false;
        
        g_xhci.scratchpad_array = reinterpret_cast<uint64_t*>(arr_dma.virt);
        g_xhci.scratchpad_array_phys = arr_dma.phys;
        
        for (uint32_t i = 0; i < g_xhci.num_scratchpad; i++) {
            const DMAAllocation pg = vmm_alloc_dma(1);
            if (!pg.phys) return false;
            g_xhci.scratchpad_array[i] = pg.phys;
        }
        g_xhci.dcbaa[0] = g_xhci.scratchpad_array_phys;
    }
    
    mmio_write64(const_cast<uint64_t*>(&g_xhci.op->dcbaap), g_xhci.dcbaa_phys);
    
    const DMAAllocation cmd_dma = vmm_alloc_dma(1);
    if (!cmd_dma.phys) return false;
    
    g_xhci.cmd_ring = reinterpret_cast<Trb*>(cmd_dma.virt);
    g_xhci.cmd_ring_phys = cmd_dma.phys;
    kstring::zero_memory(g_xhci.cmd_ring, XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.cmd_enqueue = 0;
    g_xhci.cmd_cycle = 1;
    
    Trb* link = &g_xhci.cmd_ring[XHCI_RING_SIZE - 1];
    link->parameter = g_xhci.cmd_ring_phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
    
    mmio_write64(const_cast<uint64_t*>(&g_xhci.op->crcr), g_xhci.cmd_ring_phys | CRCR_RCS);
    
    const DMAAllocation evt_dma = vmm_alloc_dma(1);
    if (!evt_dma.phys) return false;
    
    g_xhci.event_ring = reinterpret_cast<Trb*>(evt_dma.virt);
    g_xhci.event_ring_phys = evt_dma.phys;
    kstring::zero_memory(g_xhci.event_ring, XHCI_EVENT_RING_SIZE * sizeof(Trb));
    g_xhci.event_dequeue = 0;
    g_xhci.event_cycle = 1;
    
    const DMAAllocation erst_dma = vmm_alloc_dma(1);
    if (!erst_dma.phys) return false;
    
    g_xhci.erst = reinterpret_cast<ErstEntry*>(erst_dma.virt);
    g_xhci.erst_phys = erst_dma.phys;
    g_xhci.erst[0].ring_segment_base = g_xhci.event_ring_phys;
    g_xhci.erst[0].ring_segment_size = XHCI_EVENT_RING_SIZE;
    g_xhci.erst[0].reserved = 0;
    
    auto* ir = reinterpret_cast<volatile XhciInterrupterRegs*>(reinterpret_cast<uintptr_t>(g_xhci.runtime) + 0x20);
    
    mmio_write32(const_cast<uint32_t*>(&ir->iman), 0);
    mmio_write32(const_cast<uint32_t*>(&ir->erstsz), 1);
    mmio_write64(const_cast<uint64_t*>(&ir->erdp), g_xhci.event_ring_phys);
    mmio_write64(const_cast<uint64_t*>(&ir->erstba), g_xhci.erst_phys);
    mmio_write32(const_cast<uint32_t*>(&ir->imod), 4000);
    mmio_write32(const_cast<uint32_t*>(&ir->iman), IMAN_IE);
    
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        g_xhci.device_contexts[i] = nullptr;
        g_xhci.input_contexts[i] = nullptr;
        for (int j = 0; j < XHCI_MAX_ENDPOINTS; j++) {
            g_xhci.transfer_rings[i][j] = nullptr;
            g_xhci.intr_pending[i][j] = false;
            g_xhci.intr_complete[i][j] = false;
        }
    }
    
    for (uint8_t i = 0; i < g_xhci.max_ports; i++) {
        const uint32_t portsc = mmio_read32(const_cast<uint32_t*>(&g_xhci.ports[i].portsc));
        if (!(portsc & PORTSC_PP)) {
            mmio_write32(const_cast<uint32_t*>(&g_xhci.ports[i].portsc), (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_PP);
        }
    }
    
    sleep(100);
    
    if (!xhci_start()) {
        DEBUG_ERROR("Controller start failed");
        return false;
    }
    
    g_xhci_initialized = true;
    DEBUG_INFO("xHCI initialized successfully");
    return true;
}

bool xhci_reset() {
    uint32_t cmd = mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbcmd));
    cmd &= ~USBCMD_RS;
    mmio_write32(const_cast<uint32_t*>(&g_xhci.op->usbcmd), cmd);
    
    uint32_t timeout = 500000;
    while (!(mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbsts)) & USBSTS_HCH) && timeout--) io_wait();
    if (!timeout) return false;
    
    cmd = mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbcmd));
    mmio_write32(const_cast<uint32_t*>(&g_xhci.op->usbcmd), cmd | USBCMD_HCRST);
    
    timeout = 500000;
    while ((mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbcmd)) & USBCMD_HCRST) && timeout--) io_wait();
    return timeout > 0;
}

bool xhci_start() {
    const uint32_t cmd = mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbcmd));
    mmio_write32(const_cast<uint32_t*>(&g_xhci.op->usbcmd), cmd | USBCMD_RS | USBCMD_INTE);
    
    uint32_t timeout = 100000;
    while ((mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbsts)) & USBSTS_HCH) && timeout--) io_wait();
    return timeout > 0;
}

void xhci_stop() {
    const uint32_t cmd = mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbcmd));
    mmio_write32(const_cast<uint32_t*>(&g_xhci.op->usbcmd), cmd & ~USBCMD_RS);
}

bool xhci_is_initialized() { return g_xhci_initialized; }
uint8_t xhci_get_max_ports() { return g_xhci_initialized ? g_xhci.max_ports : 0; }

uint8_t xhci_get_port_protocol(uint8_t port) {
    if (port >= g_xhci.usb3_port_start && port < g_xhci.usb3_port_start + g_xhci.usb3_port_count) return 3;
    return 2;
}

uint8_t xhci_get_port_speed(uint8_t port) {
    if (port == 0 || port > g_xhci.max_ports) return 0;
    const uint32_t portsc = mmio_read32(const_cast<uint32_t*>(&g_xhci.ports[port - 1].portsc));
    return (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
}

bool xhci_port_connected(uint8_t port) {
    if (port == 0 || port > g_xhci.max_ports) return false;
    return (mmio_read32(const_cast<uint32_t*>(&g_xhci.ports[port - 1].portsc)) & PORTSC_CCS) != 0;
}

bool xhci_reset_port(uint8_t port) {
    if (port == 0 || port > g_xhci.max_ports) return false;
    
    volatile XhciPortRegs* p = &g_xhci.ports[port - 1];
    uint32_t portsc = mmio_read32(const_cast<uint32_t*>(&p->portsc));
    if (!(portsc & PORTSC_CCS)) return false;
    
    mmio_write32(const_cast<uint32_t*>(&p->portsc), (portsc & PORTSC_CHANGE_MASK) | PORTSC_PP);
    const bool is_usb3 = xhci_get_port_protocol(port) == 3;
    
    portsc = mmio_read32(const_cast<uint32_t*>(&p->portsc));
    if (is_usb3 && (portsc & PORTSC_CCS) && !(portsc & PORTSC_PED)) {
        mmio_write32(const_cast<uint32_t*>(&p->portsc), (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_WPR | PORTSC_PP);
    } else {
        mmio_write32(const_cast<uint32_t*>(&p->portsc), (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_PR | PORTSC_PP);
    }
    
    uint32_t timeout = 500000;
    while (timeout--) {
        portsc = mmio_read32(const_cast<uint32_t*>(&p->portsc));
        if (portsc & PORTSC_PRC) break;
        io_wait();
    }
    
    if (!timeout) {
        DEBUG_ERROR("Port %d reset timeout", port);
        return false;
    }
    
    mmio_write32(const_cast<uint32_t*>(&p->portsc), (portsc & PORTSC_CHANGE_MASK) | PORTSC_PP);
    return (mmio_read32(const_cast<uint32_t*>(&p->portsc)) & PORTSC_PED) != 0;
}

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target) {
    mmio_write32(const_cast<uint32_t*>(&g_xhci.doorbell[slot_id]), target);
}

static void xhci_enqueue_command(Trb* trb) {
    Trb* dest = &g_xhci.cmd_ring[g_xhci.cmd_enqueue];
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | g_xhci.cmd_cycle;
    
    if (++g_xhci.cmd_enqueue >= XHCI_RING_SIZE - 1) {
        Trb* link = &g_xhci.cmd_ring[XHCI_RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | g_xhci.cmd_cycle;
        g_xhci.cmd_cycle ^= 1;
        g_xhci.cmd_enqueue = 0;
    }
}

static bool xhci_wait_command(Trb* result, uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        Trb* evt = &g_xhci.event_ring[g_xhci.event_dequeue];
        if ((evt->control & TRB_CYCLE) == g_xhci.event_cycle) {
            const uint8_t type = TRB_GET_TYPE(evt->control);
            if (++g_xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
                g_xhci.event_dequeue = 0;
                g_xhci.event_cycle ^= 1;
            }
            
            auto* ir = reinterpret_cast<volatile XhciInterrupterRegs*>(reinterpret_cast<uintptr_t>(g_xhci.runtime) + 0x20);
            mmio_write64(const_cast<uint64_t*>(&ir->erdp), (g_xhci.event_ring_phys + g_xhci.event_dequeue * sizeof(Trb)) | ERDP_EHB);
            
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
    Trb* ring = g_xhci.transfer_rings[slot_id][ep_idx];
    uint32_t idx = g_xhci.transfer_enqueue[slot_id][ep_idx];
    
    if (idx >= XHCI_RING_SIZE - 1) {
        Trb* link = &ring[XHCI_RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | g_xhci.transfer_cycle[slot_id][ep_idx];
        g_xhci.transfer_cycle[slot_id][ep_idx] ^= 1;
        idx = g_xhci.transfer_enqueue[slot_id][ep_idx] = 0;
    }
    
    Trb* dest = &ring[idx];
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | g_xhci.transfer_cycle[slot_id][ep_idx];
    g_xhci.transfer_enqueue[slot_id][ep_idx]++;
}

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
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_DISABLE_SLOT) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_address_device(uint8_t slot_id, uint8_t port, uint8_t speed) {
    const size_t ctx_size = g_xhci.context_size_64 ? 64 : 32;
    const size_t dev_ctx_bytes = ctx_size * 32;
    
    const DMAAllocation dev_dma = vmm_alloc_dma((dev_ctx_bytes + 4095) / 4096);
    if (!dev_dma.phys) return false;
    
    g_xhci.device_contexts[slot_id] = reinterpret_cast<DeviceContext*>(dev_dma.virt);
    g_xhci.device_context_phys[slot_id] = dev_dma.phys;
    kstring::zero_memory(g_xhci.device_contexts[slot_id], dev_ctx_bytes);
    g_xhci.dcbaa[slot_id] = dev_dma.phys;
    
    const size_t input_ctx_bytes = ctx_size * 33;
    const DMAAllocation input_dma = vmm_alloc_dma((input_ctx_bytes + 4095) / 4096);
    if (!input_dma.phys) return false;
    
    g_xhci.input_contexts[slot_id] = reinterpret_cast<InputContext*>(input_dma.virt);
    g_xhci.input_context_phys[slot_id] = input_dma.phys;
    kstring::zero_memory(g_xhci.input_contexts[slot_id], input_ctx_bytes);
    
    auto* input = g_xhci.input_contexts[slot_id];
    input->control.add_flags = (1 << 0) | (1 << 1);
    input->slot.route_speed_entries = (static_cast<uint32_t>(speed) << 20) | (1 << 27);
    input->slot.latency_hub_port = (static_cast<uint32_t>(port) << 16);
    
    const DMAAllocation tr_dma = vmm_alloc_dma(1);
    if (!tr_dma.phys) return false;
    
    g_xhci.transfer_rings[slot_id][0] = reinterpret_cast<Trb*>(tr_dma.virt);
    g_xhci.transfer_ring_phys[slot_id][0] = tr_dma.phys;
    kstring::zero_memory(g_xhci.transfer_rings[slot_id][0], XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.transfer_enqueue[slot_id][0] = 0;
    g_xhci.transfer_cycle[slot_id][0] = 1;
    
    Trb* link = &g_xhci.transfer_rings[slot_id][0][XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
    
    uint16_t max_pkt;
    switch (speed) {
        case XHCI_SPEED_LOW: case XHCI_SPEED_FULL: max_pkt = 8; break;
        case XHCI_SPEED_HIGH: max_pkt = 64; break;
        default: max_pkt = 512; break;
    }
    
    input->endpoints[0].ep_info = (3 << 1) | (EP_TYPE_CONTROL << 3) | (static_cast<uint32_t>(max_pkt) << 16);
    input->endpoints[0].tr_dequeue = tr_dma.phys | 1;
    input->endpoints[0].avg_trb_length = 8;
    
    Trb cmd = {g_xhci.input_context_phys[slot_id], 0, TRB_TYPE(TRB_TYPE_ADDRESS_DEVICE) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;
    
    if (!xhci_send_command(&cmd, &result)) {
        DEBUG_ERROR("Address Device failed: %s", xhci_completion_code_str((result.status >> 24) & 0xFF));
        return false;
    }
    return true;
}

bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_num, uint8_t ep_type, uint16_t max_packet, uint8_t interval) {
    if (!g_xhci.input_contexts[slot_id]) return false;
    
    auto* input = g_xhci.input_contexts[slot_id];
    auto* dev = g_xhci.device_contexts[slot_id];
    const uint8_t dci = ep_num;
    const uint8_t ep_idx = dci - 1;
    
    input->control.add_flags = (1 << 0) | (1 << dci);
    input->slot = dev->slot;
    
    if (dci > ((dev->slot.route_speed_entries >> 27) & 0x1F)) {
        input->slot.route_speed_entries = (dev->slot.route_speed_entries & 0x07FFFFFF) | (static_cast<uint32_t>(dci) << 27);
    }
    
    const DMAAllocation tr_dma = vmm_alloc_dma(1);
    if (!tr_dma.phys) return false;
    
    g_xhci.transfer_rings[slot_id][dci] = reinterpret_cast<Trb*>(tr_dma.virt);
    g_xhci.transfer_ring_phys[slot_id][dci] = tr_dma.phys;
    kstring::zero_memory(g_xhci.transfer_rings[slot_id][dci], XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.transfer_enqueue[slot_id][dci] = 0;
    g_xhci.transfer_cycle[slot_id][dci] = 1;
    
    Trb* link = &g_xhci.transfer_rings[slot_id][dci][XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
    
    const uint8_t speed = (dev->slot.route_speed_entries >> 20) & 0xF;
    uint8_t xhci_interval = (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) ? ((interval < 1) ? 3 : (interval + 2)) : ((interval < 1) ? 0 : (interval - 1));
    if (xhci_interval > 15) xhci_interval = 15;
    
    input->endpoints[ep_idx].ep_state = (static_cast<uint32_t>(xhci_interval) << 16);
    input->endpoints[ep_idx].ep_info = (3 << 1) | (static_cast<uint32_t>(ep_type) << 3) | (static_cast<uint32_t>(max_packet) << 16);
    input->endpoints[ep_idx].tr_dequeue = tr_dma.phys | 1;
    input->endpoints[ep_idx].avg_trb_length = max_packet;
    
    Trb cmd = {g_xhci.input_context_phys[slot_id], 0, TRB_TYPE(TRB_TYPE_CONFIG_EP) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_reset_endpoint(uint8_t slot_id, uint8_t ep_num) {
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_RESET_EP) | (static_cast<uint32_t>(slot_id) << 24) | (static_cast<uint32_t>(ep_num) << 16)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_set_tr_dequeue(uint8_t slot_id, uint8_t ep_num) {
    Trb cmd = {g_xhci.transfer_ring_phys[slot_id][ep_num] | g_xhci.transfer_cycle[slot_id][ep_num], 0,
               TRB_TYPE(TRB_TYPE_SET_TR_DEQUEUE) | (static_cast<uint32_t>(slot_id) << 24) | (static_cast<uint32_t>(ep_num) << 16)};
    Trb result;
    if (xhci_send_command(&cmd, &result)) {
        g_xhci.transfer_enqueue[slot_id][ep_num] = 0;
        return true;
    }
    return false;
}

static bool xhci_wait_transfer(uint8_t, Trb* result, uint32_t iterations) {
    while (iterations--) {
        Trb* evt = &g_xhci.event_ring[g_xhci.event_dequeue];
        if ((evt->control & TRB_CYCLE) == g_xhci.event_cycle) {
            const uint8_t type = TRB_GET_TYPE(evt->control);
            if (++g_xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
                g_xhci.event_dequeue = 0;
                g_xhci.event_cycle ^= 1;
            }
            
            auto* ir = reinterpret_cast<volatile XhciInterrupterRegs*>(reinterpret_cast<uintptr_t>(g_xhci.runtime) + 0x20);
            mmio_write64(const_cast<uint64_t*>(&ir->erdp), (g_xhci.event_ring_phys + g_xhci.event_dequeue * sizeof(Trb)) | ERDP_EHB);
            
            if (type == TRB_TYPE_TRANSFER_EVENT) {
                if (result) *result = *evt;
                const uint8_t cc = (evt->status >> 24) & 0xFF;
                return cc == TRB_COMP_SUCCESS || cc == TRB_COMP_SHORT_PACKET;
            }
        }
        io_wait();
    }
    return false;
}

bool xhci_control_transfer(uint8_t slot_id, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, void* data, uint16_t* transferred) {
    if (!g_xhci.transfer_rings[slot_id][0]) return false;
    spinlock_acquire(&g_xhci_lock);
    
    DMAAllocation dma = {0, 0, 0};
    if (data && length > 0) {
        dma = vmm_alloc_dma(1);
        if (!dma.phys) { spinlock_release(&g_xhci_lock); return false; }
        if (!(request_type & 0x80)) kstring::memcpy(reinterpret_cast<void*>(dma.virt), data, length);
    }
    
    Trb setup = { (static_cast<uint64_t>(request_type) | (static_cast<uint64_t>(request) << 8) | (static_cast<uint64_t>(value) << 16) | (static_cast<uint64_t>(index) << 32) | (static_cast<uint64_t>(length) << 48)), 8, TRB_TYPE(TRB_TYPE_SETUP) | TRB_IDT };
    if (length > 0) setup.control |= (request_type & 0x80) ? TRB_TRT_IN : TRB_TRT_OUT;
    xhci_enqueue_transfer(slot_id, 0, &setup);
    
    if (length > 0) {
        Trb data_trb = {dma.phys, length, TRB_TYPE(TRB_TYPE_DATA)};
        if (request_type & 0x80) data_trb.control |= TRB_DIR_IN;
        xhci_enqueue_transfer(slot_id, 0, &data_trb);
    }
    
    Trb status_trb = {0, 0, TRB_TYPE(TRB_TYPE_STATUS) | TRB_IOC};
    if (!(request_type & 0x80) || length == 0) status_trb.control |= TRB_DIR_IN;
    xhci_enqueue_transfer(slot_id, 0, &status_trb);
    
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, DB_EP0_IN);
    
    Trb result;
    const bool success = xhci_wait_transfer(slot_id, &result, 5000);
    if (success && data && length > 0 && (request_type & 0x80)) {
        const uint32_t actual = length - (result.status & 0xFFFFFF);
        kstring::memcpy(data, reinterpret_cast<void*>(dma.virt), actual);
        if (transferred) *transferred = actual;
    }
    
    if (dma.phys) vmm_free_dma(dma);
    spinlock_release(&g_xhci_lock);
    return success;
}

bool xhci_interrupt_transfer(uint8_t slot_id, uint8_t ep_num, void* data, uint16_t length, uint16_t* transferred) {
    if (!g_xhci.transfer_rings[slot_id][ep_num]) return false;
    spinlock_acquire(&g_xhci_lock);

    if (g_intr_buffer_dma[slot_id][ep_num].phys == 0) {
        g_intr_buffer_dma[slot_id][ep_num] = vmm_alloc_dma(1);
        if (!g_intr_buffer_dma[slot_id][ep_num].phys) { spinlock_release(&g_xhci_lock); return false; }
    }
    
    if (g_xhci.intr_complete[slot_id][ep_num]) {
        const Trb result = g_xhci.transfer_result[slot_id][ep_num];
        g_xhci.intr_complete[slot_id][ep_num] = false;
        const uint8_t cc = (result.status >> 24) & 0xFF;
        
        Trb trb = {g_intr_buffer_dma[slot_id][ep_num].phys, length, TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP};
        xhci_enqueue_transfer(slot_id, ep_num, &trb);
        asm volatile("mfence" ::: "memory");
        xhci_ring_doorbell(slot_id, ep_num);
        g_xhci.intr_pending[slot_id][ep_num] = true;
        g_xhci.intr_start_time[slot_id][ep_num] = timer_get_ticks();
        
        if (cc != TRB_COMP_SUCCESS && cc != TRB_COMP_SHORT_PACKET) {
            if (++g_ep_failures[slot_id][ep_num] >= MAX_EP_FAILURES) {
                xhci_reset_endpoint(slot_id, ep_num);
                xhci_set_tr_dequeue(slot_id, ep_num);
                g_ep_failures[slot_id][ep_num] = 0;
            }
            spinlock_release(&g_xhci_lock);
            return false;
        }
        
        g_ep_failures[slot_id][ep_num] = 0;
        const uint32_t actual = length - (result.status & 0xFFFFFF);
        kstring::memcpy(data, reinterpret_cast<void*>(g_intr_buffer_dma[slot_id][ep_num].virt), actual);
        if (transferred) *transferred = actual;
        spinlock_release(&g_xhci_lock);
        return true;
    }
    
    if (g_xhci.intr_pending[slot_id][ep_num]) {
        if (timer_get_ticks() - g_xhci.intr_start_time[slot_id][ep_num] > 100) g_xhci.intr_pending[slot_id][ep_num] = false;
        else { spinlock_release(&g_xhci_lock); return false; }
    }
    
    Trb trb = {g_intr_buffer_dma[slot_id][ep_num].phys, length, TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP};
    xhci_enqueue_transfer(slot_id, ep_num, &trb);
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, ep_num);
    g_xhci.intr_pending[slot_id][ep_num] = true;
    g_xhci.intr_start_time[slot_id][ep_num] = timer_get_ticks();
    
    spinlock_release(&g_xhci_lock);
    return false;
}

void xhci_poll_events() {
    if (!g_xhci_initialized || !g_xhci.event_ring) return;
    spinlock_acquire(&g_xhci_lock);

    auto* ir = reinterpret_cast<volatile XhciInterrupterRegs*>(reinterpret_cast<uintptr_t>(g_xhci.runtime) + 0x20);
    const uint32_t iman = mmio_read32(const_cast<uint32_t*>(&ir->iman));
    if (iman & IMAN_IP) mmio_write32(const_cast<uint32_t*>(&ir->iman), iman | IMAN_IP);

    const uint32_t usbsts = mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbsts));
    if (usbsts & USBSTS_EINT) mmio_write32(const_cast<uint32_t*>(&g_xhci.op->usbsts), usbsts | USBSTS_EINT);

    int count = 0;
    while (count++ < 64) {
        Trb* evt = &g_xhci.event_ring[g_xhci.event_dequeue];
        if ((evt->control & TRB_CYCLE) != g_xhci.event_cycle) break;
        
        const uint8_t type = TRB_GET_TYPE(evt->control);
        if (type == TRB_TYPE_TRANSFER_EVENT) {
            const uint8_t slot = (evt->control >> 24) & 0xFF;
            const uint8_t ep = (evt->control >> 16) & 0x1F;
            if (g_xhci.intr_pending[slot][ep]) {
                g_xhci.transfer_result[slot][ep] = *evt;
                g_xhci.intr_complete[slot][ep] = true;
                g_xhci.intr_pending[slot][ep] = false;
            }
        } else if (type == TRB_TYPE_PORT_STATUS_CHANGE) {
            const uint8_t port = (evt->parameter >> 24) & 0xFF;
            if (port > 0 && port <= g_xhci.max_ports) {
                const uint32_t val = mmio_read32(const_cast<uint32_t*>(&g_xhci.ports[port - 1].portsc));
                mmio_write32(const_cast<uint32_t*>(&g_xhci.ports[port - 1].portsc), (val & PORTSC_CHANGE_MASK) | PORTSC_PP);
            }
        }
        
        if (++g_xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
            g_xhci.event_dequeue = 0;
            g_xhci.event_cycle ^= 1;
        }
        mmio_write64(const_cast<uint64_t*>(&ir->erdp), (g_xhci.event_ring_phys + g_xhci.event_dequeue * sizeof(Trb)) | ERDP_EHB);
    }
    spinlock_release(&g_xhci_lock);
}

bool xhci_wait_for_event(uint32_t timeout_ms) {
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        if ((g_xhci.event_ring[g_xhci.event_dequeue].control & TRB_CYCLE) == g_xhci.event_cycle) return true;
        io_wait();
    }
    return false;
}

void xhci_dump_status() {
    if (!g_xhci_initialized) {
        KLOG(LogModule::Usb, LogLevel::Warn, "xHCI not initialized");
        return;
    }
    
    KLOG(LogModule::Usb, LogLevel::Info, "xHCI Status: USBSTS=0x%x", mmio_read32(const_cast<uint32_t*>(&g_xhci.op->usbsts)));
    for (uint8_t i = 0; i < g_xhci.max_ports && i < 8; i++) {
        const uint32_t portsc = mmio_read32(const_cast<uint32_t*>(&g_xhci.ports[i].portsc));
        KLOG(LogModule::Usb, LogLevel::Trace, "Port %d: PORTSC=0x%x CCS=%d PED=%d Speed=%d",
             i + 1, portsc, (portsc & PORTSC_CCS) ? 1 : 0, (portsc & PORTSC_PED) ? 1 : 0, (portsc >> PORTSC_SPEED_SHIFT) & 0xF);
    }
}
