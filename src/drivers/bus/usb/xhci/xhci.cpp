#include <drivers/apic/ioapic.h>
#include <drivers/bus/pci/msi.h>
#include <drivers/bus/pci/pci.h>
#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/debug.h>
#include <kernel/irq.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>
#include <stddef.h>

static XhciController g_xhci;
static bool g_xhci_initialized = false;
static Spinlock g_xhci_lock = SPINLOCK_INIT;

static MsixState g_xhci_msix;
static bool g_xhci_using_message_interrupts = false;

static DMAAllocation g_intr_buffer_dma[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS] = {{{0, 0, 0}}};
static uint16_t g_intr_length[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS] = {{0}};
static uint8_t g_intr_last_cc[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS] = {{0}};
static uint8_t g_xhci_irq = 0;
static bool g_intr_recovery_needed[XHCI_MAX_SLOTS][XHCI_MAX_ENDPOINTS] = {{false}};
static bool g_in_poll = false;

static void xhci_wait_ms(uint32_t ms)
{
    timer_poll_wait_ms(ms);
}

// xHCI Context Helper Functions
inline size_t xhci_get_context_stride()
{
    return g_xhci.context_size_64 ? 64 : 32;
}

inline SlotContext *xhci_get_input_slot_ctx(InputContext *ctx)
{
    return reinterpret_cast<SlotContext *>(reinterpret_cast<uint8_t *>(ctx) + xhci_get_context_stride() * 1);
}

inline EndpointContext *xhci_get_input_ep_ctx(InputContext *ctx, uint8_t dci)
{
    return reinterpret_cast<EndpointContext *>(reinterpret_cast<uint8_t *>(ctx) +
                                               xhci_get_context_stride() * (1 + dci));
}

inline SlotContext *xhci_get_device_slot_ctx(DeviceContext *ctx)
{
    return reinterpret_cast<SlotContext *>(ctx);
}

inline EndpointContext *xhci_get_device_ep_ctx(DeviceContext *ctx, uint8_t dci)
{
    return reinterpret_cast<EndpointContext *>(reinterpret_cast<uint8_t *>(ctx) + xhci_get_context_stride() * dci);
}

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target);
static bool xhci_send_command(Trb *trb, Trb *result);
static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_idx, Trb *trb);
static void xhci_recover_interrupt_endpoint(uint8_t slot_id, uint8_t ep_num);
static void xhci_reset_transfer_ring(uint8_t slot_id, uint8_t ep_num);
static bool xhci_issue_set_tr_dequeue(uint8_t slot_id, uint8_t ep_num);

uint8_t xhci_get_irq()
{
    return g_xhci_irq;
}

[[nodiscard]] const char *xhci_completion_code_str(uint8_t code)
{
    switch (code) {
        case TRB_COMP_SUCCESS:
            return "Success";
        case TRB_COMP_DATA_BUFFER:
            return "Data Buffer Error";
        case TRB_COMP_BABBLE:
            return "Babble";
        case TRB_COMP_USB_TRANSACTION:
            return "USB Transaction Error";
        case TRB_COMP_TRB:
            return "TRB Error";
        case TRB_COMP_STALL:
            return "Stall";
        case TRB_COMP_SHORT_PACKET:
            return "Short Packet";
        case TRB_COMP_SLOT_NOT_ENABLED:
            return "Slot Not Enabled";
        case TRB_COMP_EP_NOT_ENABLED:
            return "Endpoint Not Enabled";
        default:
            return "Unknown";
    }
}

[[nodiscard]] static const char *xhci_command_type_str(uint8_t type)
{
    switch (type) {
        case TRB_TYPE_ENABLE_SLOT:
            return "Enable Slot";
        case TRB_TYPE_DISABLE_SLOT:
            return "Disable Slot";
        case TRB_TYPE_ADDRESS_DEVICE:
            return "Address Device";
        case TRB_TYPE_CONFIG_EP:
            return "Configure Endpoint";
        case TRB_TYPE_EVAL_CONTEXT:
            return "Evaluate Context";
        case TRB_TYPE_RESET_EP:
            return "Reset Endpoint";
        case TRB_TYPE_STOP_EP:
            return "Stop Endpoint";
        case TRB_TYPE_SET_TR_DEQUEUE:
            return "Set TR Dequeue";
        case TRB_TYPE_RESET_DEVICE:
            return "Reset Device";
        default:
            return "Command";
    }
}

static bool xhci_is_benign_command_failure(uint8_t type, uint8_t cc)
{
    if (type == TRB_TYPE_DISABLE_SLOT && cc == TRB_COMP_SLOT_NOT_ENABLED)
        return true;
    if (type == TRB_TYPE_STOP_EP && cc == TRB_COMP_EP_NOT_ENABLED)
        return true;
    return false;
}

static void xhci_firmware_handoff()
{
    const uint32_t xecp_off = HCCPARAMS1_XECP(g_xhci.cap->hccparams1) << 2;
    if (!xecp_off)
        return;

    auto *xecp = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(g_xhci.cap) + xecp_off);

    for (int i = 0; i < 256; i++) {
        const uint32_t header = mmio_read32(const_cast<uint32_t *>(xecp));
        if ((header & 0xFF) == XECP_ID_USB_OWNERSHIP) {
            if (header & USBLEGSUP_FIRMWARE_SEM) {
                KLOG(LogModule::Usb, LogLevel::Info, "Firmware owns xHCI, requesting handoff...");
                mmio_write32(const_cast<uint32_t *>(xecp), header | USBLEGSUP_OS_SEM);

                KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: waiting for firmware handoff...");
                uint32_t waited_ms = 0;
                while (mmio_read32(const_cast<uint32_t *>(xecp)) & USBLEGSUP_FIRMWARE_SEM) {
                    if (waited_ms >= 1000) {
                        KLOG(LogModule::Usb, LogLevel::Warn, "xHCI: firmware handoff timeout, forcing ownership");
                        break;
                    }
                    xhci_wait_ms(1);
                    waited_ms++;
                }

                volatile uint32_t *legctlsts = xecp + 1;
                mmio_write32(const_cast<uint32_t *>(legctlsts),
                             (mmio_read32(const_cast<uint32_t *>(legctlsts)) & ~0xFFFF) | 0xE0000000);
                KLOG(LogModule::Usb, LogLevel::Success, "xHCI: firmware handoff complete");
            }
            return;
        }

        const uint8_t next = (header >> 8) & 0xFF;
        if (!next)
            break;
        xecp = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(xecp) + (next << 2));
    }
}

static void xhci_parse_protocols()
{
    const uint32_t xecp_off = HCCPARAMS1_XECP(g_xhci.cap->hccparams1) << 2;
    if (!xecp_off)
        return;

    auto *xecp = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(g_xhci.cap) + xecp_off);

    for (int i = 0; i < 256; i++) {
        const uint32_t header = mmio_read32(const_cast<uint32_t *>(xecp));
        const uint8_t id = header & 0xFF;
        const uint8_t next = (header >> 8) & 0xFF;

        KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Extended Cap ID %d at 0x%lx: 0x%x", id,
             reinterpret_cast<uintptr_t>(xecp), header);

        if (id == XECP_ID_PROTOCOLS) {
            const uint32_t dw0 = mmio_read32(const_cast<uint32_t *>(xecp + 0));
            const uint32_t dw1 = mmio_read32(const_cast<uint32_t *>(xecp + 1));
            const uint32_t dw2 = mmio_read32(const_cast<uint32_t *>(xecp + 2));

            const uint8_t major = (dw0 >> 24) & 0xFF;
            const uint8_t port_off = dw2 & 0xFF;
            const uint8_t port_cnt = (dw2 >> 8) & 0xFF;

            KLOG(LogModule::Usb, LogLevel::Info, "xHCI: Protocol USB %d.0 Cap: 0x%x 0x%x 0x%x (Ports %d-%d)", major,
                 dw0, dw1, dw2, port_off, port_off + port_cnt - 1);

            if (port_off > 0) {
                for (uint8_t p = port_off; p < port_off + port_cnt; p++) {
                    g_xhci.port_protocols[p] = major;
                }
            }
        }

        if (!next)
            break;
        xecp = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(xecp) + (next << 2));
    }
}

static void xhci_irq_handler(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;
    if (xhci_is_initialized())
        xhci_poll_events();
}

