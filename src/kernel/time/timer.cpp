#include <drivers/acpi/acpi.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/irq.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/time/timer.h>

static volatile uint64_t ticks = 0;
static uint32_t tick_frequency = 0;

namespace {

constexpr uint16_t PIT_CHANNEL2_DATA = 0x42;
constexpr uint16_t PIT_SPEAKER_PORT = 0x61;
constexpr uint32_t PIT_BASE_HZ = 1193182u;
constexpr uint32_t PM_TIMER_HZ = 3579545u;

uint64_t g_tsc_freq_hz = 0;
uint64_t g_tsc_base = 0;

inline uint64_t read_tsc()
{
    uint32_t lo = 0, hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
}

[[nodiscard]] bool pit_channel2_wait_reload(uint16_t reload)
{
    if (reload == 0)
        reload = 1;

    // Channel 2, lobyte/hibyte, mode 1 (hardware one-shot), binary.
    outb(PIT_COMMAND, 0xB2);
    outb(PIT_CHANNEL2_DATA, static_cast<uint8_t>(reload & 0xFFu));
    outb(PIT_CHANNEL2_DATA, static_cast<uint8_t>((reload >> 8) & 0xFFu));

    uint8_t gate = inb(PIT_SPEAKER_PORT);
    gate &= static_cast<uint8_t>(~0x02u); // speaker off
    gate |= 0x01u;                        // gate high
    outb(PIT_SPEAKER_PORT, gate);

    // Retrigger: gate low -> high.
    outb(PIT_SPEAKER_PORT, static_cast<uint8_t>(gate & ~0x01u));
    outb(PIT_SPEAKER_PORT, gate);

    for (uint32_t timeout = 10000000u; timeout != 0; --timeout) {
        if ((inb(PIT_SPEAKER_PORT) & 0x20u) != 0)
            return true;
        asm volatile("pause");
    }
    return false;
}

static void pit_channel2_wait_ticks(uint16_t reload)
{
    (void)pit_channel2_wait_reload(reload);
}

// Waits `us` microseconds on the fixed-rate ACPI PM timer, accumulating the
// 24-bit modular counter correctly across wrap. Returns false when the PM
// timer is missing or does not advance.
[[nodiscard]] bool pm_timer_wait_us(uint64_t us)
{
    const uint32_t pm_blk = acpi_get_pm_timer_block();
    if (pm_blk == 0)
        return false;

    const uint64_t target = (us * PM_TIMER_HZ + 999999ULL) / 1000000ULL;
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

} // namespace

void timer_tsc_calibrate()
{
    if (g_tsc_freq_hz != 0)
        return;

    // 50 ms reference window: long enough that a +/-1 reference tick is a
    // tiny relative error, short enough to stay inside the PIT reload range.
    constexpr uint16_t kRefReload = 59659; // ~50 ms at PIT_BASE_HZ

    const uint64_t t0 = read_tsc();
    if (pit_channel2_wait_reload(kRefReload)) {
        const uint64_t delta = read_tsc() - t0;
        if (delta != 0) {
            g_tsc_freq_hz = (delta * PIT_BASE_HZ) / kRefReload;
            g_tsc_base = read_tsc();
        }
    }

    if (g_tsc_freq_hz == 0) {
        // Modern systems may gate the legacy PIT; the ACPI PM timer is a
        // fixed 3.579545 MHz crystal-backed reference.
        const uint64_t p0 = read_tsc();
        if (pm_timer_wait_us(50000)) {
            const uint64_t delta = read_tsc() - p0;
            if (delta != 0) {
                g_tsc_freq_hz = (delta * 1000000ULL) / 50000ULL;
                g_tsc_base = read_tsc();
            }
        }
    }

    // Reject implausible results (broken/gated references) so consumers fall
    // back to the tick-based clock instead of trusting a wild value.
    if (g_tsc_freq_hz < 1000000ULL || g_tsc_freq_hz > 100000000000ULL) {
        BOOT_WARN("Timer: TSC calibration failed (freq=%llu), using tick clock", g_tsc_freq_hz);
        g_tsc_freq_hz = 0;
        g_tsc_base = 0;
    } else {
        BOOT_LOG("Timer: TSC calibrated at %llu.%03llu MHz", g_tsc_freq_hz / 1000000ULL,
                 (g_tsc_freq_hz / 1000ULL) % 1000ULL);
    }
}

uint64_t timer_tsc_freq_hz()
{
    return g_tsc_freq_hz;
}

// Scaled division that stays inside 64 bits: whole units are taken first so
// the remainder product (remainder < denom) cannot overflow for sane inputs.
uint64_t scale_u64(uint64_t value, uint64_t numer, uint64_t denom)
{
    return (value / denom) * numer + ((value % denom) * numer) / denom;
}

uint64_t timer_now_us()
{
    if (g_tsc_freq_hz != 0) {
        const uint64_t delta = read_tsc() - g_tsc_base;
        return scale_u64(delta, 1000000ULL, g_tsc_freq_hz);
    }

    const uint32_t freq = tick_frequency ? tick_frequency : 1000u;
    return (timer_get_ticks() * 1000000ULL) / freq;
}

void udelay(uint32_t us)
{
    if (us == 0)
        return;

    if (g_tsc_freq_hz != 0) {
        const uint64_t target = scale_u64(us, g_tsc_freq_hz, 1000000ULL);
        const uint64_t start = read_tsc();
        while (read_tsc() - start < target)
            asm volatile("pause");
        return;
    }

    if (pm_timer_wait_us(us))
        return;

    // Last resort: coarse tick clock. Sub-millisecond requests degrade to a
    // compiler barrier; anything longer sleeps on whole ticks.
    const uint32_t freq = tick_frequency ? tick_frequency : 1000u;
    const uint64_t wait_ticks = (static_cast<uint64_t>(us) * freq + 999999ULL) / 1000000ULL;
    if (wait_ticks == 0) {
        asm volatile("" ::: "memory");
        return;
    }
    const uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < wait_ticks)
        asm volatile("pause");
}

