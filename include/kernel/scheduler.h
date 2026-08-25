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
// Interrupts are disabled while held. Leaf locks (fd_lock, pipe, epoll) may
// be taken underneath; the reverse order is forbidden.
uint64_t scheduler_big_lock_irqsave();
void scheduler_big_unlock_irqrestore(uint64_t flags);

// True when a pending signal would take its default-fatal action; blocking
// loops use this to bail out with -EINTR instead of sleeping through kill.
bool scheduler_fatal_signal_pending(const Process *p);

[[nodiscard]] Process *scheduler_get_process_list();

void scheduler_schedule_elapsed(uint32_t elapsed_jiffies);
void scheduler_sleep(uint64_t ticks);
void scheduler_sleep_ms(uint64_t ms);

struct WaitQueue;
struct Spinlock;
void scheduler_wait(WaitQueue *q, Spinlock *lock);
void scheduler_wake_all(WaitQueue *q);
void scheduler_wake_all_locked(WaitQueue *q);
void scheduler_wake_one(WaitQueue *q);

struct SyscallFrame;
[[nodiscard]] int64_t sys_thread_create(void (*entry)(), void *arg, void *stack_top, struct SyscallFrame *frame);
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