bool xhci_init()
{
    if (g_xhci_initialized)
        return true;
    KLOG(LogModule::Usb, LogLevel::Info, "Initializing xHCI controller...");

    PciDevice pci_dev;
    if (!pci_find_xhci(&pci_dev)) {
        KLOG(LogModule::Usb, LogLevel::Warn, "No xHCI controller detected; skipping native USB host initialization");
        return false;
    }
    KLOG(LogModule::Usb, LogLevel::Info, "Found xHCI at %d:%d.%d", pci_dev.bus, pci_dev.device, pci_dev.function);

    pci_enable_memory_space(&pci_dev);
    pci_enable_bus_mastering(&pci_dev);

    // Try MSI-X first (strongly preferred on UEFI/real hardware)
    if (pci_has_msix(&pci_dev)) {
        if (pci_enable_msix(&pci_dev, &g_xhci_msix)) {
            uint8_t xhci_vector = idt_allocate_free_vector();
            if (xhci_vector && irq_register_vector_handler(xhci_vector, xhci_irq_handler, nullptr)) {
                extern uint32_t apic_get_current_id();
                msix_set_entry(&g_xhci_msix, 0, xhci_vector, static_cast<uint8_t>(apic_get_current_id()));
                msix_unmask_vector(&g_xhci_msix, 0);

                g_xhci_using_message_interrupts = true;
                g_xhci_irq = 0; // IRQ not used for MSI-X
                KLOG(LogModule::Usb, LogLevel::Info, "xHCI using MSI-X (Vector %d)", xhci_vector);
            }
        }
    }

    if (!g_xhci_using_message_interrupts && pci_has_msi(&pci_dev)) {
        uint8_t xhci_vector = 0;
        if (pci_enable_msi(&pci_dev, &xhci_vector) && xhci_vector != 0 &&
            irq_register_vector_handler(xhci_vector, xhci_irq_handler, nullptr)) {
            g_xhci_using_message_interrupts = true;
            g_xhci_irq = 0;
            KLOG(LogModule::Usb, LogLevel::Info, "xHCI using standard MSI (Vector %d)", xhci_vector);
        }
    }

    if (!g_xhci_using_message_interrupts) {
        pci_enable_interrupts(&pci_dev);
        uint8_t irq_line = pci_dev.irq_line;
        // Validate PCI interrupt line. 0 and 255 indicate no interrupt routing on UEFI.
        if (irq_line != 0 && irq_line != 255) {
            g_xhci_irq = irq_line;
            if (irq_register_isa_handler(g_xhci_irq, xhci_irq_handler, nullptr)) {
                KLOG(LogModule::Usb, LogLevel::Info, "xHCI: using routed IRQ %d", g_xhci_irq);
            } else {
                KLOG(LogModule::Usb, LogLevel::Warn, "xHCI: failed to register routed IRQ %d", g_xhci_irq);
                g_xhci_irq = 0;
            }
        } else {
            KLOG(LogModule::Usb, LogLevel::Warn, "xHCI: invalid IRQ line %d (0 or 255), using polling fallback",
                 irq_line);
            g_xhci_irq = 0;
        }
    }

    uint64_t bar_size = 0;
    const uint64_t bar_phys = pci_get_bar(&pci_dev, 0, &bar_size) & ~0xF;
    if (bar_phys == 0)
        return false;
    if (bar_size < 0x10000)
        bar_size = 0x10000;
    const uint64_t bar_virt_addr = vmm_map_mmio(bar_phys, bar_size);
    if (!bar_virt_addr)
        return false;
    void *bar_virt = reinterpret_cast<void *>(bar_virt_addr);

    g_xhci.cap = reinterpret_cast<volatile XhciCapRegs *>(bar_virt);
    xhci_firmware_handoff();

    const uint8_t cap_len = g_xhci.cap->caplength;
    g_xhci.op = reinterpret_cast<volatile XhciOpRegs *>(reinterpret_cast<uintptr_t>(bar_virt) + cap_len);
    g_xhci.doorbell =
        reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(bar_virt) + (g_xhci.cap->dboff & ~0x3));
    g_xhci.runtime = reinterpret_cast<volatile XhciRuntimeRegs *>(reinterpret_cast<uintptr_t>(bar_virt) +
                                                                  (g_xhci.cap->rtsoff & ~0x1F));
    g_xhci.ports = reinterpret_cast<volatile XhciPortRegs *>(reinterpret_cast<uintptr_t>(g_xhci.op) + 0x400);

    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: BAR Phys=0x%lx, Virt=0x%lx, CapLen=%d", bar_phys,
         reinterpret_cast<uintptr_t>(bar_virt), cap_len);
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: OpRegs at 0x%lx, Doorbell at 0x%lx, Runtime at 0x%lx, Ports at 0x%lx",
         reinterpret_cast<uintptr_t>(g_xhci.op), reinterpret_cast<uintptr_t>(g_xhci.doorbell),
         reinterpret_cast<uintptr_t>(g_xhci.runtime), reinterpret_cast<uintptr_t>(g_xhci.ports));

    const uint32_t hcsparams1 = g_xhci.cap->hcsparams1;
    const uint32_t hcsparams2 = g_xhci.cap->hcsparams2;
    const uint32_t hccparams1 = g_xhci.cap->hccparams1;

    g_xhci.max_slots = HCSPARAMS1_MAX_SLOTS(hcsparams1);
    g_xhci.max_ports = HCSPARAMS1_MAX_PORTS(hcsparams1);
    g_xhci.max_intrs = HCSPARAMS1_MAX_INTRS(hcsparams1);
    g_xhci.context_size_64 = HCCPARAMS1_CSZ(hccparams1);
    g_xhci.page_size = mmio_read32(const_cast<uint32_t *>(&g_xhci.op->pagesize)) << 12;
    g_xhci.num_scratchpad = HCSPARAMS2_MAX_SCRATCHPAD(hcsparams2);

    KLOG(LogModule::Usb, LogLevel::Info,
         "xHCI: Controller Caps: Slots=%d, Ports=%d, Intrs=%d, CSZ=%d, PageSize=%d, Scratchpad=%d", g_xhci.max_slots,
         g_xhci.max_ports, g_xhci.max_intrs, g_xhci.context_size_64 ? 64 : 32, g_xhci.page_size, g_xhci.num_scratchpad);
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: HCSPARAMS1=0x%x, HCSPARAMS2=0x%x, HCCPARAMS1=0x%x", hcsparams1,
         hcsparams2, hccparams1);

    xhci_parse_protocols();

    if (!xhci_reset()) {
        KLOG(LogModule::Usb, LogLevel::Error, "Controller reset failed");
        return false;
    }

    uint32_t timeout = 500000;
    while ((mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbsts)) & USBSTS_CNR) && timeout--) {
        io_wait();
    }
    if (!timeout) {
        KLOG(LogModule::Usb, LogLevel::Error, "Controller not ready");
        return false;
    }

    mmio_write32(const_cast<uint32_t *>(&g_xhci.op->config), g_xhci.max_slots);

    const size_t dcbaa_size = (g_xhci.max_slots + 1) * sizeof(uint64_t);
    const DMAAllocation dcbaa_dma = vmm_alloc_dma((dcbaa_size + 4095) / 4096);
    if (!dcbaa_dma.phys)
        return false;

    g_xhci.dcbaa = reinterpret_cast<uint64_t *>(dcbaa_dma.virt);
    g_xhci.dcbaa_phys = dcbaa_dma.phys;
    kstring::zero_memory(g_xhci.dcbaa, dcbaa_size);

    if (g_xhci.num_scratchpad > 0) {
        const DMAAllocation arr_dma = vmm_alloc_dma(1);
        if (!arr_dma.phys)
            return false;

        g_xhci.scratchpad_array = reinterpret_cast<uint64_t *>(arr_dma.virt);
        g_xhci.scratchpad_array_phys = arr_dma.phys;

        for (uint32_t i = 0; i < g_xhci.num_scratchpad; i++) {
            const DMAAllocation pg = vmm_alloc_dma(1);
            if (!pg.phys)
                return false;
            g_xhci.scratchpad_array[i] = pg.phys;
        }
        g_xhci.dcbaa[0] = g_xhci.scratchpad_array_phys;
    }

    mmio_write64(const_cast<uint64_t *>(&g_xhci.op->dcbaap), g_xhci.dcbaa_phys);
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: DCBAA at 0x%lx (Virt: 0x%lx)", g_xhci.dcbaa_phys,
         reinterpret_cast<uintptr_t>(g_xhci.dcbaa));

    const DMAAllocation cmd_dma = vmm_alloc_dma(1);
    if (!cmd_dma.phys)
        return false;

    g_xhci.cmd_ring = reinterpret_cast<Trb *>(cmd_dma.virt);
    g_xhci.cmd_ring_phys = cmd_dma.phys;
    kstring::zero_memory(g_xhci.cmd_ring, XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.cmd_enqueue = 0;
    g_xhci.cmd_cycle = 1;

    Trb *link = &g_xhci.cmd_ring[XHCI_RING_SIZE - 1];
    link->parameter = g_xhci.cmd_ring_phys;
    link->status = 0;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;

    mmio_write64(const_cast<uint64_t *>(&g_xhci.op->crcr), g_xhci.cmd_ring_phys | CRCR_RCS);
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Command Ring at 0x%lx", g_xhci.cmd_ring_phys);

    const DMAAllocation evt_dma = vmm_alloc_dma(1);
    if (!evt_dma.phys)
        return false;

    g_xhci.event_ring = reinterpret_cast<Trb *>(evt_dma.virt);
    g_xhci.event_ring_phys = evt_dma.phys;
    kstring::zero_memory(g_xhci.event_ring, XHCI_EVENT_RING_SIZE * sizeof(Trb));
    g_xhci.event_dequeue = 0;
    g_xhci.event_cycle = 1;

    const DMAAllocation erst_dma = vmm_alloc_dma(1);
    if (!erst_dma.phys)
        return false;

    g_xhci.erst = reinterpret_cast<ErstEntry *>(erst_dma.virt);
    g_xhci.erst_phys = erst_dma.phys;
    g_xhci.erst[0].ring_segment_base = g_xhci.event_ring_phys;
    g_xhci.erst[0].ring_segment_size = XHCI_EVENT_RING_SIZE;
    g_xhci.erst[0].reserved = 0;

    auto *ir = reinterpret_cast<volatile XhciInterrupterRegs *>(reinterpret_cast<uintptr_t>(g_xhci.runtime) + 0x20);

    mmio_write32(const_cast<uint32_t *>(&ir->iman), 0);
    mmio_write32(const_cast<uint32_t *>(&ir->erstsz), 1);
    mmio_write64(const_cast<uint64_t *>(&ir->erdp), g_xhci.event_ring_phys);
    mmio_write64(const_cast<uint64_t *>(&ir->erstba), g_xhci.erst_phys);
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Event Ring at 0x%lx, ERST at 0x%lx", g_xhci.event_ring_phys,
         g_xhci.erst_phys);
    mmio_write32(const_cast<uint32_t *>(&ir->imod), 160);
    mmio_write32(const_cast<uint32_t *>(&ir->iman), IMAN_IE);

    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        g_xhci.device_contexts[i] = nullptr;
        g_xhci.input_contexts[i] = nullptr;
        g_xhci.port_needs_enumeration[i] = false;
        for (int j = 0; j < XHCI_MAX_ENDPOINTS; j++) {
            g_xhci.transfer_rings[i][j] = nullptr;
            g_xhci.intr_pending[i][j] = false;
            g_xhci.intr_complete[i][j] = false;
            g_xhci.intr_start_time[i][j] = 0;
            g_xhci.intr_callbacks[i][j] = nullptr;
            g_xhci.sync_pending[i][j] = false;
            g_xhci.sync_complete[i][j] = false;
            g_intr_recovery_needed[i][j] = false;
        }
    }

    g_xhci.command_event_ready = false;
    for (int i = 0; i < 256; i++) {
        g_xhci.control_event_ready[i] = false;
    }

    for (uint8_t i = 1; i <= g_xhci.max_ports; i++) {
        uint32_t portsc = mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[i - 1].portsc));
        const uint8_t proto = g_xhci.port_protocols[i];

        if (proto != 0) {
            KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Initializing Port %d (USB %d.x, PORTSC: 0x%x)", i, proto,
                 portsc);
        } else {
            KLOG(LogModule::Usb, LogLevel::Info, "xHCI: Initializing Port %d (Unknown Protocol, PORTSC: 0x%x)", i,
                 portsc);
        }

        if (!(portsc & PORTSC_PP)) {
            mmio_write32(const_cast<uint32_t *>(&g_xhci.ports[i - 1].portsc),
                         (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_PP);

            // Verification: Wait a bit and check if power actually turned on
            sleep(20);
            portsc = mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[i - 1].portsc));
            if (!(portsc & PORTSC_PP)) {
                KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Port %d power-on did not assert (PORTSC: 0x%x)", i,
                     portsc);
            }
        }
    }

    sleep(100);

    if (!xhci_start()) {
        KLOG(LogModule::Usb, LogLevel::Error, "Controller start failed");
        return false;
    }

    g_xhci_initialized = true;
    KLOG(LogModule::Usb, LogLevel::Success, "xHCI initialized successfully");
    return true;
}

