#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/irq.h>
#include <kernel/scheduler.h>
#include <kernel/time/timer.h>

static volatile uint64_t ticks = 0;
static uint32_t tick_frequency = 0;

namespace {

constexpr uint16_t PIT_CHANNEL2_DATA = 0x42;
constexpr uint16_t PIT_SPEAKER_PORT = 0x61;
constexpr uint32_t PIT_BASE_HZ = 1193182u;

static void pit_channel2_wait_ticks(uint16_t reload)
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
            return;
        asm volatile("pause");
    }
}

} // namespace

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

void timer_handler()
{
    ticks++;
    // Explicit wake paths handle events/display/process waits now. Keep the
    // IRQ path lightweight instead of waking every waiting task every tick.
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
    timer_poll_wait_ms(ms);
}
