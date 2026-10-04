#pragma once
#include <stdint.h>

// Locking model (SMP):
//
// g_sched_lock is the scheduler big lock. It guards the run queues, sleep
// queue, wait queues, and the process list. Every path that touches those
// structures must hold it; it is always acquired with local IRQs disabled
// (interrupts_save_disable() + spinlock_acquire) because the timer IRQ can
// fire schedule() on any core.
//
// Context switches release g_sched_lock on the TARGET task's behalf via
// scheduler_unlock_after_switch(), so a freshly switched-to thread never runs
// while the switcher still holds the lock.
//
// Lock ordering: g_sched_lock is outermost. While holding it, do NOT acquire
// locks that could also be held when someone else acquires g_sched_lock (e.g.
// never take heap/pmm/vma/fd locks inside a g_sched_lock critical section and
// expect them to nest back). Leaf locks (pmm, heap, vma, fd, pipe, futex,
// epoll instance) must never call into the scheduler while held except via
// scheduler_wait(q, &leaf_lock), which releases the leaf before switching.
struct Process;

void scheduler_init();
[[nodiscard]] Process *scheduler_create_task(void (*entry)(), const char *name);
// Deferred variant: task is built but NOT queued; fill setup fields, then
// publish with scheduler_enqueue_task() (safe against cross-core pickup).
[[nodiscard]] Process *scheduler_create_task_deferred(void (*entry)(), const char *name);
void scheduler_enqueue_task(Process *proc);
// Per-core idle context: pid 0, never queued; schedule() parks on it.
[[nodiscard]] Process *scheduler_create_idle_task(void (*entry)(), const char *name);
void scheduler_schedule();
void scheduler_yield();
void scheduler_notify_input_waiters();
void scheduler_wake_process(Process *p);

// Locked variants for find-and-act sequences (signal delivery, event posts):
// the scheduler big lock must be held across both so the target cannot be
// reaped between the lookup and the use.
struct Process *process_find_by_pid_locked(uint64_t pid);
void scheduler_wake_for_signal_locked(Process *p);

// Direct access to the scheduler big lock for those find-and-act sequences.
// Interrupts are disabled while held. Leaf locks (the shared fd table lock, pipe, epoll) may
// be taken underneath; the reverse order is forbidden.
uint64_t scheduler_big_lock_irqsave();
void scheduler_big_unlock_irqrestore(uint64_t flags);

// True when a pending signal would take its default-fatal action; blocking
// loops use this to bail out with -EINTR instead of sleeping through kill.
bool scheduler_fatal_signal_pending(const Process *p);

// Exec support: sever the dead thread group's references to the address
// space the exec'ing member just swapped away from, so their later frees
// cannot re-free it. The caller holds g_sched_lock across both the swap
// and this call; see scheduler.cpp for the full ordering contract.
void scheduler_sever_dead_group_references(uint64_t *old_pml4);

[[nodiscard]] Process *scheduler_get_process_list();

void scheduler_schedule_elapsed(uint32_t elapsed_jiffies);
void scheduler_sleep(uint64_t ticks);
void scheduler_sleep_ms(uint64_t ms);

struct WaitQueue;
struct Spinlock;
void scheduler_wait(WaitQueue *q, Spinlock *lock);

// Sleep queue deadline for timed epoll waits. sys_epoll_wait parks on
// g_epoll_wait_queue, which has no native timeout; the tick path fires the
// deadline and wakes the queue.
void scheduler_note_epoll_deadline(uint64_t deadline_ticks);

// Queued sleep with a lost-wakeup guard. The task is pushed onto the wait
// queue under g_sched_lock first; `recheck` then runs while still holding
// g_sched_lock. If it returns true, the task never sleeps: the condition
// became true after the caller's last check and any producer that set it
// already ran its wake (serialized by g_sched_lock) against an empty queue.
// The recheck must only read locklessly-atomic state (aligned word loads,
// no leaf locks): its verdict is a hint — false negatives are recovered by
// the producer's later wake finding this task queued, false positives just
// re-run the caller's scan loop.
typedef bool (*scheduler_wait_recheck_fn)(void *ctx);
void scheduler_wait_rechecked(WaitQueue *q, Spinlock *lock, scheduler_wait_recheck_fn recheck, void *ctx);

// Wake up to `count` waiters from q (count == 0 wakes all), taking g_sched_lock
// around the whole traversal so signal-driven queue removals cannot interleave.
// For callers that hold the queue's own leaf lock (futex buckets); the lock
// order is leaf -> scheduler, matching scheduler_wait. When `match` is given,
// only queued waiters it accepts count towards `count` (the walk still skips
// the rest without waking them) — the futex WAKE path uses this to spend its
// wake count on the addressed word only.
typedef bool (*scheduler_wait_match_fn)(const struct Process *p, void *ctx);
int scheduler_wake_waiters_under_leaf(WaitQueue *q, uint32_t count, scheduler_wait_match_fn match = nullptr,
                                      void *ctx = nullptr);
void scheduler_wake_all(WaitQueue *q);
void scheduler_wake_all_locked(WaitQueue *q);
void scheduler_wake_one(WaitQueue *q);

struct SyscallFrame;
[[nodiscard]] int64_t sys_thread_create(void (*entry)(), void *arg, void *stack_top, struct SyscallFrame *frame,
                                        uint64_t stack_lo = 0, uint64_t stack_size = 0, uint64_t flags = 0);
// Deadline machinery for timed waits on leaf wait queues (futex timeouts):
// register the earliest wake deadline; the scheduler walker wakes waiters
// still parked on their queue and marks them timed_wake. Returns false
// (and arms nothing) when the fixed timed-wait table is full — the caller
// must fail the wait rather than degrade it to an infinite sleep.
bool scheduler_note_wake_deadline(struct Process *p, uint64_t deadline_ticks);
// Drop every timed-wait registration for `p`: the wait that armed it has
// ended (wake, signal, expiry or thread exit). Registrations must never
// outlive their wait, or the walker marks an unrelated later wait of the
// same (or a recycled) Process as timed out.
void scheduler_clear_wake_deadline(struct Process *p);
void scheduler_remove_from_ready_queue(Process *p);
void scheduler_boost_process_priority(Process *p, uint8_t new_priority);
void scheduler_boost_process_priority_under_lock(Process *p, uint8_t new_priority);
// ktest hook: validates ready-queue push/pop/remove invariants on isolated
// synthetic state. Returns false on any invariant breach.
bool scheduler_ready_queue_self_test();

extern WaitQueue g_epoll_wait_queue;

void preempt_disable();
void preempt_enable();

// SMP: final AP handoff — adopt `idle` as this core's parked context and run
// the idle loop (never returns). Called by the AP itself after bring-up.
void scheduler_enter_idle(Process *idle);
// Broadcasts a resched IPI so idle cores pull newly-ready work.
void scheduler_notify_idle_cpus();

#ifdef DEBUG
// Boot teardown audit (debug builds): snapshot for the audit task in
// kmain — total deferred list length (maintained counter), parked
// kernel-mode zombies on it (the leak class of the old null-matching
// bug), and kernel-mode processes still in the list (any state) other
// than the caller, whose names fill `survivors` comma-separated. Call
// with no scheduler leaf held.
void scheduler_debug_teardown_state(uint64_t except_pid, uint64_t *deferred_total, uint64_t *deferred_kernel_tasks,
                                    uint64_t *remaining_kernel_tasks, char *survivors, uint64_t survivors_cap);
#endif