bool xhci_reset()
{
    uint32_t cmd = mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbcmd));
    cmd &= ~USBCMD_RS;
    mmio_write32(const_cast<uint32_t *>(&g_xhci.op->usbcmd), cmd);

    uint32_t waited_ms = 0;
    while (!(mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbsts)) & USBSTS_HCH)) {
        if (waited_ms >= 100)
            return false;
        xhci_wait_ms(1);
        waited_ms++;
    }

    cmd = mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbcmd));
    mmio_write32(const_cast<uint32_t *>(&g_xhci.op->usbcmd), cmd | USBCMD_HCRST);

    waited_ms = 0;
    while (mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbcmd)) & USBCMD_HCRST) {
        if (waited_ms >= 100)
            return false;
        xhci_wait_ms(1);
        waited_ms++;
    }
    return true;
}

bool xhci_start()
{
    const uint32_t cmd = mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbcmd));
    mmio_write32(const_cast<uint32_t *>(&g_xhci.op->usbcmd), cmd | USBCMD_RS | USBCMD_INTE);

    uint32_t waited_ms = 0;
    while (mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbsts)) & USBSTS_HCH) {
        if (waited_ms >= 100)
            return false;
        xhci_wait_ms(1);
        waited_ms++;
    }
    return true;
}

void xhci_stop()
{
    const uint32_t cmd = mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbcmd));
    mmio_write32(const_cast<uint32_t *>(&g_xhci.op->usbcmd), cmd & ~USBCMD_RS);
}

bool xhci_is_initialized()
{
    return g_xhci_initialized;
}
uint8_t xhci_get_max_ports()
{
    return g_xhci_initialized ? g_xhci.max_ports : 0;
}

uint8_t xhci_get_port_protocol(uint8_t port)
{
    if (port == 0)
        return 0;

    // 1. Check if we have a static protocol capability for this port
    if (g_xhci.port_protocols[port] != 0)
        return g_xhci.port_protocols[port];

    // 2. Dynamic Fallback: Check PORTSC speed if port is connected/enabled
    const uint32_t portsc = mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[port - 1].portsc));
    const uint8_t speed = static_cast<uint8_t>((portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT);

    // Speed 4 is SuperSpeed (USB 3.0), Speed 3 is High-Speed (USB 2.0)
    if (speed >= 4)
        return 3;
    if (speed > 0)
        return 2;

    // 3. Default to USB 2.0 for unknown inactive ports (safest for reset logic)
    return 2;
}

uint8_t xhci_get_port_speed(uint8_t port)
{
    if (port == 0 || port > g_xhci.max_ports)
        return 0;
    const uint32_t portsc = mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[port - 1].portsc));
    return (portsc & PORTSC_SPEED_MASK) >> PORTSC_SPEED_SHIFT;
}

bool xhci_port_connected(uint8_t port)
{
    if (port == 0 || port > g_xhci.max_ports)
        return false;
    return (mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[port - 1].portsc)) & PORTSC_CCS) != 0;
}

bool xhci_get_port_needs_enumeration(uint8_t port)
{
    if (port == 0 || port > g_xhci.max_ports)
        return false;
    return g_xhci.port_needs_enumeration[port - 1];
}

void xhci_clear_port_needs_enumeration(uint8_t port)
{
    if (port == 0 || port > g_xhci.max_ports)
        return;
    g_xhci.port_needs_enumeration[port - 1] = false;
}

