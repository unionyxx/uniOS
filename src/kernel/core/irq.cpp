#include <drivers/acpi/acpi.h>
#include <drivers/apic/ioapic.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/irq.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/time/timer.h>
#include <stdint.h>

static uint64_t g_lapic_base = 0;
static bool g_apic_enabled = false;
static uint32_t g_lapic_timer_initcnt = 0;
static IrqVectorHandler g_vector_handlers[IDT_ENTRIES] = {};
static void *g_vector_contexts[IDT_ENTRIES] = {};

namespace {
constexpr uint8_t kVectorBase = 32;
constexpr uint8_t kVectorTimer = 32;
constexpr uint8_t kVectorKeyboard = 33;
constexpr uint8_t kVectorMouse = 44;
constexpr uint8_t kVectorSpurious = 0xFF;
static uint8_t g_resched_vector = 0;
static uint8_t g_stop_vector = 0;
static volatile uint32_t g_stop_initiated = 0;

constexpr uint32_t LAPIC_ID = 0x020;
constexpr uint32_t LAPIC_TPR = 0x080;
constexpr uint32_t LAPIC_EOI = 0x0B0;
constexpr uint32_t LAPIC_SVR = 0x0F0;
constexpr uint32_t LAPIC_ICR_LO = 0x300;
constexpr uint32_t LAPIC_ICR_HI = 0x310;
constexpr uint32_t LAPIC_LVT_TIMER = 0x320;
constexpr uint32_t LAPIC_INITCNT = 0x380;
constexpr uint32_t LAPIC_CURRCNT = 0x390;
constexpr uint32_t LAPIC_DIVIDE = 0x3E0;

constexpr uint32_t LAPIC_SW_ENABLE = 1u << 8;
constexpr uint32_t LAPIC_TIMER_MASK = 1u << 16;
constexpr uint32_t LAPIC_TIMER_PER = 1u << 17;
constexpr uint32_t LAPIC_DIVIDE_16 = 0x3;   // xAPIC divide-by-16 encoding
constexpr uint32_t PIT_10MS_RELOAD = 11931; // 1.193182 MHz / 100

// ICR low dword layouts: delivery mode sits in bits [10:8], level assert in
// bit 14, trigger mode in bit 15. INIT = 101b, STARTUP = 110b — both require
// the assert bit. INIT de-assert (MP spec) is level-triggered INIT with the
// level bit cleared.
constexpr uint32_t ICR_DELIVERY_INIT = 0x00004500;
constexpr uint32_t ICR_DELIVERY_INIT_DEASSERT = 0x00008500;
constexpr uint32_t ICR_DELIVERY_SIPI = 0x00004600;

inline volatile uint32_t *lapic_reg(uint32_t off)
{
    return reinterpret_cast<volatile uint32_t *>(g_lapic_base + off);
}

inline uint32_t lapic_read(uint32_t off)
{
    return *lapic_reg(off);
}

inline void lapic_write(uint32_t off, uint32_t value)
{
    *lapic_reg(off) = value;
    asm volatile("" ::: "memory");
}

inline void mask_pic()
{
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

inline void send_interrupt_eoi(uint8_t vector)
{
    if (g_apic_enabled && g_lapic_base) {
        lapic_write(LAPIC_EOI, 0);
        return;
    }

    if (vector >= kVectorBase && vector < kVectorBase + 16)
        pic_send_eoi(static_cast<uint8_t>(vector - kVectorBase));
}

inline uint32_t isa_irq_to_gsi(uint8_t irq)
{
    return ioapic_irq_to_gsi(irq);
}

bool pit_wait_10ms_via_channel2()
{
    // Use PIT channel 2 for calibration so we do not repurpose channel 0.
    // ch2, lobyte/hibyte, mode 1 (hardware one-shot), binary
    outb(0x43, 0xB2);
    outb(0x42, static_cast<uint8_t>(PIT_10MS_RELOAD & 0xFF));
    outb(0x42, static_cast<uint8_t>((PIT_10MS_RELOAD >> 8) & 0xFF));

    uint8_t gate = inb(0x61);
    gate &= static_cast<uint8_t>(~0x02u); // speaker off
    gate |= 0x01u;                        // gate high
    outb(0x61, gate);

    // Retrigger: gate low -> high
    outb(0x61, static_cast<uint8_t>(gate & ~0x01u));
    outb(0x61, gate);

    for (uint32_t timeout = 10000000; timeout != 0; --timeout) {
        if (inb(0x61) & 0x20u)
            return true;
        asm volatile("pause");
    }
    return false;
}

static bool dispatch_vector_handler(uint8_t vector)
{
    IrqVectorHandler handler = g_vector_handlers[vector];
    void *ctx = g_vector_contexts[vector];

    if (!handler)
        return false;

    handler(vector, ctx);
    return true;
}

} // namespace

// cppcheck-suppress arrayIndexOutOfBoundsCond
bool irq_register_vector_handler(uint8_t vector, IrqVectorHandler handler, void *ctx)
{
    if (!handler)
        return false;

    if (vector < kVectorBase || vector == kVectorSpurious || vector == 0x80)
        return false;

    if (g_vector_handlers[vector] != nullptr)
        return false;

    // Publish ctx before handler.
    g_vector_contexts[vector] = ctx;
    asm volatile("" ::: "memory");
    g_vector_handlers[vector] = handler;
    return true;
}

// cppcheck-suppress arrayIndexOutOfBoundsCond
void irq_unregister_vector_handler(uint8_t vector)
{
    if (vector < kVectorBase || vector == kVectorSpurious || vector == 0x80)
        return;

    g_vector_handlers[vector] = nullptr;
    asm volatile("" ::: "memory");
    g_vector_contexts[vector] = nullptr;
}

uint8_t irq_isa_to_vector(uint8_t irq)
{
    return static_cast<uint8_t>(kVectorBase + irq);
}

bool irq_register_isa_handler(uint8_t irq, IrqVectorHandler handler, void *ctx)
{
    if (irq >= 16 || !handler)
        return false;

    const uint8_t vector = irq_isa_to_vector(irq);
    if (!irq_register_vector_handler(vector, handler, ctx))
        return false;

    if (g_apic_enabled) {
        const uint32_t gsi = isa_irq_to_gsi(irq);
        ioapic_set_entry(gsi, vector);
        return true;
    }

    pic_clear_mask(irq);
    return true;
}

void irq_unregister_isa_handler(uint8_t irq)
{
    if (irq >= 16)
        return;

    irq_unregister_vector_handler(irq_isa_to_vector(irq));
}

// Delays for `us` microseconds using the ACPI PM timer (fixed 3.579545 MHz).
// Returns false when no PM timer is available or it does not advance. The
// 24-bit modular delta accumulates correctly across counter wrap as long as
// the loop samples more often than the 24-bit wrap period (~4.7 s).
[[nodiscard]] static bool pm_timer_wait_us(uint64_t us)
{
    const uint32_t pm_blk = acpi_get_pm_timer_block();
    if (pm_blk == 0)
        return false;

    const uint64_t target = (us * 3579545ULL + 999999ULL) / 1000000ULL;
    uint32_t last = inl(pm_blk);
    uint64_t elapsed = 0;
    for (uint64_t guard = 0; guard < 100000000ULL; guard++) {
        const uint32_t cur = inl(pm_blk);
        elapsed += (cur - last) & 0xFFFFFFu;
        last = cur;
        if (elapsed >= target)
            return true;
        asm volatile("pause");
    }
    return false;
}

void apic_timer_init(uint32_t frequency)
{
    if (!g_lapic_base || frequency == 0)
        return;

    // Put timer in a known masked state before calibration.
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASK | kVectorTimer);
    lapic_write(LAPIC_DIVIDE, LAPIC_DIVIDE_16);
    lapic_write(LAPIC_INITCNT, 0xFFFFFFFFu);

