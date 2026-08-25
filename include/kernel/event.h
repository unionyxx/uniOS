#pragma once
#include <kernel/sync/spinlock.h>
#include <stdint.h>
#include <uapi/event.h>

#define EVENT_QUEUE_SIZE 128

struct EventQueue
{
    Event events[EVENT_QUEUE_SIZE];
    int head, tail;
    Spinlock lock;
};

void event_init(EventQueue &q);
/// Enqueue without waking waiters. Safe under g_sched_lock; the caller must
/// wake the target itself (see scheduler_wake_all_locked).
bool event_enqueue(EventQueue &q, const Event &e);
/// Enqueue and wake all event waiters. Never call while holding g_sched_lock.
void event_push(EventQueue &q, const Event &e);
bool event_poll(EventQueue &q, Event &out);
bool event_empty(const EventQueue &q);
void gui_set_wm_pid(uint64_t pid);
uint64_t gui_get_wm_pid();
void gui_set_focus_pid(uint64_t pid);
uint64_t gui_get_focus_pid();

/// Poll hardware input and push events into the global event queue.
void pump_events();
