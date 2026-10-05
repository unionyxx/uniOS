#include <kernel/event.h>
#include <kernel/ktest.h>
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

namespace {

// Timed-wait registry ktests drive the real registration/park/walker path
// through real kernel tasks: each worker registers a deadline on itself,
// parks on a test wait queue, and reports whether the deadline walker
// released it — the walker sets Proc::timed_wake only for non-epoll
// entries, exactly like a futex timeout, so timed_wake distinguishes a
// genuine timeout wake from any other release.
constexpr int TIMED_WAIT_SCALE_WORKERS = 64;
constexpr uint64_t TIMED_WAIT_DEADLINE_TICKS = 200;
constexpr uint64_t TIMED_WAIT_MARGIN_TICKS = 5000; // generous under TCG
constexpr uint64_t RACE_DEADLINE_TICKS = 4;
// Covers only wake-to-resumption scheduling latency; a lost timeout would
// exceed any bound.
constexpr uint64_t RACE_WAKE_SLACK_TICKS = 100;
constexpr uint64_t CLEAR_WINDOW_TICKS = 300;

WaitQueue g_timed_wait_scale_queue = {nullptr, nullptr};
WaitQueue g_timed_wait_race_queue = {nullptr, nullptr};
WaitQueue g_timed_wait_clear_queue = {nullptr, nullptr};

volatile int g_scale_registered = 0;
volatile int g_scale_timed_out = 0;
volatile int g_scale_early_wake = 0;

volatile int g_race_registered = 0;
volatile int g_race_woken = 0;
volatile uint64_t g_race_park_tick = 0;
volatile uint64_t g_race_wake_tick = 0;

volatile int g_clear_worker_done = 0;
volatile int g_clear_timed_wake = 0;

static void timed_wait_scale_worker()
{
    Process *self = process_get_current();
    const uint64_t deadline = timer_get_ticks() + TIMED_WAIT_DEADLINE_TICKS;
    if (scheduler_note_wake_deadline(self, deadline)) {
        __sync_fetch_and_add(&g_scale_registered, 1);
        scheduler_wait(&g_timed_wait_scale_queue, nullptr);
        // No-op when the walker fired (it frees the entry itself); covers
        // a release by any other path.
        scheduler_clear_wake_deadline(self);
        if (self->timed_wake) {
            self->timed_wake = false;
            if (timer_get_ticks() < deadline)
                __sync_fetch_and_add(&g_scale_early_wake, 1);
            __sync_fetch_and_add(&g_scale_timed_out, 1);
        }
    }
}

static void timed_wait_race_worker()
{
    Process *self = process_get_current();
    const uint64_t deadline = timer_get_ticks() + RACE_DEADLINE_TICKS;
    if (!scheduler_note_wake_deadline(self, deadline))
        return;
    __sync_fetch_and_add(&g_race_registered, 1);
    // Burn ticks past the deadline while NOT parked: every tick in this
    // window runs the walker, which must keep the entry (a waiter between
    // its arm and its queue push) instead of dropping it.
    while (timer_get_ticks() <= deadline)
        scheduler_yield();
    __atomic_store_n(&g_race_park_tick, timer_get_ticks(), __ATOMIC_RELEASE);
    scheduler_wait(&g_timed_wait_race_queue, nullptr);
    __atomic_store_n(&g_race_wake_tick, timer_get_ticks(), __ATOMIC_RELEASE);
    if (self->timed_wake) {
        self->timed_wake = false;
        __sync_fetch_and_add(&g_race_woken, 1);
    }
}

static void timed_wait_clear_worker()
{
    Process *self = process_get_current();
    const uint64_t deadline = timer_get_ticks() + RACE_DEADLINE_TICKS;
    (void)scheduler_note_wake_deadline(self, deadline);
    // The wait that armed the entry never happened: the clear must remove
    // it, so the walker has nothing to fire when the deadline passes.
    scheduler_clear_wake_deadline(self);
    scheduler_wait(&g_timed_wait_clear_queue, nullptr);
    if (self->timed_wake) {
        self->timed_wake = false;
        __sync_fetch_and_add(&g_clear_timed_wake, 1);
    }
    __sync_fetch_and_add(&g_clear_worker_done, 1);
}

} // namespace