bool xhci_reset_port(uint8_t port)
{
    if (port == 0 || port > g_xhci.max_ports)
        return false;

    volatile XhciPortRegs *p = &g_xhci.ports[port - 1];
    uint32_t portsc = mmio_read32(const_cast<uint32_t *>(&p->portsc));
    if (!(portsc & PORTSC_CCS))
        return false;

    mmio_write32(const_cast<uint32_t *>(&p->portsc), (portsc & PORTSC_CHANGE_MASK) | PORTSC_PP);
    const bool is_usb3 = xhci_get_port_protocol(port) == 3;

    // Some controllers (AMD B450) need a bit of time before we start the reset
    sleep(20);

    portsc = mmio_read32(const_cast<uint32_t *>(&p->portsc));
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Port %d pre-reset PORTSC: 0x%x", port, portsc);
    if (is_usb3 && (portsc & PORTSC_CCS) && !(portsc & PORTSC_PED)) {
        KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Port %d using Warm Port Reset", port);
        mmio_write32(const_cast<uint32_t *>(&p->portsc), (portsc & ~PORTSC_CHANGE_MASK) | PORTSC_WPR | PORTSC_PP);
    } else {
        KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Port %d using Standard Port Reset", port);
        mmio_write32(const_cast<uint32_t *>(&p->portsc),
                     (portsc & ~(PORTSC_CHANGE_MASK | PORTSC_PED)) | PORTSC_PR | PORTSC_PP);
    }
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Port %d Reset Started (PORTSC: 0x%x)", port, portsc);

    uint32_t waited_ms = 0;
    while (true) {
        portsc = mmio_read32(const_cast<uint32_t *>(&p->portsc));
        if (portsc & PORTSC_PRC)
            break;
        if (waited_ms >= 500) { // Increased timeout to 500ms
            KLOG(LogModule::Usb, LogLevel::Error, "Port %d reset timeout", port);
            return false;
        }
        xhci_wait_ms(1);
        waited_ms++;
    }

    // Clear reset change bit and ensure power is ON
    mmio_write32(const_cast<uint32_t *>(&p->portsc), (portsc & PORTSC_CHANGE_MASK) | PORTSC_PP);

    // Wait for the port to stabilize after reset
    sleep(50);
    portsc = mmio_read32(const_cast<uint32_t *>(&p->portsc));
    const uint8_t speed = static_cast<uint8_t>((portsc >> PORTSC_SPEED_SHIFT) & 0xF);
    KLOG(LogModule::Usb, LogLevel::Info, "xHCI: Port %d Reset Complete (Speed: %d, PORTSC: 0x%x)", port, speed, portsc);
    return (portsc & PORTSC_PED) != 0;
}

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target)
{
    mmio_write32(const_cast<uint32_t *>(&g_xhci.doorbell[slot_id]), target);
}

static void xhci_enqueue_command(Trb *trb)
{
    Trb *dest = &g_xhci.cmd_ring[g_xhci.cmd_enqueue];

    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Enqueue Command: [0]=0x%lx [2]=0x%x [3]=0x%x", trb->parameter,
         trb->status, trb->control);

    dest->parameter = trb->parameter;
    dest->status = trb->status;
    dest->control = (trb->control & ~TRB_CYCLE) | g_xhci.cmd_cycle;

    if (++g_xhci.cmd_enqueue >= XHCI_RING_SIZE - 1) {
        Trb *link = &g_xhci.cmd_ring[XHCI_RING_SIZE - 1];
        link->control = (link->control & ~TRB_CYCLE) | g_xhci.cmd_cycle;
        g_xhci.cmd_cycle ^= 1;
        g_xhci.cmd_enqueue = 0;
    }
}

static bool xhci_wait_command(Trb *result, uint32_t timeout_ms)
{
    for (uint32_t waited_ms = 0; waited_ms < timeout_ms; ++waited_ms) {
        if (g_xhci.command_event_ready) {
            if (result)
                *result = g_xhci.last_command_event;
            g_xhci.command_event_ready = false;
            return ((g_xhci.last_command_event.status >> 24) & 0xFF) == TRB_COMP_SUCCESS;
        }
        // GUARDED to prevent recursive polling
        if (!g_in_poll && g_xhci.event_ring &&
            (g_xhci.event_ring[g_xhci.event_dequeue].control & TRB_CYCLE) == g_xhci.event_cycle) {
            g_in_poll = true;
            xhci_poll_events();
            g_in_poll = false;
        }
        xhci_wait_ms(1);
    }
    return false;
}

static bool xhci_send_command(Trb *trb, Trb *result)
{
    const uint8_t type = TRB_GET_TYPE(trb->control);
    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Sending %s (%d)...", xhci_command_type_str(type), type);

    spinlock_acquire(&g_xhci_lock);
    g_xhci.command_event_ready = false;
    xhci_enqueue_command(trb);
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(0, 0);
    spinlock_release(&g_xhci_lock);

    bool ok = xhci_wait_command(result, 1000);
    if (!ok && result) {
        const uint8_t cc = static_cast<uint8_t>((result->status >> 24) & 0xFF);
        if (xhci_is_benign_command_failure(type, cc)) {
            KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: %s (%d) ignored benign failure: %s",
                 xhci_command_type_str(type), type, xhci_completion_code_str(cc));
        } else {
            KLOG(LogModule::Usb, LogLevel::Error, "xHCI: %s (%d) failed: %s", xhci_command_type_str(type), type,
                 xhci_completion_code_str(cc));
        }
    }
    return ok;
}

static void xhci_enqueue_transfer(uint8_t slot_id, uint8_t ep_idx, Trb *trb)
{
    if (slot_id == 0 || ep_idx >= XHCI_MAX_ENDPOINTS)
        return;
    if (!g_xhci.transfer_rings[slot_id][ep_idx])
        return;

    Trb *ring = g_xhci.transfer_rings[slot_id][ep_idx];
    uint32_t *idx = &g_xhci.transfer_enqueue[slot_id][ep_idx];

    KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Enqueue Transfer (Slot %d, EP %d): [0]=0x%lx [2]=0x%x [3]=0x%x",
         slot_id, ep_idx, trb->parameter, trb->status, trb->control);

    Trb *dest = &ring[*idx];
    dest->parameter = trb->parameter;
    dest->status = trb->status;
    asm volatile("sfence" ::: "memory");
    dest->control = (trb->control & ~TRB_CYCLE) | g_xhci.transfer_cycle[slot_id][ep_idx];

    (*idx)++;

    if (*idx >= XHCI_RING_SIZE - 1) {
        Trb *link = &ring[XHCI_RING_SIZE - 1];
        uint8_t current_cycle = g_xhci.transfer_cycle[slot_id][ep_idx];
        link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | (current_cycle ? TRB_CYCLE : 0);

        g_xhci.transfer_cycle[slot_id][ep_idx] ^= 1;
        *idx = 0;
    }
}

bool xhci_update_ep0_packet_size(uint8_t slot_id, uint16_t max_packet)
{
    if (slot_id == 0)
        return false;
    if (!g_xhci.input_contexts[slot_id] || !g_xhci.device_contexts[slot_id])
        return false;

    auto *input = g_xhci.input_contexts[slot_id];
    auto *dev = g_xhci.device_contexts[slot_id];

    // Control context is ALWAYS at index 0 of Input Context (32 bytes)
    auto *ctrl_ctx = reinterpret_cast<InputControlContext *>(input);
    kstring::zero_memory(ctrl_ctx, sizeof(InputControlContext));
    ctrl_ctx->add_flags = (1 << 1);

    auto *input_ep0 = xhci_get_input_ep_ctx(input, 1);
    auto *dev_ep0 = xhci_get_device_ep_ctx(dev, 1);

    *input_ep0 = *dev_ep0;
    input_ep0->ep_info = (input_ep0->ep_info & 0x0000FFFF) | (static_cast<uint32_t>(max_packet) << 16);

    Trb cmd = {g_xhci.input_context_phys[slot_id], 0,
               TRB_TYPE(TRB_TYPE_EVAL_CONTEXT) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

int xhci_enable_slot()
{
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_ENABLE_SLOT)};
    Trb result;
    if (!xhci_send_command(&cmd, &result)) {
        KLOG(LogModule::Usb, LogLevel::Error, "Enable Slot command failed");
        return -1;
    }
    return static_cast<int>((result.control >> 24) & 0xFF);
}

