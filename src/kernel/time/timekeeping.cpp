#include <drivers/acpi/acpi.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/time/timekeeping.h>
#include <kernel/time/timer.h>

// TSC-derived monotonic nanoseconds per locked decision 2. Tier selection
// happens once here (BSP, pre-SMP, feature-detected); the tier table and
// calling contracts live in include/kernel/time/timekeeping.h.

namespace {

constexpr uint32_t MSR_IA32_TSC_ADJUST = 0x3B;

// TSC_ADJUST deltas up to this size are corrected in place and the tier
// stays TscSynced; larger deltas mean the cores are demonstrably not in
// lockstep and the system downgrades to TscOffsets. 1 ms in TSC cycles.
constexpr uint64_t kSyncToleranceMs = 1;

// PM-timer differences are masked to 24 bits: exact for 24-bit counters and
// for 32-bit ones (2^24 divides 2^32, so deltas below the wrap stay exact).
constexpr uint32_t kPmTimerMask = 0xFFFFFFu;

// References publish at the tick rate (1 kHz); a difference beyond ~2 s of
// PM-timer ticks means publications stalled past a safe window — reject
// instead of calibrating against a wrapped value.
constexpr uint32_t kPmtMaxDelta = 7159090u;

constexpr int kSeqlockReadAttempts = 8;

[[gnu::target("no-sse")]] inline void cpuid_leaf(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx,
                                                 uint32_t &ecx, uint32_t &edx)
{
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}

[[gnu::target("no-sse")]] inline uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// RDTSCP retires only after all prior instructions, so it is ordered by
// itself; the aux (rcx) output must be declared even though unused.
[[gnu::target("no-sse")]] uint64_t read_tsc_rdtscp()
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t aux = 0;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux)::"memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