// P10 regression: more than 16 concurrent timed waits. The old fixed
// 16-entry table refused the 17th registration (sys_futex returned
// -ENOSPC); the dynamic registry must place all 64 — exercising the
// 70%-load bucket growth — and wake every one at its deadline, none early.
KTEST(sched_timed_wait_registry_holds_64_concurrent)
{
    g_scale_registered = 0;
    g_scale_timed_out = 0;
    g_scale_early_wake = 0;

    int started = 0;
    for (int i = 0; i < TIMED_WAIT_SCALE_WORKERS; i++) {
        if (scheduler_create_task(timed_wait_scale_worker, "tw_scale"))
            started++;
    }
    KTEST_EXPECT_EQ(started, TIMED_WAIT_SCALE_WORKERS);

    const uint64_t wait_until = timer_get_ticks() + TIMED_WAIT_DEADLINE_TICKS + TIMED_WAIT_MARGIN_TICKS;
    while (__atomic_load_n(&g_scale_timed_out, __ATOMIC_ACQUIRE) < started && timer_get_ticks() < wait_until)
        scheduler_yield();

    KTEST_EXPECT_EQ(g_scale_registered, started);
    KTEST_EXPECT_EQ(g_scale_timed_out, started);
    KTEST_EXPECT_EQ(g_scale_early_wake, 0);

    // Settle: reaps the worker zombies before the next ktest.
    scheduler_sleep(100);
    scheduler_yield();
}

// C6 regression: arm -> deadline expires -> queue. The old global
// epoll-deadline mechanism cleared the deadline unconditionally at expiry,
// so a waiter that had armed but not yet parked lost its timeout and hung
// past it. The registry keeps unparked entries and re-checks them every
// tick, so parking after expiry still wakes on the next walker pass.
KTEST(sched_timed_wait_arm_before_queue_race)
{
    g_race_registered = 0;
    g_race_woken = 0;

    KTEST_EXPECT(scheduler_create_task(timed_wait_race_worker, "tw_race") != nullptr);

    const uint64_t wait_until = timer_get_ticks() + RACE_DEADLINE_TICKS + TIMED_WAIT_MARGIN_TICKS;
    while (__atomic_load_n(&g_race_woken, __ATOMIC_ACQUIRE) == 0 && timer_get_ticks() < wait_until)
        scheduler_yield();

    KTEST_EXPECT_EQ(g_race_registered, 1);
    KTEST_EXPECT_EQ(g_race_woken, 1);
    const uint64_t park_tick = __atomic_load_n(&g_race_park_tick, __ATOMIC_ACQUIRE);
    const uint64_t wake_tick = __atomic_load_n(&g_race_wake_tick, __ATOMIC_ACQUIRE);
    KTEST_EXPECT(wake_tick >= park_tick);
    KTEST_EXPECT(wake_tick - park_tick <= RACE_WAKE_SLACK_TICKS);

    scheduler_sleep(100);
    scheduler_yield();
}

// Registrations are waiter-owned and must die with their wait: after a
// clear, ticking past the deadline leaves the waiter parked (no wake, no
// walker burn), and a later explicit wake must not report a timeout.
KTEST(sched_timed_wait_clear_prevents_wake)
{
    g_clear_worker_done = 0;
    g_clear_timed_wake = 0;

    Process *worker = scheduler_create_task(timed_wait_clear_worker, "tw_clear");
    KTEST_EXPECT(worker != nullptr);

    // Run well past the dropped deadline.
    const uint64_t until = timer_get_ticks() + CLEAR_WINDOW_TICKS;
    while (timer_get_ticks() < until)
        scheduler_yield();

    KTEST_EXPECT_EQ(__atomic_load_n(&g_clear_worker_done, __ATOMIC_ACQUIRE), 0);
    KTEST_EXPECT(worker->state == ProcessState_Waiting);
    KTEST_EXPECT_EQ(g_clear_timed_wake, 0);

    // Release the worker through the normal wake path.
    scheduler_wake_all(&g_timed_wait_clear_queue);
    const uint64_t wait_until = timer_get_ticks() + TIMED_WAIT_MARGIN_TICKS;
    while (__atomic_load_n(&g_clear_worker_done, __ATOMIC_ACQUIRE) == 0 && timer_get_ticks() < wait_until)
        scheduler_yield();

    KTEST_EXPECT_EQ(g_clear_worker_done, 1);
    KTEST_EXPECT_EQ(g_clear_timed_wake, 0);

    scheduler_sleep(100);
    scheduler_yield();
}