bool xhci_disable_slot(uint8_t slot_id)
{
    Trb cmd = {0, 0, TRB_TYPE(TRB_TYPE_DISABLE_SLOT) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;
    bool success = xhci_send_command(&cmd, &result);
    xhci_free_device_resources(slot_id);
    return success;
}

void xhci_free_device_resources(uint8_t slot_id)
{
    if (slot_id == 0)
        return;
    for (int i = 0; i < 32; i++) {
        if (g_xhci.transfer_rings[slot_id][i]) {
            vmm_free_dma({g_xhci.transfer_ring_phys[slot_id][i],
                          reinterpret_cast<uint64_t>(g_xhci.transfer_rings[slot_id][i]), 1});
            g_xhci.transfer_rings[slot_id][i] = nullptr;
        }
        if (g_intr_buffer_dma[slot_id][i].phys) {
            vmm_free_dma(g_intr_buffer_dma[slot_id][i]);
            g_intr_buffer_dma[slot_id][i] = {0, 0, 0};
        }
        g_xhci.intr_callbacks[slot_id][i] = nullptr;
        g_xhci.intr_pending[slot_id][i] = false;
        g_xhci.intr_complete[slot_id][i] = false;
        g_xhci.intr_start_time[slot_id][i] = 0;
        g_intr_length[slot_id][i] = 0;
        g_intr_last_cc[slot_id][i] = 0;
        g_intr_recovery_needed[slot_id][i] = false;
    }
    if (g_xhci.input_contexts[slot_id]) {
        vmm_free_dma({g_xhci.input_context_phys[slot_id], reinterpret_cast<uint64_t>(g_xhci.input_contexts[slot_id]),
                      (g_xhci.context_size_64 ? 64ULL : 32ULL) * 33 / 4096 + 1});
        g_xhci.input_contexts[slot_id] = nullptr;
    }
    if (g_xhci.device_contexts[slot_id]) {
        vmm_free_dma({g_xhci.device_context_phys[slot_id], reinterpret_cast<uint64_t>(g_xhci.device_contexts[slot_id]),
                      (g_xhci.context_size_64 ? 64ULL : 32ULL) * 32 / 4096 + 1});
        g_xhci.device_contexts[slot_id] = nullptr;
        g_xhci.dcbaa[slot_id] = 0;
    }
}

bool xhci_address_device(uint8_t slot_id, uint8_t port, uint8_t speed, uint8_t parent_hub_slot, uint8_t parent_port)
{
    const size_t ctx_stride = xhci_get_context_stride();
    const size_t dev_ctx_bytes = ctx_stride * 32;

    const DMAAllocation dev_dma = vmm_alloc_dma((dev_ctx_bytes + 4095) / 4096);
    if (!dev_dma.phys)
        return false;

    g_xhci.device_contexts[slot_id] = reinterpret_cast<DeviceContext *>(dev_dma.virt);
    g_xhci.device_context_phys[slot_id] = dev_dma.phys;
    kstring::zero_memory(g_xhci.device_contexts[slot_id], dev_ctx_bytes);
    g_xhci.dcbaa[slot_id] = dev_dma.phys;

    const size_t input_ctx_bytes = ctx_stride * 33;
    const DMAAllocation input_dma = vmm_alloc_dma((input_ctx_bytes + 4095) / 4096);
    if (!input_dma.phys)
        return false;

    g_xhci.input_contexts[slot_id] = reinterpret_cast<InputContext *>(input_dma.virt);
    g_xhci.input_context_phys[slot_id] = input_dma.phys;
    kstring::zero_memory(g_xhci.input_contexts[slot_id], input_ctx_bytes);

    auto *input = g_xhci.input_contexts[slot_id];
    auto *ctrl_ctx = reinterpret_cast<InputControlContext *>(input);
    ctrl_ctx->add_flags = (1 << 0) | (1 << 1);

    auto *slot_ctx = xhci_get_input_slot_ctx(input);
    slot_ctx->route_speed_entries = (static_cast<uint32_t>(speed) << 20) | (1 << 27);
    slot_ctx->latency_hub_port = (static_cast<uint32_t>(port) << 16);

    if (parent_hub_slot != 0) {
        // Only Low/Full speed devices behind a High Speed hub need TT info
        if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
            slot_ctx->tt_info =
                (static_cast<uint32_t>(parent_hub_slot) & 0xFF) | ((static_cast<uint32_t>(parent_port) & 0xFF) << 8);
        } else {
            slot_ctx->tt_info = 0;
        }
    }

    const DMAAllocation tr_dma = vmm_alloc_dma(1);
    if (!tr_dma.phys)
        return false;

    g_xhci.transfer_rings[slot_id][0] = reinterpret_cast<Trb *>(tr_dma.virt);
    g_xhci.transfer_ring_phys[slot_id][0] = tr_dma.phys;
    kstring::zero_memory(g_xhci.transfer_rings[slot_id][0], XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.transfer_enqueue[slot_id][0] = 0;
    g_xhci.transfer_cycle[slot_id][0] = 1;

    Trb *link = &g_xhci.transfer_rings[slot_id][0][XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;

    uint16_t max_pkt;
    switch (speed) {
        case XHCI_SPEED_LOW:
        case XHCI_SPEED_FULL:
            max_pkt = 8;
            break;
        case XHCI_SPEED_HIGH:
            max_pkt = 64;
            break;
        default:
            max_pkt = 512;
            break;
    }

    auto *ep0_ctx = xhci_get_input_ep_ctx(input, 1);
    ep0_ctx->ep_info = (3 << 1) | (EP_TYPE_CONTROL << 3) | (static_cast<uint32_t>(max_pkt) << 16);
    ep0_ctx->tr_dequeue = tr_dma.phys | 1;
    ep0_ctx->avg_trb_length = 8;

    Trb cmd = {g_xhci.input_context_phys[slot_id], 0,
               TRB_TYPE(TRB_TYPE_ADDRESS_DEVICE) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;

    if (!xhci_send_command(&cmd, &result)) {
        KLOG(LogModule::Usb, LogLevel::Error, "Address Device failed: %s (Slot %d, Port %d)",
             xhci_completion_code_str(static_cast<uint8_t>((result.status >> 24) & 0xFF)), slot_id, port);
        goto fail;
    }
    KLOG(LogModule::Usb, LogLevel::Info, "xHCI: Device Addressed (Slot %d, Port %d, Speed %d, Context at 0x%lx)",
         slot_id, port, speed, g_xhci.device_context_phys[slot_id]);
    return true;

fail:
    if (g_xhci.transfer_rings[slot_id][0]) {
        vmm_free_dma(
            {g_xhci.transfer_ring_phys[slot_id][0], reinterpret_cast<uint64_t>(g_xhci.transfer_rings[slot_id][0]), 1});
        g_xhci.transfer_rings[slot_id][0] = nullptr;
    }
    if (g_xhci.input_contexts[slot_id]) {
        vmm_free_dma({g_xhci.input_context_phys[slot_id], reinterpret_cast<uint64_t>(g_xhci.input_contexts[slot_id]),
                      (g_xhci.context_size_64 ? 64ULL : 32ULL) * 33 / 4096 + 1});
        g_xhci.input_contexts[slot_id] = nullptr;
    }
    if (g_xhci.device_contexts[slot_id]) {
        vmm_free_dma({g_xhci.device_context_phys[slot_id], reinterpret_cast<uint64_t>(g_xhci.device_contexts[slot_id]),
                      (g_xhci.context_size_64 ? 64ULL : 32ULL) * 32 / 4096 + 1});
        g_xhci.device_contexts[slot_id] = nullptr;
        g_xhci.dcbaa[slot_id] = 0;
    }
    return false;
}

bool xhci_configure_endpoint(uint8_t slot_id, uint8_t ep_num, uint8_t ep_type, uint16_t max_packet, uint8_t interval,
                             uint8_t max_burst, uint8_t mult, uint16_t max_esit_payload)
{
    if (!g_xhci.input_contexts[slot_id])
        return false;

    auto *input = g_xhci.input_contexts[slot_id];
    auto *dev = g_xhci.device_contexts[slot_id];
    const uint8_t dci = ep_num;

    auto *ctrl_ctx = reinterpret_cast<InputControlContext *>(input);
    ctrl_ctx->add_flags = (1 << 0) | (1 << dci);

    auto *input_slot = xhci_get_input_slot_ctx(input);
    auto *dev_slot = xhci_get_device_slot_ctx(dev);
    *input_slot = *dev_slot;

    if (dci > ((dev_slot->route_speed_entries >> 27) & 0x1F)) {
        input_slot->route_speed_entries =
            (dev_slot->route_speed_entries & 0x07FFFFFF) | (static_cast<uint32_t>(dci) << 27);
    }

    const DMAAllocation tr_dma = vmm_alloc_dma(1);
    if (!tr_dma.phys)
        return false;

    g_xhci.transfer_rings[slot_id][dci] = reinterpret_cast<Trb *>(tr_dma.virt);
    g_xhci.transfer_ring_phys[slot_id][dci] = tr_dma.phys;
    kstring::zero_memory(g_xhci.transfer_rings[slot_id][dci], XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.transfer_enqueue[slot_id][dci] = 0;
    g_xhci.transfer_cycle[slot_id][dci] = 1;

    Trb *link = &g_xhci.transfer_rings[slot_id][dci][XHCI_RING_SIZE - 1];
    link->parameter = tr_dma.phys;
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;

    const uint8_t speed = (dev_slot->route_speed_entries >> 20) & 0xF;

    uint8_t xhci_interval;
    if (speed == XHCI_SPEED_LOW || speed == XHCI_SPEED_FULL) {
        uint8_t ms = interval;
        uint8_t log2 = 0;
        while (ms > 1) {
            ms >>= 1;
            log2++;
        }
        xhci_interval = (log2 + 3);
    } else {
        xhci_interval = (interval < 1) ? 0 : (interval - 1);
    }
    if (xhci_interval > 15)
        xhci_interval = 15;

    auto *ep_ctx = xhci_get_input_ep_ctx(input, dci);
    ep_ctx->ep_state = (static_cast<uint32_t>(mult & 0x3) << 8) | (static_cast<uint32_t>(xhci_interval) << 16);
    ep_ctx->ep_info = (3 << 1) | (static_cast<uint32_t>(ep_type) << 3) | (static_cast<uint32_t>(max_burst) << 8) |
                      (static_cast<uint32_t>(max_packet) << 16);
    ep_ctx->tr_dequeue = tr_dma.phys | 1;
    ep_ctx->avg_trb_length = static_cast<uint32_t>(max_packet) |
                             (static_cast<uint32_t>(max_esit_payload ? max_esit_payload : max_packet) << 16);

    Trb cmd = {g_xhci.input_context_phys[slot_id], 0,
               TRB_TYPE(TRB_TYPE_CONFIG_EP) | (static_cast<uint32_t>(slot_id) << 24)};
    Trb result;
    if (!xhci_send_command(&cmd, &result)) {
        KLOG(LogModule::Usb, LogLevel::Error, "Configure Endpoint failed: %s (Slot %d, EP %d)",
             xhci_completion_code_str(static_cast<uint8_t>((result.status >> 24) & 0xFF)), slot_id, ep_num);
        return false;
    }
    KLOG(LogModule::Usb, LogLevel::Info,
         "xHCI: Configured EP %d (Slot %d, Type %d, MaxPkt %d, Interval %d, Burst %d, Mult %d, Ring at 0x%lx)", ep_num,
         slot_id, ep_type, max_packet, interval, max_burst, mult, tr_dma.phys);
    return true;
}

bool xhci_reset_endpoint(uint8_t slot_id, uint8_t ep_num)
{
    Trb cmd = {0, 0,
               TRB_TYPE(TRB_TYPE_RESET_EP) | (static_cast<uint32_t>(slot_id) << 24) |
                   (static_cast<uint32_t>(ep_num) << 16)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_stop_endpoint(uint8_t slot_id, uint8_t ep_num)
{
    Trb cmd = {0, 0,
               TRB_TYPE(TRB_TYPE_STOP_EP) | (static_cast<uint32_t>(slot_id) << 24) |
                   (static_cast<uint32_t>(ep_num) << 16)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

static void xhci_reset_transfer_ring(uint8_t slot_id, uint8_t ep_num)
{
    kstring::zero_memory(g_xhci.transfer_rings[slot_id][ep_num], XHCI_RING_SIZE * sizeof(Trb));
    g_xhci.transfer_enqueue[slot_id][ep_num] = 0;
    g_xhci.transfer_cycle[slot_id][ep_num] = 1;

    Trb *link = &g_xhci.transfer_rings[slot_id][ep_num][XHCI_RING_SIZE - 1];
    link->parameter = g_xhci.transfer_ring_phys[slot_id][ep_num];
    link->control = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC | TRB_CYCLE;
}

static bool xhci_issue_set_tr_dequeue(uint8_t slot_id, uint8_t ep_num)
{
    Trb cmd = {g_xhci.transfer_ring_phys[slot_id][ep_num] | 1, 0,
               TRB_TYPE(TRB_TYPE_SET_TR_DEQUEUE) | (static_cast<uint32_t>(slot_id) << 24) |
                   (static_cast<uint32_t>(ep_num) << 16)};
    Trb result;
    return xhci_send_command(&cmd, &result);
}

bool xhci_set_tr_dequeue(uint8_t slot_id, uint8_t ep_num)
{
    // The endpoint must be stopped before the dequeue pointer is moved.
    xhci_stop_endpoint(slot_id, ep_num);
    xhci_reset_transfer_ring(slot_id, ep_num);
    return xhci_issue_set_tr_dequeue(slot_id, ep_num);
}

static bool xhci_wait_transfer(uint8_t slot_id, Trb *result, uint32_t timeout_ms)
{
    for (uint32_t waited_ms = 0; waited_ms < timeout_ms; ++waited_ms) {
        if (g_xhci.control_event_ready[slot_id]) {
            if (result)
                *result = g_xhci.last_control_event[slot_id];
            g_xhci.control_event_ready[slot_id] = false;
            const uint8_t cc = static_cast<uint8_t>((g_xhci.last_control_event[slot_id].status >> 24) & 0xFF);
            return cc == TRB_COMP_SUCCESS || cc == TRB_COMP_SHORT_PACKET;
        }
        // GUARDED to prevent recursive polling
        if (!g_in_poll && g_xhci.event_ring &&
            (g_xhci.event_ring[g_xhci.event_dequeue].control & TRB_CYCLE) == g_xhci.event_cycle) {
            g_in_poll = true;
            xhci_poll_events();
            g_in_poll = false;
        }
        xhci_wait_ms(1);
    }
    return false;
}

static bool xhci_wait_endpoint_transfer(uint8_t slot_id, uint8_t ep_num, Trb *result, uint32_t timeout_ms)
{
    for (uint32_t waited_ms = 0; waited_ms < timeout_ms; ++waited_ms) {
        if (g_xhci.sync_complete[slot_id][ep_num]) {
            if (result)
                *result = g_xhci.transfer_result[slot_id][ep_num];
            g_xhci.sync_complete[slot_id][ep_num] = false;
            const uint8_t cc = static_cast<uint8_t>((g_xhci.transfer_result[slot_id][ep_num].status >> 24) & 0xFF);
            return cc == TRB_COMP_SUCCESS || cc == TRB_COMP_SHORT_PACKET;
        }
        if (!g_in_poll && g_xhci.event_ring &&
            (g_xhci.event_ring[g_xhci.event_dequeue].control & TRB_CYCLE) == g_xhci.event_cycle) {
            g_in_poll = true;
            xhci_poll_events();
            g_in_poll = false;
        }
        xhci_wait_ms(1);
    }
    return false;
}

bool xhci_control_transfer(uint8_t slot_id, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                           uint16_t length, void *data, uint16_t *transferred, bool log_error)
{
    if (!g_xhci.transfer_rings[slot_id][0])
        return false;

    DMAAllocation dma = {0, 0, 0};
    if (data && length > 0) {
        dma = vmm_alloc_dma(1);
        if (!dma.phys)
            return false;
        if (!(request_type & 0x80))
            kstring::memcpy(reinterpret_cast<void *>(dma.virt), data, length);
    }

    KLOG(LogModule::Usb, LogLevel::Trace,
         "xHCI: Control Transfer: Slot %d, ReqType 0x%x, Req 0x%x, Val 0x%x, Idx 0x%x, Len %d", slot_id, request_type,
         request, value, index, length);

    spinlock_acquire(&g_xhci_lock);
    g_xhci.control_event_ready[slot_id] = false;

    Trb setup = {(static_cast<uint64_t>(request_type) | (static_cast<uint64_t>(request) << 8) |
                  (static_cast<uint64_t>(value) << 16) | (static_cast<uint64_t>(index) << 32) |
                  (static_cast<uint64_t>(length) << 48)),
                 8, TRB_TYPE(TRB_TYPE_SETUP) | TRB_IDT};
    if (length > 0)
        setup.control |= (request_type & 0x80) ? TRB_TRT_IN : TRB_TRT_OUT;
    xhci_enqueue_transfer(slot_id, 0, &setup);

    if (length > 0) {
        Trb data_trb = {dma.phys, length, TRB_TYPE(TRB_TYPE_DATA)};
        if (request_type & 0x80)
            data_trb.control |= TRB_DIR_IN;
        xhci_enqueue_transfer(slot_id, 0, &data_trb);
    }

    Trb status_trb = {0, 0, TRB_TYPE(TRB_TYPE_STATUS) | TRB_IOC};
    if (!(request_type & 0x80) || length == 0)
        status_trb.control |= TRB_DIR_IN;
    xhci_enqueue_transfer(slot_id, 0, &status_trb);

    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, DB_EP0_IN);
    spinlock_release(&g_xhci_lock);

    Trb result = {0, 0, 0};
    const bool success = xhci_wait_transfer(slot_id, &result, 2500);
    if (!success && log_error) {
        const uint8_t cc = static_cast<uint8_t>((result.status >> 24) & 0xFF);
        KLOG(LogModule::Usb, LogLevel::Error, "xHCI: Control Transfer Failed: %s (Slot %d, Req %d)",
             xhci_completion_code_str(cc), slot_id, request);
    }

    if (success && data && length > 0 && (request_type & 0x80)) {
        const uint32_t actual = length - (result.status & 0xFFFFFF);
        kstring::memcpy(data, reinterpret_cast<void *>(dma.virt), actual);
        if (transferred)
            *transferred = static_cast<uint16_t>(actual);
    }

    if (dma.phys)
        vmm_free_dma(dma);
    return success;
}

bool xhci_bulk_transfer(uint8_t slot_id, uint8_t ep_num, bool in, void *data, uint32_t length, uint32_t *transferred,
                        uint32_t timeout_ms)
{
    if (transferred)
        *transferred = 0;
    if (slot_id == 0 || ep_num == 0 || ep_num >= XHCI_MAX_ENDPOINTS)
        return false;
    if (!g_xhci.transfer_rings[slot_id][ep_num])
        return false;
    if (length != 0 && !data)
        return false;

    DMAAllocation dma = {0, 0, 0};
    if (length != 0) {
        dma = vmm_alloc_dma((length + 4095u) / 4096u);
        if (!dma.phys)
            return false;
        if (!in)
            kstring::memcpy(reinterpret_cast<void *>(dma.virt), data, length);
    }

    spinlock_acquire(&g_xhci_lock);
    g_xhci.sync_pending[slot_id][ep_num] = true;
    g_xhci.sync_complete[slot_id][ep_num] = false;

    Trb trb = {dma.phys, length, TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP};
    xhci_enqueue_transfer(slot_id, ep_num, &trb);
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, ep_num);
    spinlock_release(&g_xhci_lock);

    Trb result = {0, 0, 0};
    const bool success = xhci_wait_endpoint_transfer(slot_id, ep_num, &result, timeout_ms);
    if (!success) {
        g_xhci.sync_pending[slot_id][ep_num] = false;
        g_xhci.sync_complete[slot_id][ep_num] = false;
        if (dma.phys)
            vmm_free_dma(dma);
        const uint8_t cc = static_cast<uint8_t>((result.status >> 24) & 0xFF);
        KLOG(LogModule::Usb, LogLevel::Warn, "xHCI: bulk transfer failed on Slot %d EP %d: %s", slot_id, ep_num,
             xhci_completion_code_str(cc));
        return false;
    }

    uint32_t actual = length;
    if (length != 0) {
        const uint32_t remaining = result.status & 0xFFFFFFu;
        actual = remaining <= length ? (length - remaining) : 0;
        if (in && actual != 0)
            kstring::memcpy(data, reinterpret_cast<void *>(dma.virt), actual);
    }
    if (transferred)
        *transferred = actual;
    if (dma.phys)
        vmm_free_dma(dma);
    return true;
}

void xhci_register_interrupt_callback(uint8_t slot_id, uint8_t ep_num, XhciInterruptCallback cb)
{
    if (slot_id == 0 || ep_num >= XHCI_MAX_ENDPOINTS)
        return;
    g_xhci.intr_callbacks[slot_id][ep_num] = cb;
}

bool xhci_submit_interrupt_transfer(uint8_t slot_id, uint8_t ep_num, uint16_t length)
{
    if (slot_id == 0 || ep_num >= XHCI_MAX_ENDPOINTS)
        return false;
    if (!g_xhci.transfer_rings[slot_id][ep_num])
        return false;
    spinlock_acquire(&g_xhci_lock);

    if (g_intr_buffer_dma[slot_id][ep_num].phys == 0) {
        g_intr_buffer_dma[slot_id][ep_num] = vmm_alloc_dma(1);
        if (!g_intr_buffer_dma[slot_id][ep_num].phys) {
            spinlock_release(&g_xhci_lock);
            return false;
        }
    }

    if (g_xhci.intr_pending[slot_id][ep_num]) {
        spinlock_release(&g_xhci_lock);
        return false;
    }

    g_intr_length[slot_id][ep_num] = length;
    Trb trb = {g_intr_buffer_dma[slot_id][ep_num].phys, length, TRB_TYPE(TRB_TYPE_NORMAL) | TRB_IOC | TRB_ISP};
    xhci_enqueue_transfer(slot_id, ep_num, &trb);
    asm volatile("mfence" ::: "memory");
    xhci_ring_doorbell(slot_id, ep_num);
    g_xhci.intr_pending[slot_id][ep_num] = true;
    g_xhci.intr_complete[slot_id][ep_num] = false;
    g_xhci.intr_start_time[slot_id][ep_num] = timer_get_ticks();
    g_intr_recovery_needed[slot_id][ep_num] = false;

    spinlock_release(&g_xhci_lock);
    return true;
}

bool xhci_interrupt_transfer_pending(uint8_t slot_id, uint8_t ep_num)
{
    if (slot_id == 0 || ep_num >= XHCI_MAX_ENDPOINTS)
        return false;
    return g_xhci.intr_pending[slot_id][ep_num];
}

void xhci_poll_events()
{
    if (!g_xhci_initialized || !g_xhci.event_ring)
        return;

    struct PendingAction
    {
        XhciInterruptCallback cb;
        void *data;
        uint32_t len;
        uint8_t slot;
        uint8_t ep;
        bool invoke_cb;
        bool recover;
    };
    PendingAction actions[32];
    int action_count = 0;

    spinlock_acquire(&g_xhci_lock);
    auto *ir = reinterpret_cast<volatile XhciInterrupterRegs *>(reinterpret_cast<uintptr_t>(g_xhci.runtime) + 0x20);
    const uint32_t iman = mmio_read32(const_cast<uint32_t *>(&ir->iman));
    // RW1C: Only write back IP to acknowledge, preserve IE
    if (iman & IMAN_IP)
        mmio_write32(const_cast<uint32_t *>(&ir->iman), IMAN_IP | IMAN_IE);

    const uint32_t usbsts = mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbsts));
    uint32_t clear_sts = 0;
    if (usbsts & USBSTS_EINT)
        clear_sts |= USBSTS_EINT;
    if (usbsts & USBSTS_PCD)
        clear_sts |= USBSTS_PCD;
    if (clear_sts)
        mmio_write32(const_cast<uint32_t *>(&g_xhci.op->usbsts), clear_sts);

    int count = 0;
    while (count++ < 64) {
        Trb *evt = &g_xhci.event_ring[g_xhci.event_dequeue];
        if ((evt->control & TRB_CYCLE) != g_xhci.event_cycle)
            break;

        const uint8_t type = TRB_GET_TYPE(evt->control);
        if (type == TRB_TYPE_TRANSFER_EVENT) {
            const uint8_t slot = static_cast<uint8_t>((evt->control >> 24) & 0xFF);
            const uint8_t ep = static_cast<uint8_t>((evt->control >> 16) & 0x1F);

            if (slot == 0 || ep >= XHCI_MAX_ENDPOINTS) {
                KLOG(LogModule::Usb, LogLevel::Warn,
                     "xHCI: Ignoring transfer event with invalid slot/endpoint (Slot %d, EP %d)", slot, ep);
            } else if (ep == 1) { // Control Transfer (EP0)
                g_xhci.last_control_event[slot] = *evt;
                g_xhci.control_event_ready[slot] = true;
            } else if (g_xhci.sync_pending[slot][ep]) {
                g_xhci.transfer_result[slot][ep] = *evt;
                g_xhci.sync_pending[slot][ep] = false;
                g_xhci.sync_complete[slot][ep] = true;
            } else if (g_xhci.intr_pending[slot][ep]) {
                g_xhci.transfer_result[slot][ep] = *evt;
                g_xhci.intr_complete[slot][ep] = true;
                g_xhci.intr_pending[slot][ep] = false;
                g_xhci.intr_start_time[slot][ep] = 0;

                const uint8_t cc = static_cast<uint8_t>((evt->status >> 24) & 0xFF);
                g_intr_last_cc[slot][ep] = cc;
                if (cc == TRB_COMP_SUCCESS || cc == TRB_COMP_SHORT_PACKET) {
                    if (g_xhci.intr_callbacks[slot][ep] &&
                        action_count < static_cast<int>(sizeof(actions) / sizeof(actions[0]))) {
                        uint32_t transferred = g_intr_length[slot][ep] - (evt->status & 0xFFFFFF);
                        actions[action_count++] = {g_xhci.intr_callbacks[slot][ep],
                                                   reinterpret_cast<void *>(g_intr_buffer_dma[slot][ep].virt),
                                                   transferred,
                                                   slot,
                                                   ep,
                                                   true,
                                                   false};
                    }
                } else {
                    KLOG(LogModule::Usb, LogLevel::Warn, "xHCI: Transfer Error on Slot %d, EP %d: %s", slot, ep,
                         xhci_completion_code_str(cc));
                    g_intr_recovery_needed[slot][ep] = true;
                    if (action_count < static_cast<int>(sizeof(actions) / sizeof(actions[0]))) {
                        actions[action_count++] = {nullptr, nullptr, 0, slot, ep, false, true};
                    }
                }
            } else if (ep != 1) {
                g_intr_last_cc[slot][ep] = 0;
                KLOG(LogModule::Usb, LogLevel::Warn,
                     "xHCI: Unexpected transfer event on Slot %d, EP %d without a pending TD", slot, ep);
                g_intr_recovery_needed[slot][ep] = true;
                if (action_count < static_cast<int>(sizeof(actions) / sizeof(actions[0]))) {
                    actions[action_count++] = {nullptr, nullptr, 0, slot, ep, false, true};
                }
            }
        } else if (type == TRB_TYPE_COMMAND_COMPLETION) {
            const uint8_t cc = static_cast<uint8_t>((evt->status >> 24) & 0xFF);
            (void)cc;
            KLOG(LogModule::Usb, LogLevel::Trace, "xHCI: Command Completion: CC=%s", xhci_completion_code_str(cc));
            g_xhci.last_command_event = *evt;
            g_xhci.command_event_ready = true;
        } else if (type == TRB_TYPE_PORT_STATUS_CHANGE) {
            const uint8_t port = static_cast<uint8_t>((evt->parameter >> 24) & 0xFF);
            if (port > 0 && port <= g_xhci.max_ports) {
                const uint32_t val = mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[port - 1].portsc));
                KLOG(LogModule::Usb, LogLevel::Info, "xHCI: Port %d Status Change (PORTSC: 0x%x)", port, val);
                if (val & PORTSC_CSC)
                    g_xhci.port_needs_enumeration[port - 1] = true;
                mmio_write32(const_cast<uint32_t *>(&g_xhci.ports[port - 1].portsc),
                             (val & (PORTSC_CSC | PORTSC_PEC | PORTSC_OCC)) | PORTSC_PP);
            }
        }

        if (++g_xhci.event_dequeue >= XHCI_EVENT_RING_SIZE) {
            g_xhci.event_dequeue = 0;
            g_xhci.event_cycle ^= 1;
        }
    }

    // Always write ERDP, even if count==1, to clear the EHB flag since we acknowledged the interrupt line
    mmio_write64(const_cast<uint64_t *>(&ir->erdp),
                 (g_xhci.event_ring_phys + g_xhci.event_dequeue * sizeof(Trb)) | ERDP_EHB);

    spinlock_release(&g_xhci_lock);

    for (int i = 0; i < action_count; i++) {
        if (actions[i].invoke_cb && actions[i].cb) {
            actions[i].cb(actions[i].slot, actions[i].ep, actions[i].data, static_cast<uint16_t>(actions[i].len));
            if (!g_intr_recovery_needed[actions[i].slot][actions[i].ep]) {
                xhci_submit_interrupt_transfer(actions[i].slot, actions[i].ep,
                                               g_intr_length[actions[i].slot][actions[i].ep]);
            }
        }

        if (actions[i].recover) {
            xhci_recover_interrupt_endpoint(actions[i].slot, actions[i].ep);
        }
    }
}

static void xhci_recover_interrupt_endpoint(uint8_t slot_id, uint8_t ep_num)
{
    if (slot_id == 0 || ep_num >= XHCI_MAX_ENDPOINTS)
        return;
    if (!g_xhci.transfer_rings[slot_id][ep_num])
        return;
    if (!g_intr_recovery_needed[slot_id][ep_num])
        return;

    KLOG(LogModule::Usb, LogLevel::Warn, "xHCI: Recovering interrupt endpoint %d on slot %d", ep_num, slot_id);
    g_intr_recovery_needed[slot_id][ep_num] = false;
    g_xhci.intr_pending[slot_id][ep_num] = false;
    g_xhci.intr_complete[slot_id][ep_num] = false;
    g_xhci.intr_start_time[slot_id][ep_num] = 0;

    if (!xhci_stop_endpoint(slot_id, ep_num))
        return;
    if (!xhci_reset_endpoint(slot_id, ep_num))
        return;
    xhci_reset_transfer_ring(slot_id, ep_num);
    if (!xhci_issue_set_tr_dequeue(slot_id, ep_num))
        return;

    if (g_intr_last_cc[slot_id][ep_num] == TRB_COMP_STALL) {
        const uint16_t ep_addr = static_cast<uint16_t>((ep_num / 2) | ((ep_num % 2) ? 0x80 : 0));
        xhci_control_transfer(slot_id, USB_REQ_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD | USB_REQ_RECIPIENT_ENDPOINT,
                              USB_REQ_CLEAR_FEATURE, 0, ep_addr, 0, nullptr, nullptr);
    }

    if (g_intr_length[slot_id][ep_num] != 0) {
        xhci_submit_interrupt_transfer(slot_id, ep_num, g_intr_length[slot_id][ep_num]);
    }
}

bool xhci_has_pending_events()
{
    if (!g_xhci_initialized || !g_xhci.event_ring)
        return false;
    return (g_xhci.event_ring[g_xhci.event_dequeue].control & TRB_CYCLE) == g_xhci.event_cycle;
}

bool xhci_wait_for_event(uint32_t timeout_ms)
{
    uint32_t timeout = timeout_ms * 1000;
    while (timeout--) {
        if ((g_xhci.event_ring[g_xhci.event_dequeue].control & TRB_CYCLE) == g_xhci.event_cycle)
            return true;
        io_wait();
    }
    return false;
}

void xhci_dump_status()
{
    if (!g_xhci_initialized) {
        KLOG(LogModule::Usb, LogLevel::Warn, "xHCI not initialized");
        return;
    }

    KLOG(LogModule::Usb, LogLevel::Info, "xHCI Status: USBSTS=0x%x",
         mmio_read32(const_cast<uint32_t *>(&g_xhci.op->usbsts)));
    for (uint8_t i = 0; i < g_xhci.max_ports && i < 8; i++) {
        const uint32_t portsc = mmio_read32(const_cast<uint32_t *>(&g_xhci.ports[i].portsc));
        (void)portsc;
        KLOG(LogModule::Usb, LogLevel::Trace, "Port %d: PORTSC=0x%x CCS=%d PED=%d Speed=%d", i + 1, portsc,
             (portsc & PORTSC_CCS) ? 1 : 0, (portsc & PORTSC_PED) ? 1 : 0, (portsc >> PORTSC_SPEED_SHIFT) & 0xF);
    }
}
