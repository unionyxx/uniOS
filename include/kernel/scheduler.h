#pragma once
#include <stdint.h>

struct Process;

void scheduler_init();
[[nodiscard]] Process *scheduler_create_task(void (*entry)(), const char *name);
void scheduler_schedule();
void scheduler_yield();
void scheduler_notify_input_waiters();
void scheduler_wake_process(Process *p);

[[nodiscard]] Process *scheduler_get_process_list();

void scheduler_sleep(uint64_t ticks);
void scheduler_sleep_ms(uint64_t ms);

struct WaitQueue;
struct Spinlock;
void scheduler_wait(WaitQueue *q, Spinlock *lock);
void scheduler_wake_all(WaitQueue *q);
void scheduler_wake_one(WaitQueue *q);

struct SyscallFrame;
[[nodiscard]] int64_t sys_thread_create(void (*entry)(), void *arg, void *stack_top, struct SyscallFrame *frame);
void scheduler_remove_from_ready_queue(Process *p);
void scheduler_boost_process_priority(Process *p, uint8_t new_priority);

extern WaitQueue g_epoll_wait_queue;

void preempt_disable();
void preempt_enable();


