#include <kernel/cpu.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/mutex.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

// Validates the per-CPU bookkeeping from the BSP's perspective: this core's
// GS-relative block is inside g_cpus[], marked online, and has a live current
// task. Safe on both single-core and multi-core boots.
KTEST(smp_percpu_sanity)
{
    PerCpu *cpu = cpu_get_local();
    KTEST_EXPECT(cpu != nullptr);
    bool found = false;
    for (uint32_t i = 0; i < CONFIG_SMP_MAX_CPUS; i++) {
        if (&g_cpus[i] == cpu) {
            found = true;
            break;
        }
    }
    KTEST_EXPECT(found);
    KTEST_EXPECT(cpu->cpu_id < CONFIG_SMP_MAX_CPUS);
    KTEST_EXPECT(cpu->online);
    KTEST_EXPECT(__atomic_load_n(&g_cpu_online_count, __ATOMIC_ACQUIRE) >= 1);
    KTEST_EXPECT(process_get_current() != nullptr);
}

namespace {

constexpr int STRESS_THREADS = 4;
constexpr int STRESS_ITERS = 200;

static volatile int g_stress_done = 0;
static Mutex g_stress_mutex;
static Spinlock g_stress_lock = SPINLOCK_INIT;

static void stress_thread()
{
    Process *self = process_get_current();
    uint32_t seed = static_cast<uint32_t>(self ? self->pid : 1);

    for (int i = 0; i < STRESS_ITERS; i++) {
        // Heap churn through shared allocator paths.
        void *blob = malloc(64 + (seed % 512));
        if (blob) {
            kstring::memset(blob, static_cast<int>(seed & 0xFF), 64);
            free(blob);
        }

        // Contended short critical section.
        const uint64_t irq_flags = spinlock_acquire_irqsave(&g_stress_lock);
        seed = seed * 1664525u + 1013904223u;
        spinlock_release_irqrestore(&g_stress_lock, irq_flags);

        // Mutex handoff exercises the scheduler wait/wake queues, including
        // priority inheritance on the owning task.
        mutex_lock(&g_stress_mutex);
        seed++;
        mutex_unlock(&g_stress_mutex);

        // Yield so workers interleave across cores when SMP is active.
        if ((i & 15) == 0)
            scheduler_yield();
    }

    __sync_fetch_and_add(&g_stress_done, 1);
}

} // namespace

// Spawns kernel threads that hammer the allocator, a global lock and the
// mutex/wait-queue paths concurrently with the test thread itself. Passes
// when every worker finishes within the tick budget regardless of how many
// CPUs run it.
KTEST(smp_threaded_stress)
{
    g_stress_done = 0;
    mutex_init(&g_stress_mutex);

    Process *workers[STRESS_THREADS] = {};
    int started = 0;
    for (int i = 0; i < STRESS_THREADS; i++) {
        workers[i] = scheduler_create_task(stress_thread, "smp_stress");
        if (workers[i])
            started++;
    }
    KTEST_EXPECT_EQ(started, STRESS_THREADS);

    const uint64_t deadline = timer_get_ticks() + 5000; // generous under TCG
    while (__atomic_load_n(&g_stress_done, __ATOMIC_ACQUIRE) < started && timer_get_ticks() < deadline) {
        scheduler_yield();
    }

    KTEST_EXPECT_EQ(g_stress_done, started);
}