uint64_t timer_ms_to_ticks(uint64_t ms)
{
    const uint32_t freq = tick_frequency ? tick_frequency : 1000u;
    const uint64_t whole = ms / 1000u;
    const uint64_t rem = ms % 1000u;
    uint64_t out = whole * static_cast<uint64_t>(freq);
    const uint64_t rem_ticks = (rem * static_cast<uint64_t>(freq) + 999u) / 1000u;
    if (UINT64_MAX - out < rem_ticks)
        return UINT64_MAX;
    return out + rem_ticks;
}

void timer_set_frequency(uint32_t frequency)
{
    if (frequency == 0)
        return;
    tick_frequency = frequency;
}

void timer_init(uint32_t frequency)
{
    if (frequency == 0)
        frequency = 100;
    if (frequency > PIT_BASE_HZ)
        frequency = PIT_BASE_HZ;

    if (apic_is_enabled()) {
        // The LAPIC timer is already programmed during APIC bring-up. Re-enabling
        // the PIT IRQ here would double-fire vector 32 on APIC boots. Preserve
        // the real LAPIC rate published by apic_timer_init(); only fall back to
        // the caller's rate if APIC setup did not publish one.
        if (tick_frequency == 0)
            tick_frequency = frequency;
        return;
    }

    uint32_t divisor = PIT_BASE_HZ / frequency;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFFu)
        divisor = 0xFFFFu;

    tick_frequency = PIT_BASE_HZ / divisor;
    if (tick_frequency == 0)
        tick_frequency = frequency;

    outb(PIT_COMMAND, 0x36);

    outb(PIT_CHANNEL0_DATA, static_cast<uint8_t>(divisor & 0xFFu));
    outb(PIT_CHANNEL0_DATA, static_cast<uint8_t>((divisor >> 8) & 0xFFu));

    pic_clear_mask(0);
}

uint64_t timer_get_ticks()
{
    return ticks;
}

uint32_t timer_get_frequency()
{
    return tick_frequency;
}

uint32_t timer_handler()
{
    // Every core programs its own LAPIC timer, but only the BSP advances the
    // global tick: all sleep/deadline math assumes timer_get_frequency() Hz.
    // Per-core accounting arrives with multi-core scheduling (Phase 4).
    if (cpu_get_local()->cpu_id != 0)
        return 1;
    __sync_add_and_fetch(&ticks, 1);
    return 1;
}

void timer_poll_wait_ms(uint32_t ms)
{
    while (ms != 0) {
        uint32_t chunk_ms = ms > 50u ? 50u : ms;
        uint64_t reload = (static_cast<uint64_t>(PIT_BASE_HZ) * static_cast<uint64_t>(chunk_ms) + 999u) / 1000u;
        if (reload == 0)
            reload = 1;
        if (reload > 0xFFFFu)
            reload = 0xFFFFu;

        pit_channel2_wait_ticks(static_cast<uint16_t>(reload));
        ms -= chunk_ms;
    }
}

void sleep(uint32_t ms)
{
    Process *curr = process_get_current();
    if (curr && curr->pid != 0) {
        scheduler_sleep_ms(ms);
    } else {
        timer_poll_wait_ms(ms);
    }
}