    bool calibrated = pit_wait_10ms_via_channel2();
    uint32_t elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_CURRCNT);

    // On modern systems the legacy PIT can be gated off, making PIT
    // calibration fail. Fall back to the fixed-rate ACPI PM timer.
    if (!calibrated || elapsed == 0) {
        lapic_write(LAPIC_INITCNT, 0xFFFFFFFFu);
        if (pm_timer_wait_us(10000)) {
            elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_CURRCNT);
            calibrated = elapsed != 0;
        }
    }

    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASK | kVectorTimer);

    uint64_t ticks_per_10ms = elapsed;
    if (!calibrated || ticks_per_10ms == 0) {
        BOOT_WARN("APIC: PIT/PM-timer calibration failed, using fallback");
        ticks_per_10ms = 100000; // 10 MHz fallback for a 10 ms window
    }

    uint64_t initcnt = (ticks_per_10ms * 100ull) / static_cast<uint64_t>(frequency);
    if (initcnt == 0)
        initcnt = 1;
    if (initcnt > 0xFFFFFFFFull)
        initcnt = 0xFFFFFFFFull;

    g_lapic_timer_initcnt = static_cast<uint32_t>(initcnt);
    BOOT_LOG("APIC: timer calibrated (initcnt=%u)", g_lapic_timer_initcnt);

    apic_timer_start_this_core(g_lapic_timer_initcnt);
    timer_set_frequency(frequency);
}

