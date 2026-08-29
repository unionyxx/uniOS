#include "wm_window.h"
#include "wm_input.h"
#include "wm_damage.h"
#include "wm_metrics.h"

static void mark_window_titlebar_damage(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    int title_h = w.y - outer.y;
    if (title_h < 0)
        title_h = 0;
    if (title_h > outer.h)
        title_h = outer.h;
    if (title_h > 0)
        enqueue_damage_rect(outer.x, outer.y, outer.w, title_h);
}

void mark_window_frame_damage(const Window &w)
{
    DirtyRect outer = window_outer_bounds(w);
    enqueue_damage_rect_expanded(outer, wm_window_damage_pad());
}

void mark_window_chrome_damage(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    DirtyRect client = window_visible_client_bounds(w);
    DirtyRect overlap = {};
    if (!rect_intersection(outer, client, &overlap)) {
        enqueue_damage_rect(outer.x, outer.y, outer.w, outer.h);
        return;
    }

    if (outer.y < overlap.y)
        enqueue_damage_rect(outer.x, outer.y, outer.w, overlap.y - outer.y);

    int overlap_bottom = overlap.y + overlap.h;
    int outer_bottom = outer.y + outer.h;
    if (overlap_bottom < outer_bottom)
        enqueue_damage_rect(outer.x, overlap_bottom, outer.w, outer_bottom - overlap_bottom);

    if (outer.x < overlap.x)
        enqueue_damage_rect(outer.x, overlap.y, overlap.x - outer.x, overlap.h);

    int overlap_right = overlap.x + overlap.w;
    int outer_right = outer.x + outer.w;
    if (overlap_right < outer_right)
        enqueue_damage_rect(overlap_right, overlap.y, outer_right - overlap_right, overlap.h);
}

void mark_window_decoration_damage(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    mark_window_titlebar_damage(w);
    mark_window_chrome_damage(w);

    bool interactive = g_input.pointer_down && g_input.drag_index >= WM_FIRST_USER_WINDOW;
    int shadow_extent = interactive
                            ? (wm_frame_shadow_offset_x() > wm_frame_shadow_offset_y() ? wm_frame_shadow_offset_x()
                                                                                       : wm_frame_shadow_offset_y())
                            : gui_scaled_metric(8) + gui_scaled_metric(3) + gui_scaled_metric(2) + gui_scaled_metric(1);
    if (shadow_extent < CURSOR_DAMAGE_PAD)
        shadow_extent = CURSOR_DAMAGE_PAD;

    int side_strip = shadow_extent;
    if (side_strip > outer.w)
        side_strip = outer.w;
    if (side_strip > 0) {
        enqueue_damage_rect(outer.x, outer.y, side_strip, outer.h);
        int right_x = outer.x + outer.w - side_strip;
        enqueue_damage_rect(right_x, outer.y, side_strip, outer.h);
    }

    int bottom_strip = shadow_extent;
    if (bottom_strip > outer.h)
        bottom_strip = outer.h;
    if (bottom_strip > 0)
        enqueue_damage_rect(outer.x, outer.y + outer.h - bottom_strip, outer.w, bottom_strip);
}

void mark_exposed_transition_damage(const DirtyRect &old_outer, const DirtyRect &new_outer)
{
    bool manip = g_input.pointer_down && g_input.drag_index >= WM_FIRST_USER_WINDOW;
    int pad = manip ? wm_window_damage_pad_interactive() : wm_window_damage_pad();
    DirtyRect old_padded = rect_expand(old_outer, pad);
    DirtyRect new_padded = rect_expand(new_outer, pad);
    wm::ExposedTransitionDamage dmg =
        wm::compute_exposed_transition_damage(to_policy_rect(old_padded), to_policy_rect(new_padded));
    for (int i = 0; i < dmg.count; i++)
        enqueue_damage_rect(dmg.rects[i].x, dmg.rects[i].y, dmg.rects[i].w, dmg.rects[i].h);
}

void mark_window_transition_damage(const Window &old_w, const Window &new_w)
{
    int pad = (g_input.pointer_down && g_input.drag_index >= WM_FIRST_USER_WINDOW) ? wm_window_damage_pad_interactive()
                                                                                   : wm_window_damage_pad();
    DirtyRect last_rendered_outer;
    if (old_w.transparent) {
        last_rendered_outer = {old_w.last_rendered_x, old_w.last_rendered_y, old_w.last_rendered_w,
                               old_w.last_rendered_h};
    } else {
        int t_h = wm_title_bar_h();
        last_rendered_outer = {old_w.last_rendered_x, old_w.last_rendered_y - t_h,
                               old_w.last_rendered_w + wm_frame_shadow_offset_x(),
                               old_w.last_rendered_h + t_h + wm_frame_shadow_offset_y()};
    }
    DirtyRect o = rect_expand(last_rendered_outer, pad);
    DirtyRect n = rect_expand(window_outer_bounds(new_w), pad);
    enqueue_damage_rect(n.x, n.y, n.w, n.h);
    DirtyRect overlap = {};
    if (!rect_intersection(o, n, &overlap)) {
        enqueue_damage_rect(o.x, o.y, o.w, o.h);
        return;
    }
    if (rect_contains(overlap, o))
        return;
    if (o.y < overlap.y)
        enqueue_damage_rect(o.x, o.y, o.w, overlap.y - o.y);
    if (overlap.y + overlap.h < o.y + o.h)
        enqueue_damage_rect(o.x, overlap.y + overlap.h, o.w, o.y + o.h - (overlap.y + overlap.h));
    if (o.x < overlap.x)
        enqueue_damage_rect(o.x, overlap.y, overlap.x - o.x, overlap.h);
    if (overlap.x + overlap.w < o.x + o.w)
        enqueue_damage_rect(overlap.x + overlap.w, overlap.y, o.x + o.w - (overlap.x + overlap.w),
                            overlap.y + o.h - overlap.y);
}

