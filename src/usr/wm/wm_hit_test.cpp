#include "wm_input.h"
#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"
#include "wm_window.h"

int hit_test_resize(const Window &w, int px, int py)
{
    if (!is_user_window(w) || !is_window_visible(w) || w.transparent || !w.entry ||
        !(w.entry->flags & WIN_FLAG_RESIZABLE) || w.entry->state == WIN_MAXIMIZED)
        return RESIZE_NONE;

    // Grips straddle the window's own edges. The outer bounds include the drop
    // shadow, so measuring from them would push the right/bottom grips out past
    // the shadow and away from the window.
    int grip = wm_resize_grip();
    int x1 = w.x + window_effective_w(w);
    int y1 = w.y + window_effective_h(w);

    if (px < w.x - grip || px >= x1 + grip || py < w.y - grip || py >= y1 + grip)
        return RESIZE_NONE;

    int edges = RESIZE_NONE;
    if (px < w.x + grip)
        edges |= RESIZE_LEFT;
    if (px >= x1 - grip)
        edges |= RESIZE_RIGHT;
    if (py < w.y + grip)
        edges |= RESIZE_TOP;
    if (py >= y1 - grip)
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

// The unified-header drag zone: the full-width top gui_headerbar_h() band of the
// client area. Traffic-light buttons, resize edges and any client-published
// header input rects are tested earlier in the hit-test cascade, so they stay
// interactive; everything else in the band drags the window.
static bool point_in_header_drag_zone(const Window &w, int px, int py)
{
    if (w.transparent)
        return false;
    // Start at the very top edge (including the hairline) so there is no dead
    // row, and span the headerband below it.
    DirtyRect zone = {w.x, w.y, w.w, wm_frame_body_inset() + gui_headerbar_h()};
    return point_in_rect(zone, px, py);
}

static bool point_in_rounded_window_titlebar(const Window &w, int px, int py)
{
    return point_in_header_drag_zone(w, px, py);
}

static bool point_in_rounded_window_client(const Window &w, int px, int py)
{
    DirtyRect client = window_visible_client_bounds(w);
    if (!point_in_rect(client, px, py))
        return false;
    if (w.transparent)
        return true;

    int inner_r = gui_radius_xl() - wm_frame_body_inset();
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

bool point_in_header_input(const Window &w, int px, int py)
{
    if (w.transparent || !w.entry)
        return false;

    // Snapshot the client-published rects across the seq fence; the writer
    // bumps header_input_seq to odd before the payload and to even after, so
    // a stable even seq means a consistent snapshot (a stable odd seq means a
    // mid-write torn read: retry, then bail out).
    const WindowEntry &e = *w.entry;
    Rect rects[WINDOW_HEADER_INPUT_MAX];
    uint32_t seq0 = 0, seq1 = 1;
    int count = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        seq0 = e.header_input_seq;
        smp_rmb();
        if (seq0 & 1u)
            continue;
        count = e.header_input_count;
        if (count < 0)
            count = 0;
        if (count > WINDOW_HEADER_INPUT_MAX)
            count = WINDOW_HEADER_INPUT_MAX;
        for (int i = 0; i < count; i++)
            rects[i] = e.header_input[i];
        smp_rmb();
        seq1 = e.header_input_seq;
        if (seq0 == seq1)
            break;
    }
    if (seq0 != seq1 || (seq0 & 1u) || count <= 0)
        return false;

    // The rects live in client canvas space; the pointer is in screen space and
    // the client body is inset by the chrome frame and offset by the scroll.
    int cx = px - w.x - wm_frame_body_inset() + w.scroll_x;
    int cy = py - w.y - wm_frame_body_inset() + w.scroll_y;
    for (int i = 0; i < count; i++) {
        const Rect &r = rects[i];
        if (r.w > 0 && r.h > 0 && cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h)
            return true;
    }
    return false;
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
