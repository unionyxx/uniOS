#pragma once
#include <stdint.h>

// TSC-derived monotonic nanoseconds — phase 2 of the scheduler core
// modernization (docs/superpowers/specs/2026-10-05-scheduler-core-modernization-design.md,
// locked decision 2). One cache-friendly read: ordered TSC + per-CPU offset
// + scale.
//
// Tiers, feature-detected once at init (every tier must boot and pass smoke):
//   TscSynced    invariant TSC (CPUID.80000007H:EDX[8]) + calibrated
//                frequency + sync-verified via TSC_ADJUST
//                (CPUID.07H:EBX[1]). Per-CPU offsets are zero by
//                construction on well-behaved firmware; a TSC_ADJUST delta
//                is applied as the correction. A delta beyond tolerance
//                downgrades the system to TscOffsets.
//   TscOffsets   invariant TSC + frequency, but sync not verifiable (no
//                TSC_ADJUST): per-CPU offsets calibrated against the ACPI PM
//                timer — the fixed 3.579545 MHz reference the TSC
//                calibration fallback already uses. (HPET as the reference
//                is the phase-3 upgrade; the kernel has no HPET driver
//                today.)
//   TickFallback no invariant TSC, or TSC calibration failed:
//                timer_get_ticks() * 1e9 / timer_get_frequency(). Honest
//                monotonic time; the HPET-global clock lands in phase 3
//                together with ns-based sleeps.
//
// Ordered reads: RDTSCP (CPUID.80000001H:EDX[27]) when present, else
// LFENCE+RDTSC. The mechanism is selected once at init (function pointer
// dispatch), never per-call CPUID.

enum class TimekeepingTier : uint8_t
{
    TickFallback = 0,
    TscSynced,
    TscOffsets,
};

// One-time setup. Called from the tail of timer_tsc_calibrate()
// (src/kernel/time/timer.cpp) so the existing kmain call site doubles as the
// timekeeping entry point — kmain.cpp needs no edit. Everything
// timer_tsc_calibrate() already requires (CPUID, acpi_init() for the PM
// timer, GS base from cpu_init()) is satisfied by that point in kmain.
// Idempotent; reads made before it runs are tick-derived.
void timekeeping_init();

// Global monotonic time in ns. Never regresses on the same CPU. On the
// TscOffsets tier a task migrating between CPUs can observe a small
// backwards step bounded by the calibration error (single-digit
// microseconds on the PM timer); context-switch read sites must use the
// clamped variant.
uint64_t timekeeping_monotonic_ns();

// max(monotonic_ns(), last_observed_ns) — the migration clamp from locked
// decision 2. The scheduler uses this at switch time (phase 3); the
// per-task last_observed_ns wiring is phase 3, not this commit.
uint64_t timekeeping_monotonic_ns_clamped(uint64_t last_observed_ns);

// True while monotonic_ns() is TSC-driven (TscSynced or TscOffsets), false
// on the tick-derived fallback.
bool timekeeping_is_tsc_source();

// Tier introspection for logs and ktests.
TimekeepingTier timekeeping_tier();
const char *timekeeping_tier_name();

// KNOWN-UNWIRED HOOK: cross-CPU sync verification. SMP bring-up
// (src/kernel/smp/smp.cpp ap_main, after cpu_core_setup() installs the GS
// base) must call this ON the CPU named by cpu_id — TSC and TSC_ADJUST are
// only locally readable, so the snapshot cannot be taken from another CPU,
// and it must run before the CPU executes any timekeeping read. It compares
// this CPU's TSC_ADJUST against the BSP boot snapshot (TscSynced tier:
// verifies; on mismatch the system downgrades to TscOffsets with the delta
// as the correction) or calibrates this CPU's offset against the published
// PM-timer reference (TscOffsets tier). No caller exists yet (smp.cpp is
// intentionally untouched in this landing); until it is wired, only the BSP
// is registered (at init) and the boot-time sync check trivially passes.
// Takes no locks; safe with interrupts enabled or disabled.
void timekeeping_note_cpu_online(uint32_t cpu_id);

// Internal: BSP timer-tick hook. Publishes the {TSC, PM-timer} reference
// pair that note_cpu_online() calibrates against, seqlock-protected with
// the tick handler as the single (IRQ-off) writer. No-op unless the active
// tier is TscOffsets with a PM timer. Called from timer_handler()
// (src/kernel/time/timer.cpp).
void timekeeping_publish_reference();

// Internal/test-only: raw ordered TSC read via the init-selected mechanism
// (RDTSCP or LFENCE+RDTSC). Exposed so ktests can exercise the dispatch;
// everything else should read timekeeping_monotonic_ns().
uint64_t timekeeping_debug_ordered_tsc();
