#include <kernel/ktest.h>
#include <kernel/time/timer.h>

KTEST(timer_tsc_freq_is_plausible_or_uncalibrated)
{
    const uint64_t freq = timer_tsc_freq_hz();
    if (freq == 0)
        return;
    KTEST_EXPECT(freq >= 1000000ULL);
    KTEST_EXPECT(freq <= 100000000000ULL);
}

KTEST(timer_now_us_is_monotonic)
{
    const uint64_t a = timer_now_us();
    udelay(2000);
    const uint64_t b = timer_now_us();
    KTEST_EXPECT(b >= a);
    // A 2 ms delay must measure at least half its nominal length even on a
    // slow/virtualized reference, but never stall for hundreds of ms.
    KTEST_EXPECT(b - a >= 1000ULL);
    KTEST_EXPECT(b - a < 500000ULL);
}

KTEST(timer_udelay_small_values_return)
{
    const uint64_t start_ticks = timer_get_ticks();
    for (int i = 0; i < 8; i++)
        udelay(50);
    const uint64_t elapsed_ticks = timer_get_ticks() - start_ticks;
    // 8x50 us is under one tick at 1 kHz; allow generous slack for slow TCG.
    KTEST_EXPECT(elapsed_ticks < 500ULL);
}

KTEST(timer_ms_to_ticks_matches_frequency)
{
    const uint32_t freq = timer_get_frequency();
    KTEST_EXPECT(freq != 0);
    KTEST_EXPECT_EQ(timer_ms_to_ticks(0), 0ULL);
    KTEST_EXPECT_EQ(timer_ms_to_ticks(1000), static_cast<uint64_t>(freq));
    // 1 ms rounds up to at least one tick.
    KTEST_EXPECT(timer_ms_to_ticks(1) >= 1ULL);
}
