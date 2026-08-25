#include <kernel/ktest.h>
#include <kernel/scheduler.h>
#include <kernel/time/timer.h>

KTEST(sched_ready_queue_state_guards)
{
    KTEST_EXPECT(scheduler_ready_queue_self_test());
}

namespace {

constexpr int ONCE_WORKERS = 8;

static volatile int g_once_runs = 0;

static void run_once_worker()
{
    __sync_fetch_and_add(&g_once_runs, 1);
}

} // namespace

// Regression: a process must execute exactly once per enqueue. The
// in_ready_queue/on_cpu guards prevent double-linked ready queues, which
// previously let one task be scheduled twice (double run) or stranded.
KTEST(sched_enqueue_runs_task_exactly_once)
{
    g_once_runs = 0;

    int started = 0;
    for (int i = 0; i < ONCE_WORKERS; i++) {
        Process *worker = scheduler_create_task(run_once_worker, "sched_once");
        if (worker)
            started++;
    }
    KTEST_EXPECT_EQ(started, ONCE_WORKERS);

    const uint64_t deadline = timer_get_ticks() + 5000; // generous under TCG
    while (__atomic_load_n(&g_once_runs, __ATOMIC_ACQUIRE) < started && timer_get_ticks() < deadline)
        scheduler_yield();
    KTEST_EXPECT_EQ(g_once_runs, started);

    // Settle: a double-enqueued worker would surface as an extra run.
    scheduler_sleep(100);
    scheduler_yield();
    KTEST_EXPECT_EQ(g_once_runs, started);
}
