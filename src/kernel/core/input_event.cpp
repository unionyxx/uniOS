#include <drivers/class/hid/input.h>
#include <kernel/event.h>

EventQueue g_event_queue;
static volatile uint64_t s_gui_wm_pid = 0;
static volatile uint64_t s_gui_focus_pid = 0;

static int32_t s_last_mx = -1, s_last_my = -1;
static bool s_prev_left = false;
static bool s_prev_right = false;

void event_init(EventQueue &q)
{
    q.head = 0;
    q.tail = 0;
}

void event_push(EventQueue &q, const Event &e)
{
    int next = (q.tail + 1) % EVENT_QUEUE_SIZE;
    if (next == q.head)
        return;
    q.events[q.tail] = e;
    q.tail = next;

    extern void scheduler_notify_input_waiters();
    scheduler_notify_input_waiters();
}

bool event_poll(EventQueue &q, Event &out)
{
    if (q.head == q.tail)
        return false;
    out = q.events[q.head];
    q.head = (q.head + 1) % EVENT_QUEUE_SIZE;
    return true;
}

bool event_empty(const EventQueue &q)
{
    return q.head == q.tail;
}

#include <kernel/process.h>

void gui_set_wm_pid(uint64_t pid)
{
    s_gui_wm_pid = pid;
    if (s_gui_focus_pid == 0) {
        s_gui_focus_pid = pid;
    }
}

uint64_t gui_get_wm_pid()
{
    return s_gui_wm_pid;
}

void gui_set_focus_pid(uint64_t pid)
{
    s_gui_focus_pid = pid;
}

uint64_t gui_get_focus_pid()
{
    return s_gui_focus_pid;
}

void pump_events()
{
    input_poll();

    uint64_t wm_pid = s_gui_wm_pid;
    Process *wm = wm_pid ? process_find_by_pid(wm_pid) : nullptr;
    if (!wm) {
        if (wm_pid != 0) {
            s_gui_wm_pid = 0;
            if (s_gui_focus_pid == wm_pid) {
                s_gui_focus_pid = 0;
            }
        }
        return;
    }

    uint64_t focus_pid = s_gui_focus_pid;
    Process *key_target = focus_pid ? process_find_by_pid(focus_pid) : nullptr;
    if (!key_target) {
        if (focus_pid != 0) {
            s_gui_focus_pid = 0;
        }
        key_target = wm;
    }

    InputMouseState ms;
    input_mouse_get_state(&ms);

    if (ms.x != s_last_mx || ms.y != s_last_my) {
        event_push(wm->event_queue, {EVT_MOUSE_MOVE, {.mouse = {ms.x, ms.y, 0, 0, 0, 0}}});
        s_last_mx = ms.x;
        s_last_my = ms.y;
    }

    // LMB
    if (ms.left && !s_prev_left) {
        event_push(wm->event_queue, {EVT_MOUSE_DOWN, {.mouse = {ms.x, ms.y, 1, 0, 0, 0}}});
    } else if (!ms.left && s_prev_left) {
        event_push(wm->event_queue, {EVT_MOUSE_UP, {.mouse = {ms.x, ms.y, 1, 0, 0, 0}}});
    }

    // RMB
    if (ms.right && !s_prev_right) {
        event_push(wm->event_queue, {EVT_MOUSE_DOWN, {.mouse = {ms.x, ms.y, 2, 0, 0, 0}}});
    } else if (!ms.right && s_prev_right) {
        event_push(wm->event_queue, {EVT_MOUSE_UP, {.mouse = {ms.x, ms.y, 2, 0, 0, 0}}});
    }

    if (ms.scroll_delta != 0) {
        event_push(wm->event_queue, {EVT_MOUSE_SCROLL, {.mouse = {ms.x, ms.y, 0, 0, ms.scroll_delta, 0}}});
    }

    while (input_keyboard_has_char()) {
        event_push(key_target->event_queue, {EVT_KEY_DOWN, {.key = {input_keyboard_get_char(), 0}}});
    }

    s_prev_left = ms.left;
    s_prev_right = ms.right;
}
