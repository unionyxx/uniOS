#pragma once
#include <stdint.h>
#include <uapi/event.h>

#define EVENT_QUEUE_SIZE 128

struct EventQueue
{
    Event events[EVENT_QUEUE_SIZE];
    int head, tail;
};

void event_init(EventQueue &q);
void event_push(EventQueue &q, const Event &e);
bool event_poll(EventQueue &q, Event &out);
bool event_empty(const EventQueue &q);
void gui_set_wm_pid(uint64_t pid);
uint64_t gui_get_wm_pid();
void gui_set_focus_pid(uint64_t pid);
uint64_t gui_get_focus_pid();

/// Poll hardware input and push events into the global event queue.
void pump_events();