// Programs this core's LAPIC timer without PIT recalibration (PIT channel 2
// is shared hardware; APs reuse the BSP's calibrated count).
void apic_timer_start_this_core(uint32_t initcnt)
{
    if (!g_lapic_base || initcnt == 0)
        return;

    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASK | kVectorTimer);
    lapic_write(LAPIC_DIVIDE, LAPIC_DIVIDE_16);
    lapic_write(LAPIC_INITCNT, initcnt);
    lapic_write(LAPIC_LVT_TIMER, kVectorTimer | LAPIC_TIMER_PER);
}

uint32_t apic_timer_bsp_initcnt()
{
    return g_lapic_timer_initcnt;
}

namespace {
// AP tick divisor: APs take the timer IRQ at tick_rate / 10. Idle cores stop
// hammering the global scheduler lock 1000x/second, and busy cores only give
// up 1 ms -> 10 ms forced-preemption granularity (newly-ready work still
// arrives promptly via the RESCHED IPI).
constexpr uint32_t LAPIC_AP_TIMER_DIVISOR = 10;
} // namespace

uint32_t apic_timer_ap_initcnt()
{
    if (g_lapic_timer_initcnt == 0)
        return 0;
    const uint64_t scaled = static_cast<uint64_t>(g_lapic_timer_initcnt) * LAPIC_AP_TIMER_DIVISOR;
    return scaled > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(scaled);
}

uint32_t apic_timer_ap_divisor()
{
    return LAPIC_AP_TIMER_DIVISOR;
}

// Enables the LAPIC on the current core. g_lapic_base is process-global
// (same physical address on every core), only the MSRs/registers are per-core.
void apic_enable_this_core()
{
    if (!g_lapic_base)
        return;

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_SVR, LAPIC_SW_ENABLE | kVectorSpurious);
}

// If firmware left the local APIC in x2APIC mode, MMIO register access does
// nothing and interrupts silently stop working. Attempt the documented
// transition back to xAPIC mode (disable the APIC, clear the EXT bit, then
// re-enable). Some CPUs only allow this via reset, so treat it as best-effort
// and warn loudly if x2APIC stays on.
static void apic_force_xapic_mode()
{
    constexpr uint32_t kIa32ApicBase = 0x1B;
    constexpr uint64_t kApicGlobalEnable = 1ULL << 11;
    constexpr uint64_t kApicX2ApicEnable = 1ULL << 10;

    uint32_t lo = 0, hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(kIa32ApicBase));
    uint64_t base = (static_cast<uint64_t>(hi) << 32) | lo;

    if ((base & kApicX2ApicEnable) == 0)
        return;

    BOOT_WARN("APIC: x2APIC mode is enabled, attempting transition to xAPIC");

    // Disable the APIC, clear x2APIC, then re-enable.
    base &= ~kApicGlobalEnable;
    asm volatile("wrmsr" ::"c"(kIa32ApicBase), "a"(static_cast<uint32_t>(base)),
                 "d"(static_cast<uint32_t>(base >> 32)));
    base &= ~kApicX2ApicEnable;
    asm volatile("wrmsr" ::"c"(kIa32ApicBase), "a"(static_cast<uint32_t>(base)),
                 "d"(static_cast<uint32_t>(base >> 32)));
    base |= kApicGlobalEnable;
    asm volatile("wrmsr" ::"c"(kIa32ApicBase), "a"(static_cast<uint32_t>(base)),
                 "d"(static_cast<uint32_t>(base >> 32)));

    lo = 0;
    hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(kIa32ApicBase));
    const uint64_t check = (static_cast<uint64_t>(hi) << 32) | lo;
    if (check & kApicX2ApicEnable)
        BOOT_WARN("APIC: unable to leave x2APIC mode; MMIO APIC access may not function");
    else
        BOOT_LOG("APIC: switched to xAPIC mode");
}

