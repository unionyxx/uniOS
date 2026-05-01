#include <drivers/apic/ioapic.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <drivers/class/hid/ps2_mouse.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/debug.h>
#include <kernel/irq.h>
#include <kernel/mm/vmm.h>
#include <kernel/scheduler.h>
#include <kernel/time/timer.h>
#include <stdint.h>

static uint64_t g_lapic_base = 0;
static bool g_apic_enabled = false;
static IrqVectorHandler g_vector_handlers[IDT_ENTRIES] = {};
static void *g_vector_contexts[IDT_ENTRIES] = {};

namespace {
constexpr uint8_t kVectorBase = 32;
constexpr uint8_t kVectorTimer = 32;
constexpr uint8_t kVectorKeyboard = 33;
constexpr uint8_t kVectorMouse = 44;
constexpr uint8_t kVectorSpurious = 0xFF;

constexpr uint32_t LAPIC_ID = 0x020;
constexpr uint32_t LAPIC_TPR = 0x080;
constexpr uint32_t LAPIC_EOI = 0x0B0;
constexpr uint32_t LAPIC_SVR = 0x0F0;
constexpr uint32_t LAPIC_LVT_TIMER = 0x320;
constexpr uint32_t LAPIC_INITCNT = 0x380;
constexpr uint32_t LAPIC_CURRCNT = 0x390;
constexpr uint32_t LAPIC_DIVIDE = 0x3E0;

constexpr uint32_t LAPIC_SW_ENABLE = 1u << 8;
constexpr uint32_t LAPIC_TIMER_MASK = 1u << 16;
constexpr uint32_t LAPIC_TIMER_PER = 1u << 17;
constexpr uint32_t LAPIC_DIVIDE_16 = 0x3;   // xAPIC divide-by-16 encoding
constexpr uint32_t PIT_10MS_RELOAD = 11931; // 1.193182 MHz / 100

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

void apic_timer_init(uint32_t frequency)
{
    if (!g_lapic_base || frequency == 0)
        return;

    // Put timer in a known masked state before calibration.
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASK | kVectorTimer);
    lapic_write(LAPIC_DIVIDE, LAPIC_DIVIDE_16);
    lapic_write(LAPIC_INITCNT, 0xFFFFFFFFu);

    const bool calibrated = pit_wait_10ms_via_channel2();
    const uint32_t elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_CURRCNT);

    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_MASK | kVectorTimer);

    uint64_t ticks_per_10ms = elapsed;
    if (!calibrated || ticks_per_10ms == 0) {
        BOOT_WARN("APIC: PIT calibration failed/timed out, using fallback");
        ticks_per_10ms = 100000; // 10 MHz fallback for a 10 ms window
    }

    uint64_t initcnt = (ticks_per_10ms * 100ull) / static_cast<uint64_t>(frequency);
    if (initcnt == 0)
        initcnt = 1;
    if (initcnt > 0xFFFFFFFFull)
        initcnt = 0xFFFFFFFFull;

    lapic_write(LAPIC_DIVIDE, LAPIC_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, kVectorTimer | LAPIC_TIMER_PER);
    lapic_write(LAPIC_INITCNT, static_cast<uint32_t>(initcnt));

    BOOT_LOG("APIC: timer initialized at %u Hz (initcnt=%u)", frequency, static_cast<uint32_t>(initcnt));
}

void apic_init()
{
    constexpr uint64_t k_default_lapic_phys = 0xFEE00000;
    uint64_t lapic_phys = k_default_lapic_phys;
    g_lapic_base = 0;
    g_apic_enabled = false;

    AcpiMadtHeader *madt = (AcpiMadtHeader *)acpi_find_table("APIC");
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
        timer_handler();
        send_interrupt_eoi(vector);
        scheduler_schedule();
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
