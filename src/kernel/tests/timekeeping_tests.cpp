#include <kernel/ktest.h>
#include <kernel/time/timekeeping.h>
#include <kernel/time/timer.h>

// Timekeeping ktests (phase 2, locked decision 2). All tier-dependent
// expectations are feature-detected: the suite must pass on the
// tick-fallback tier (qemu64, TCG) exactly as on TSC hardware. Ktests run
// on the BSP after timekeeping_init() (called from the tail of
// timer_tsc_calibrate()) and before smp_init(), so cross-CPU skew is
// exercised only once timekeeping_note_cpu_online() is wired into SMP
// bring-up.

namespace {

bool streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

} // namespace

KTEST(timekeeping_monotonic_non_decreasing)
{
    uint64_t prev = timekeeping_monotonic_ns();
    for (int i = 0; i < 10000; i++) {
        const uint64_t cur = timekeeping_monotonic_ns();
        KTEST_EXPECT(cur >= prev);
        prev = cur;
    }
}

KTEST(timekeeping_reads_advance)
{
    // Back-to-back pairs: never negative, under a coarse ceiling (TCG can
    // stretch a spin, never by seconds), and at least one pair nonzero.
    bool saw_progress = false;
    for (int i = 0; i < 64; i++) {
        const uint64_t a = timekeeping_monotonic_ns();
        for (int spin = 0; spin < 20000; spin++)
            asm volatile("pause");
        const uint64_t b = timekeeping_monotonic_ns();
        KTEST_EXPECT(b >= a);
        KTEST_EXPECT(b - a < 100000000ULL);
        if (b > a)
            saw_progress = true;
    }
    KTEST_EXPECT(saw_progress);

    // A deliberate 2 ms gap must yield a plausible nonzero delta on the TSC
    // tiers and on tick frequencies >= 500 Hz (the kernel boots at 1 kHz).
    const uint64_t a = timekeeping_monotonic_ns();
    udelay(2000);
    const uint64_t b = timekeeping_monotonic_ns();
    KTEST_EXPECT(b >= a);
    if (timekeeping_is_tsc_source() || timer_get_frequency() >= 500u)
        KTEST_EXPECT(b > a);
    KTEST_EXPECT(b - a < 500000000ULL);
}

KTEST(timekeeping_clamp_semantics)
{
    const uint64_t now = timekeeping_monotonic_ns();

    // Future observation: the clamp returns the passed value untouched.
    const uint64_t future = now + 1000000000ULL;
    KTEST_EXPECT_EQ(timekeeping_monotonic_ns_clamped(future), future);

    // Past observation: resolves to a live read, never below the current time.
    const uint64_t fresh = timekeeping_monotonic_ns();
    KTEST_EXPECT(timekeeping_monotonic_ns_clamped(now / 2) >= fresh);

    // Current observation: a live read, so at least the sample it follows.
    KTEST_EXPECT(timekeeping_monotonic_ns_clamped(now) >= now);
}

KTEST(timekeeping_tier_report)
{
    const TimekeepingTier tier = timekeeping_tier();
    const char *name = timekeeping_tier_name();
    KTEST_EXPECT(name != nullptr);
    KTEST_EXPECT(name[0] != '\0');

    switch (tier) {
        case TimekeepingTier::TscSynced:
            KTEST_EXPECT(streq(name, "tsc-synced"));
            break;
        case TimekeepingTier::TscOffsets:
            KTEST_EXPECT(streq(name, "tsc-offsets"));
            break;
        case TimekeepingTier::TickFallback:
            KTEST_EXPECT(streq(name, "tick-fallback"));
            break;
    }

    const bool is_tsc = timekeeping_is_tsc_source();
    KTEST_EXPECT(is_tsc == (tier != TimekeepingTier::TickFallback));
    if (is_tsc)
        KTEST_EXPECT(timer_tsc_freq_hz() != 0);

    DEBUG_INFO("ktest timekeeping: tier=%s tsc_source=%d tsc_freq_hz=%llu", name, is_tsc ? 1 : 0, timer_tsc_freq_hz());
}

KTEST(timekeeping_ordered_read_sane)
{
    // Exercise the RDTSCP/LFENCE+RDTSC dispatch: an ordered read is nonzero
    // and cannot go backwards.
    const uint64_t r0 = timekeeping_debug_ordered_tsc();
    for (int spin = 0; spin < 2000; spin++)
        asm volatile("pause");
    const uint64_t r1 = timekeeping_debug_ordered_tsc();
    KTEST_EXPECT(r0 != 0);
    KTEST_EXPECT(r1 >= r0);

    // On TSC tiers the ordered read is the same counter monotonic_ns()
    // scales: a raw TSC delta between sandwiched monotonic reads must map
    // to a ns delta of the same magnitude. Loose bounds only — TCG timing
    // makes exact ratios flaky.
    if (!timekeeping_is_tsc_source())
        return;
    const uint64_t freq = timer_tsc_freq_hz();
    KTEST_EXPECT(freq != 0);

    const uint64_t a = timekeeping_monotonic_ns();
    const uint64_t ra = timekeeping_debug_ordered_tsc();
    for (int spin = 0; spin < 2000; spin++)
        asm volatile("pause");
    const uint64_t rb = timekeeping_debug_ordered_tsc();
    const uint64_t b = timekeeping_monotonic_ns();
    if (rb > ra && b > a) {
        const uint64_t tsc_ns = scale_u64(rb - ra, 1000000000ULL, freq);
        KTEST_EXPECT(b - a <= tsc_ns + 1000000ULL);
        KTEST_EXPECT(tsc_ns <= (b - a) * 10ULL + 1000000ULL);
    }
}