void apic_init()
{
    constexpr uint64_t k_default_lapic_phys = 0xFEE00000;
    uint64_t lapic_phys = k_default_lapic_phys;
    g_lapic_base = 0;
    g_apic_enabled = false;

    apic_force_xapic_mode();

    AcpiMadtHeader *madt = reinterpret_cast<AcpiMadtHeader *>(acpi_find_table("APIC"));
    if (madt) {
        if (madt->local_apic_address != 0) {
            lapic_phys = madt->local_apic_address;
            BOOT_LOG("APIC: LAPIC base from MADT header: 0x%lx", lapic_phys);
        } else {
            BOOT_WARN("APIC: MADT LAPIC base is zero, falling back to default 0x%lx", k_default_lapic_phys);
        }

        uint8_t *ptr = reinterpret_cast<uint8_t *>(madt) + sizeof(AcpiMadtHeader);
        uint8_t *end = reinterpret_cast<uint8_t *>(madt) + madt->header.length;
        while (ptr + sizeof(AcpiMadtRecord) <= end) {
            auto *record = reinterpret_cast<AcpiMadtRecord *>(ptr);
            if (record->length < sizeof(AcpiMadtRecord) || ptr + record->length > end) {
                BOOT_WARN("APIC: malformed MADT record type %u length %u", record->type, record->length);
                break;
            }

            if (record->type == 5) {
                if (record->length >= sizeof(AcpiMadtLapicAddressOverride)) {
                    auto *override_record = reinterpret_cast<AcpiMadtLapicAddressOverride *>(ptr);
                    lapic_phys = override_record->local_apic_address;
                    BOOT_LOG("APIC: LAPIC base override from MADT: 0x%lx", lapic_phys);
                } else {
                    BOOT_WARN("APIC: short MADT LAPIC address override record");
                }
                break;
            }

            ptr += record->length;
        }
    } else {
        BOOT_WARN("APIC: MADT not found, using default LAPIC address 0xFEE00000");
    }

    g_lapic_base = vmm_map_mmio(lapic_phys, 0x1000);
    if (!g_lapic_base) {
        if (lapic_phys != k_default_lapic_phys) {
            BOOT_WARN("APIC: failed to map LAPIC MMIO at 0x%lx, retrying default 0x%lx", lapic_phys,
                      k_default_lapic_phys);
            lapic_phys = k_default_lapic_phys;
            g_lapic_base = vmm_map_mmio(lapic_phys, 0x1000);
        }
        if (!g_lapic_base) {
            BOOT_ERROR("APIC: failed to map LAPIC MMIO at 0x%lx, keeping PIC/PIT fallback active", lapic_phys);
            return;
        }
    }

    mask_pic();

    lapic_write(LAPIC_TPR, 0);

    lapic_write(LAPIC_SVR, LAPIC_SW_ENABLE | kVectorSpurious);
    g_apic_enabled = true;
    BOOT_SUCCESS("APIC: LAPIC enabled at virtual address 0x%lx", g_lapic_base);

    ioapic_init();

    // Do not hardcode keyboard/mouse routing here if registration also programs IOAPIC.
    apic_timer_init(1000);

    // Dedicated RESCHED vector for waking idle cores (SMP scheduling).
    // Handled directly in irq_handler before generic dispatch; allocating the
    // vector reserves it from other users.
    if (g_resched_vector == 0)
        g_resched_vector = idt_allocate_free_vector();
    if (g_stop_vector == 0)
        g_stop_vector = idt_allocate_free_vector();
}

// Wakes every other core so idle ones re-run schedule() and pull work.
void apic_send_resched_ipi_to_others()
{
    if (g_resched_vector != 0)
        apic_send_ipi_all_excluding_self(g_resched_vector);
}

void apic_stop_other_cpus()
{
    if (g_stop_vector == 0 || g_cpu_online_count <= 1)
        return;

    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_stop_initiated, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_store_n(&g_cpu_stopped_count, 0, __ATOMIC_RELEASE);
    apic_send_ipi_all_excluding_self(g_stop_vector);

    const int target = __atomic_load_n(&g_cpu_online_count, __ATOMIC_ACQUIRE) - 1;
    constexpr uint32_t kMaxStopPolls = 10000000;
    for (uint32_t i = 0; i < kMaxStopPolls; i++) {
        if (__atomic_load_n(&g_cpu_stopped_count, __ATOMIC_ACQUIRE) >= target)
            break;
        asm volatile("pause");
    }
}