[[gnu::target("no-sse")]] uint64_t read_tsc_lfence_rdtsc()
{
    asm volatile("lfence" ::: "memory");
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

using OrderedTscReader = uint64_t (*)();
OrderedTscReader g_read_ordered = read_tsc_lfence_rdtsc;

// Tier stored as the enum's underlying integer: clang's __atomic builtins
// reject enum pointers. RELAXED reads on the hot path; a RELEASE write
// makes mid-boot downgrades (timekeeping_note_cpu_online) visible to every
// CPU.
uint8_t g_tier_raw = static_cast<uint8_t>(TimekeepingTier::TickFallback);
bool g_inited = false;
bool g_has_tsc_adjust = false;

uint64_t g_tsc_freq = 0;
uint64_t g_tsc_base = 0;
uint64_t g_bsp_tsc_adjust = 0;

// Per-CPU correction in TSC cycles, indexed identically to g_cpus[] (index 0
// = BSP). Written only by the owning CPU (BSP at init, APs at
// timekeeping_note_cpu_online) and read by the same CPU in
// monotonic_ns(): x86-64 naturally-aligned u64 accesses are atomic and
// same-CPU program order suffices — no lock, no cross-CPU fencing on the
// read path.
uint64_t g_tsc_offset[CONFIG_SMP_MAX_CPUS] = {};
bool g_cpu_noted[CONFIG_SMP_MAX_CPUS] = {};

// PM-timer cross-CPU reference (TscOffsets tier only). Single writer: the
// BSP timer handler with IRQs off. Seqlock discipline: seq odd = write in
// progress, even = stable; seq 0 = never published. Readers retry.
uint64_t g_ref_seq = 0;
uint64_t g_ref_tsc = 0;
uint32_t g_ref_pmt = 0;
uint32_t g_pm_block = 0;

inline TimekeepingTier current_tier()
{
    return static_cast<TimekeepingTier>(__atomic_load_n(&g_tier_raw, __ATOMIC_RELAXED));
}

inline void set_tier(TimekeepingTier tier)
{
    __atomic_store_n(&g_tier_raw, static_cast<uint8_t>(tier), __ATOMIC_RELEASE);
}

inline uint32_t pm_timer_read_raw()
{
    return inl(static_cast<uint16_t>(g_pm_block));
}

struct PmtTscPair
{
    uint64_t tsc;
    uint32_t pmt;
};

// Reads (pmt, ordered TSC, pmt) and timestamps the TSC at the interval
// midpoint: the sandwich width bounds the pairing error on both sides.
PmtTscPair pm_timer_sandwich_tsc()
{
    const uint32_t a = pm_timer_read_raw();
    const uint64_t tsc = g_read_ordered();
    const uint32_t b = pm_timer_read_raw();
    return PmtTscPair{tsc, a + (((b - a) & kPmTimerMask) >> 1)};
}

// AP-side offset calibration against the last published BSP pair:
//   offset = (ref_tsc + elapsed_pm_ticks * tsc_freq / PM_HZ) - local_tsc
// Error budget: two sandwich widths plus publication staleness (<= one
// tick) — single-digit microseconds on the PM timer, matching the
// documented PM-timer skew bound.
bool calibrate_offset_via_pm(uint32_t cpu_id)
{
    if (g_pm_block == 0)
        return false;

    for (int attempt = 0; attempt < kSeqlockReadAttempts; attempt++) {
        const uint64_t seq1 = __atomic_load_n(&g_ref_seq, __ATOMIC_ACQUIRE);
        if (seq1 == 0 || (seq1 & 1ULL) != 0)
            continue;
        const uint64_t ref_tsc = __atomic_load_n(&g_ref_tsc, __ATOMIC_RELAXED);
        const uint32_t ref_pmt = __atomic_load_n(&g_ref_pmt, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_ref_seq, __ATOMIC_ACQUIRE) != seq1)
            continue;

        const PmtTscPair local = pm_timer_sandwich_tsc();
        const uint32_t d_pmt = (local.pmt - ref_pmt) & kPmTimerMask;
        if (d_pmt > kPmtMaxDelta)
            break;

        const uint64_t expected = ref_tsc + scale_u64(d_pmt, g_tsc_freq, TIMER_PM_TIMER_HZ);
        const int64_t offset = static_cast<int64_t>(expected - local.tsc);
        __atomic_store_n(&g_tsc_offset[cpu_id], static_cast<uint64_t>(offset), __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

} // namespace

void timekeeping_init()
{
    if (g_inited)
        return;
    g_inited = true;

    g_pm_block = acpi_get_pm_timer_block();

    // One CPUID sweep on the BSP; the flags are per-part, not per-core.
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid_leaf(0x80000000U, 0, eax, ebx, ecx, edx);
    const uint32_t max_ext = eax;
    bool invariant_tsc = false;
    bool has_rdtscp = false;
    if (max_ext >= 0x80000001U) {
        cpuid_leaf(0x80000001U, 0, eax, ebx, ecx, edx);
        has_rdtscp = (edx & (1U << 27)) != 0; // RDTSCP
    }
    if (max_ext >= 0x80000007U) {
        cpuid_leaf(0x80000007U, 0, eax, ebx, ecx, edx);
        invariant_tsc = (edx & (1U << 8)) != 0; // Invariant TSC
    }
    cpuid_leaf(0, 0, eax, ebx, ecx, edx);
    if (eax >= 7) {
        cpuid_leaf(7, 0, eax, ebx, ecx, edx);
        g_has_tsc_adjust = (ebx & (1U << 1)) != 0; // IA32_TSC_ADJUST
    }

    g_read_ordered = has_rdtscp ? read_tsc_rdtscp : read_tsc_lfence_rdtsc;

    g_tsc_freq = timer_tsc_freq_hz();
    if (g_tsc_freq == 0) {
        BOOT_WARN("Timekeeping: TSC frequency uncalibrated; tick-derived fallback tier");
        set_tier(TimekeepingTier::TickFallback);
        return;
    }
    if (!invariant_tsc) {
        // Decision 2 tier (c): the TSC is not the clock. Tick-derived ns is
        // monotonic and correct for this API; the HPET-global upgrade lands
        // with phase-3 ns-based sleeps.
        BOOT_LOG("Timekeeping: no invariant TSC; tick-derived fallback tier");
        set_tier(TimekeepingTier::TickFallback);
        return;
    }

    g_tsc_base = g_read_ordered();
    if (g_has_tsc_adjust) {
        g_bsp_tsc_adjust = rdmsr64(MSR_IA32_TSC_ADJUST);
        set_tier(TimekeepingTier::TscSynced);
    } else if (g_pm_block != 0) {
        set_tier(TimekeepingTier::TscOffsets);
    } else {
        // Invariant TSC, but neither verification (TSC_ADJUST) nor a
        // calibration reference (PM timer) exists: run as TscSynced with a
        // loud assumption instead of pretending verification happened.
        set_tier(TimekeepingTier::TscSynced);
        BOOT_WARN(
            "Timekeeping: TSC sync unverifiable (no TSC_ADJUST, no PM timer); assuming firmware-synchronized TSC");
    }

    // Register the BSP: it is the reference, so the sync check trivially
    // passes and its offset stays 0. The PM-timer reference itself is first
    // published by the next timer tick.
    timekeeping_note_cpu_online(cpu_get_local()->cpu_id);

    BOOT_LOG("Timekeeping: tier=%s rdtscp=%d tsc=%llu.%03llu MHz pm_timer=%u", timekeeping_tier_name(),
             has_rdtscp ? 1 : 0, g_tsc_freq / 1000000ULL, (g_tsc_freq / 1000ULL) % 1000ULL, g_pm_block);
}

void timekeeping_note_cpu_online(uint32_t cpu_id)
{
    if (cpu_id >= CONFIG_SMP_MAX_CPUS || g_cpu_noted[cpu_id])
        return;

    const TimekeepingTier tier = current_tier();

    if (tier == TimekeepingTier::TickFallback) {
        g_cpu_noted[cpu_id] = true;
        return;
    }

    if (tier == TimekeepingTier::TscSynced) {
        if (g_has_tsc_adjust) {
            // IA32_TSC_ADJUST records exactly how much software/firmware
            // offset this CPU's TSC against its siblings: the delta IS the
            // correction, and equality is the sync check.
            const int64_t correction = static_cast<int64_t>(g_bsp_tsc_adjust - rdmsr64(MSR_IA32_TSC_ADJUST));
            const uint64_t mag =
                correction < 0 ? 0ULL - static_cast<uint64_t>(correction) : static_cast<uint64_t>(correction);
            const uint64_t tolerance = (g_tsc_freq / 1000ULL) * kSyncToleranceMs;
            if (mag > tolerance) {
                BOOT_WARN("Timekeeping: CPU%u TSC_ADJUST skew %s%llu cycles beyond tolerance; per-CPU-offset tier",
                          cpu_id, correction < 0 ? "-" : "", mag);
                set_tier(TimekeepingTier::TscOffsets);
            } else if (correction != 0) {
                DEBUG_TRACE("Timekeeping: CPU%u TSC_ADJUST correction %s%llu cycles", cpu_id, correction < 0 ? "-" : "",
                            mag);
            }
            __atomic_store_n(&g_tsc_offset[cpu_id], static_cast<uint64_t>(correction), __ATOMIC_RELEASE);
        }
        g_cpu_noted[cpu_id] = true;
        return;
    }

    // TscOffsets: the BSP owns the reference timeline (offset 0 by
    // definition); APs calibrate against the published PM-timer pair.
    if (cpu_id != 0 && !calibrate_offset_via_pm(cpu_id))
        BOOT_WARN("Timekeeping: CPU%u offset calibration failed (no fresh PM-timer reference); keeping offset 0",
                  cpu_id);
    g_cpu_noted[cpu_id] = true;
}

void timekeeping_publish_reference()
{
    if (current_tier() != TimekeepingTier::TscOffsets || g_pm_block == 0)
        return;

    // Single writer (BSP timer handler, IRQs off) — plain seqlock publish.
    const PmtTscPair ref = pm_timer_sandwich_tsc();
    const uint64_t seq = __atomic_load_n(&g_ref_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ref_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ref_tsc, ref.tsc, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ref_pmt, ref.pmt, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ref_seq, seq + 2, __ATOMIC_RELEASE);
}

uint64_t timekeeping_monotonic_ns()
{
    if (current_tier() == TimekeepingTier::TickFallback) {
        const uint32_t freq = timer_get_frequency();
        if (freq == 0)
            return 0;
        return scale_u64(timer_get_ticks(), 1000000000ULL, static_cast<uint64_t>(freq));
    }

    const uint32_t cpu_id = cpu_get_local()->cpu_id;
    const uint64_t tsc = g_read_ordered();
    const uint64_t offset = __atomic_load_n(&g_tsc_offset[cpu_id], __ATOMIC_RELAXED);
    return scale_u64(tsc - g_tsc_base + offset, 1000000000ULL, g_tsc_freq);
}

uint64_t timekeeping_monotonic_ns_clamped(uint64_t last_observed_ns)
{
    const uint64_t now = timekeeping_monotonic_ns();
    return now > last_observed_ns ? now : last_observed_ns;
}

bool timekeeping_is_tsc_source()
{
    return current_tier() != TimekeepingTier::TickFallback;
}

TimekeepingTier timekeeping_tier()
{
    return current_tier();
}

const char *timekeeping_tier_name()
{
    switch (current_tier()) {
        case TimekeepingTier::TscSynced:
            return "tsc-synced";
        case TimekeepingTier::TscOffsets:
            return "tsc-offsets";
        case TimekeepingTier::TickFallback:
            break;
    }
    return "tick-fallback";
}

uint64_t timekeeping_debug_ordered_tsc()
{
    return g_read_ordered();
}
