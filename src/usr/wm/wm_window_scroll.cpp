#include "wm_core.h"

void publish_window_scroll(const Window &w)
{
    if (!w.entry)
        return;
    if (w.entry->scroll_x == w.scroll_x && w.entry->scroll_y == w.scroll_y)
        return;
    w.entry->scroll_x = w.scroll_x;
    w.entry->scroll_y = w.scroll_y;
    asm volatile("sfence" ::: "memory");
}

static int window_max_scroll_x(const Window &w)
{
    int visible_w = w.w > 0 ? w.w : 0;
    int content_w = w.content_w > 0 ? w.content_w : visible_w;
    return content_w > visible_w ? content_w - visible_w : 0;
}

static int window_max_scroll_y(const Window &w)
{
    int visible_h = w.h > 0 ? w.h : 0;
    int content_h = w.content_h > 0 ? w.content_h : visible_h;
    return content_h > visible_h ? content_h - visible_h : 0;
}

bool clamp_window_scroll(Window &w)
{
    int old_x = w.scroll_x;
    int old_y = w.scroll_y;
    int max_x = window_max_scroll_x(w);
    int max_y = window_max_scroll_y(w);

    if (max_x <= 0)
        w.scroll_x = 0;
    else if (w.scroll_x < 0)
        w.scroll_x = 0;
    else if (w.scroll_x > max_x)
        w.scroll_x = max_x;

    if (max_y <= 0)
        w.scroll_y = 0;
    else if (w.scroll_y < 0)
        w.scroll_y = 0;
    else if (w.scroll_y > max_y)
        w.scroll_y = max_y;

    return old_x != w.scroll_x || old_y != w.scroll_y;
}

bool scroll_window_content(Window &w, int delta_x, int delta_y)
{
    if (!is_user_window(w) || !is_window_visible(w) || !w.buffer)
        return false;
    if (delta_x == 0 && delta_y == 0)
        return false;

    if (window_max_scroll_x(w) <= 0 && window_max_scroll_y(w) <= 0)
        return false;

    int old_x = w.scroll_x;
    int old_y = w.scroll_y;
    int64_t next_x = (int64_t)w.scroll_x + (int64_t)delta_x;
    int64_t next_y = (int64_t)w.scroll_y + (int64_t)delta_y;
    w.scroll_x = clamp_i64_to_int(next_x);
    w.scroll_y = clamp_i64_to_int(next_y);
    clamp_window_scroll(w);
    if (old_x == w.scroll_x && old_y == w.scroll_y)
        return false;

    publish_window_scroll(w);
    post_scroll_event_to_window(w);

    DirtyRect client = window_visible_client_bounds(w);
    enqueue_damage_rect(client.x, client.y, client.w, client.h);
    return true;
}