extern "C" void irq_handler(void *stack_frame)
{
    // This magic offset is ABI-fragile. Replace with a real InterruptFrame struct.
    const uint8_t vector = static_cast<uint8_t>(static_cast<uint64_t *>(stack_frame)[15]);

    if (vector == kVectorSpurious)
        return;

    if (vector < kVectorBase)
        return;

    if (vector == kVectorTimer) {
        // Elapsed scheduler jiffies for this IRQ: 1 on the BSP (1 kHz clock),
        // the AP divisor on APs (their timer fires that much less often).
        const uint32_t elapsed_jiffies = timer_handler();
        send_interrupt_eoi(vector);
        Process *curr = process_get_current();
        if (curr && curr->preempt_count > 0) {
            curr->preempt_pending = 1;
        } else {
            scheduler_schedule_elapsed(elapsed_jiffies);
        }
        return;
    }

    // Resched IPI: EOI first (like the timer), then schedule. If the schedule
    // switches away, this handler never returns — that is by design and safe
    // because the EOI already happened.
    if (g_resched_vector != 0 && vector == g_resched_vector) {
        send_interrupt_eoi(vector);
        scheduler_schedule();
        return;
    }

    // Stop IPI: never enter the scheduler or acquire a kernel lock here.
    // This path is used while another CPU is panicking or resetting.
    if (g_stop_vector != 0 && vector == g_stop_vector) {
        send_interrupt_eoi(vector);
        cpu_get_local()->stop_requested = true;
        __sync_fetch_and_add(&g_cpu_stopped_count, 1);
        asm volatile("cli\n"
                     "1:\n"
                     "hlt\n"
                     "jmp 1b\n");
        return;
    }

    switch (vector) {
        case kVectorKeyboard:
            ps2_keyboard_handler();
            break;
        case kVectorMouse:
            ps2_mouse_handler();
            break;
        default:
            dispatch_vector_handler(vector);
            break;
    }

    send_interrupt_eoi(vector);
}

uint32_t apic_get_current_id()
{
    if (!g_lapic_base)
        return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

bool apic_is_enabled()
{
    return g_apic_enabled && g_lapic_base != 0;
}

void apic_send_eoi()
{
    if (g_apic_enabled && g_lapic_base)
        lapic_write(LAPIC_EOI, 0);
}

static bool icr_send_to(uint8_t dest_apic_id, uint32_t icr_low)
{
    if (!g_apic_enabled || !g_lapic_base)
        return false;

    // Bounded waits: some virtual LAPICs keep the delivery-status bit sticky
    // for INIT/SIPI, so an unbounded spin here would wedge the BSP.
    constexpr uint32_t kMaxStatusPolls = 100000000u;

    // Wait for any previous IPI to clear
    for (uint32_t i = 0; i < kMaxStatusPolls && (lapic_read(LAPIC_ICR_LO) & (1u << 12)); i++) {
        asm volatile("pause");
    }

    lapic_write(LAPIC_ICR_HI, static_cast<uint32_t>(dest_apic_id) << 24);
    lapic_write(LAPIC_ICR_LO, icr_low);
    for (uint32_t i = 0; i < kMaxStatusPolls && (lapic_read(LAPIC_ICR_LO) & (1u << 12)); i++) {
        asm volatile("pause");
    }
    return (lapic_read(LAPIC_ICR_LO) & (1u << 12)) == 0;
}

bool apic_send_init_ipi(uint8_t dest_apic_id)
{
    return icr_send_to(dest_apic_id, ICR_DELIVERY_INIT);
}

bool apic_send_init_deassert_ipi(uint8_t dest_apic_id)
{
    return icr_send_to(dest_apic_id, ICR_DELIVERY_INIT_DEASSERT);
}

bool apic_send_sipi(uint8_t dest_apic_id, uint8_t vector)
{
    return icr_send_to(dest_apic_id, ICR_DELIVERY_SIPI | vector);
}

bool apic_send_ipi_to(uint8_t dest_apic_id, uint8_t vector)
{
    return icr_send_to(dest_apic_id, 0x00004000 | vector);
}

void apic_send_ipi_all_excluding_self(uint8_t vector)
{
    if (!g_apic_enabled || !g_lapic_base)
        return;

    // Bound the waits: panic/shutdown must never wedge forever on a virtual
    // or faulty LAPIC with sticky delivery status.
    constexpr uint32_t kMaxStatusPolls = 100000000u;

    // Writing the ICR while a previous IPI is still in flight drops that IPI;
    // wait it out first (this path carries stop-IPIs and resched broadcasts).
    for (uint32_t i = 0; i < kMaxStatusPolls && (lapic_read(LAPIC_ICR_LO) & (1u << 12)); i++) {
        asm volatile("pause");
    }

    // Clear Destination Field in ICR High
    lapic_write(LAPIC_ICR_HI, 0);

    // Destination Shorthand: All Excluding Self (0xC0000), Fixed delivery.
    // The Level bit only applies to INIT delivery and must stay 0 here — some
    // LAPIC implementations flag it on Fixed IPIs.
    lapic_write(LAPIC_ICR_LO, 0x000C0000 | vector);

    for (uint32_t i = 0; i < kMaxStatusPolls && (lapic_read(LAPIC_ICR_LO) & (1u << 12)); i++) {
        asm volatile("pause");
    }
}
