#include "wm_core.h"

int hit_test_resize(const Window &w, int px, int py)
{
    if (!is_user_window(w) || !is_window_visible(w) || w.transparent || !w.entry ||
        !(w.entry->flags & WIN_FLAG_RESIZABLE) || w.entry->state == WIN_MAXIMIZED)
        return RESIZE_NONE;

    DirtyRect outer = window_outer_bounds(w);
    int grip = wm_resize_grip();

    if (px < outer.x - grip || px >= outer.x + outer.w + grip || py < outer.y - grip || py >= outer.y + outer.h + grip)
        return RESIZE_NONE;

    int edges = RESIZE_NONE;
    if (px < outer.x + grip)
        edges |= RESIZE_LEFT;
    if (px >= outer.x + outer.w - grip)
        edges |= RESIZE_RIGHT;
    if (py < outer.y + grip)
        edges |= RESIZE_TOP;
    if (py >= outer.y + outer.h - grip)
        edges |= RESIZE_BOTTOM;
    return edges;
}

static bool point_in_rounded_window_outer(const Window &w, int px, int py)
{
    DirtyRect outer = window_outer_bounds(w);
    if (!point_in_rect(outer, px, py))
        return false;
    if (w.transparent)
        return true;

    int radius = gui_scaled_metric(12);
    if (radius < 0)
        radius = 0;
    return gui_rounded_rect_coverage_local(px - outer.x, py - outer.y, outer.w, outer.h, radius,
                                           GUI_ROUNDED_EDGE_ALL) != 0;
}

static bool point_in_rounded_window_titlebar(const Window &w, int px, int py)
{
    if (w.transparent)
        return false;
    DirtyRect title = {w.x, w.y - wm_title_bar_h(), w.w, wm_title_bar_h()};
    if (!point_in_rect(title, px, py))
        return false;

    int radius = gui_scaled_metric(12) - wm_frame_border();
    if (radius < 0)
        radius = 0;
    if (radius > title.w / 2)
        radius = title.w / 2;
    if (radius > title.h)
        radius = title.h;
    return gui_rounded_rect_coverage_local(px - title.x, py - title.y, title.w, title.h, radius,
                                           GUI_ROUNDED_EDGE_TOP) != 0;
}

static bool point_in_rounded_window_client(const Window &w, int px, int py)
{
    DirtyRect client = window_visible_client_bounds(w);
    if (!point_in_rect(client, px, py))
        return false;
    if (w.transparent)
        return true;

    int inner_r = gui_scaled_metric(12) - wm_frame_border();
    if (inner_r <= 0)
        return true;

    if (px >= client.x + inner_r && px < client.x + client.w - inner_r)
        return true;
    if (py < client.y + client.h - inner_r)
        return true;

    if (inner_r > client.w / 2)
        inner_r = client.w / 2;
    if (inner_r > client.h / 2)
        inner_r = client.h / 2;

    return gui_rounded_rect_coverage_local(px - client.x, py - client.y, client.w, client.h, inner_r,
                                           GUI_ROUNDED_EDGE_BOTTOM) != 0;
}

bool point_in_titlebar(const Window &w, int px, int py)
{
    return point_in_rounded_window_titlebar(w, px, py);
}

bool point_in_client(const Window &w, int px, int py)
{
    return point_in_rounded_window_client(w, px, py);
}

bool point_in_outer(const Window &w, int px, int py)
{
    return point_in_rounded_window_outer(w, px, py);
}

bool point_in_button(const Window &w, int px, int py, int idx)
{
    return point_in_rect(window_button_bounds(w, idx), px, py);
}

int system_window_hit(int px, int py)
{
    for (int i = 1; i >= 0; i--) {
        if (i >= g_window_count)
            continue;
        const Window &w = g_windows[i];
        if (!is_window_visible(w) || !w.buffer)
            continue;
        if (w.transparent ? point_hits_window_visible_pixel(w, px, py) : point_in_client(w, px, py))
            return i;
    }
    return -1;
}

bool pointer_blocked_by_shell_overlay(int px, int py)
{
    return g_storage_prompt.visible || g_context_menu.open || g_index.active || g_control_center.open ||
           system_window_hit(px, py) >= 0;
}

