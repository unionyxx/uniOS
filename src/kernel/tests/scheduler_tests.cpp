#include <kernel/ktest.h>
#include <kernel/event.h>
#include <kernel/process.h>
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

// Regression: SYS_POST_EVENT pushes an event while holding g_sched_lock.
// event_push() re-enters g_sched_lock via scheduler_notify_input_waiters()
// and self-deadlocks, silently freezing the system the first time the WM
// posts a mouse event (cursor crossing a window border). Mirror the syscall
// sequence with the notify-free enqueue; any scheduler re-entry added back
// into this sequence hangs the boot and fails the smoke suite.
KTEST(sched_event_post_sequence_under_big_lock_does_not_deadlock)
{
    EventQueue q;
    event_init(q);
    WaitQueue waiters = {};

    Event ev = {};
    ev.type = EVT_MOUSE_MOVE;

    const uint64_t flags = scheduler_big_lock_irqsave();
    const bool enqueued = event_enqueue(q, ev);
    scheduler_wake_all_locked(&waiters);
    scheduler_big_unlock_irqrestore(flags);
    KTEST_EXPECT(enqueued);

    Event out = {};
    KTEST_EXPECT(event_poll(q, out));
    KTEST_EXPECT_EQ(out.type, EVT_MOUSE_MOVE);
    KTEST_EXPECT(!event_poll(q, out));
}
