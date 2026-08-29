#include "wm_input.h"
#include "wm_window.h"

void post_mouse_event_to_window(const Window &w, EventType type, int px, int py, uint8_t button, int8_t scroll_y)
{
    if (!is_user_window(w) || !is_window_visible(w) || !w.owner_pid)
        return;
    Event ev = {};
    ev.type = type;
    ev.mouse.x = px - w.x + w.scroll_x;
    ev.mouse.y = py - w.y + w.scroll_y;
    ev.mouse.button = button;
    ev.mouse.scroll_y = scroll_y;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uint64_t)&ev);
}

void post_key_event_to_window(const Window &w, EventType type, char c, uint8_t scancode)
{
    if (!is_user_window(w) || !is_window_visible(w) || !w.owner_pid)
        return;
    Event ev = {};
    ev.type = type;
    ev.key.c = c;
    ev.key.scancode = scancode;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uintptr_t)&ev);
}

void post_plain_event_to_window(const Window &w, EventType type)
{
    if (!is_user_window(w) || !w.owner_pid)
        return;
    Event ev = {};
    ev.type = type;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uintptr_t)&ev);
}

void post_scroll_event_to_window(const Window &w)
{
    if (!is_user_window(w) || !is_window_visible(w) || !w.owner_pid)
        return;
    Event ev = {};
    ev.type = EVT_WINDOW_SCROLL;
    ev.scroll.scroll_x = w.scroll_x;
    ev.scroll.scroll_y = w.scroll_y;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uintptr_t)&ev);
}

static void post_plain_event_to_pid(uint32_t pid, EventType type)
{
    if (!pid)
        return;
    Event ev = {};
    ev.type = type;
    syscall2(SYS_POST_EVENT, pid, (uintptr_t)&ev);
}

void post_focus_change_events(uint32_t prev_pid, uint32_t next_pid)
{
    if (prev_pid == next_pid)
        return;
    post_plain_event_to_pid(prev_pid, EVT_UNFOCUS);
    post_plain_event_to_pid(next_pid, EVT_FOCUS);
}
