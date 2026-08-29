#include "wm_window.h"
#include "wm_metrics.h"

DirtyRect window_client_bounds(const Window &w)
{
    return {w.x, w.y, w.w, w.h};
}

DirtyRect window_outer_bounds(const Window &w)
{
    int eff_w = window_effective_w(w);
    int eff_h = window_effective_h(w);
    if (w.transparent)
        return {w.x, w.y, eff_w, eff_h};
    int t_h = wm_title_bar_h();
    return {w.x, w.y - t_h, eff_w + wm_frame_shadow_offset_x(), eff_h + t_h + wm_frame_shadow_offset_y()};
}

static inline int window_safe_side_inset()
{
    int inset = gui_scaled_metric(FRAME_OCCLUSION_INSET) + wm_frame_border() + wm_frame_shadow_offset_x();
    return inset < 2 ? 2 : inset;
}

void get_window_opaque_cover_rects(const Window &w, DirtyRect *out_rects, int *out_count)
{
    if (!out_rects || !out_count)
        return;
    *out_count = 0;
    if (w.transparent) {
        return;
    }

    int eff_w = window_effective_w(w);
    int eff_h = window_effective_h(w);

    int side_inset = window_safe_side_inset();
    int title_h = wm_title_bar_h();
    int radius = gui_scaled_metric(12) - wm_frame_border();
    if (radius < side_inset)
        radius = side_inset;

    DirtyRect rects[3];
    int count = 0;

    int shadow_pad = wm_frame_shadow_offset_y();
    DirtyRect main = {w.x + side_inset, w.y - title_h + radius, eff_w - side_inset * 2,
                      eff_h + title_h - radius - radius - shadow_pad};
    if (main.w > 0 && main.h > 0)
        rects[count++] = main;

    DirtyRect top_band = {w.x + radius, w.y - title_h + side_inset, eff_w - radius * 2, radius - side_inset};
    if (top_band.w > 0 && top_band.h > 0)
        rects[count++] = top_band;

    DirtyRect bottom_band = {w.x + radius, w.y + eff_h - radius, eff_w - radius * 2, radius - side_inset};
    if (bottom_band.w > 0 && bottom_band.h > 0)
        rects[count++] = bottom_band;

    if (count == 0) {
        DirtyRect fallback = {w.x + side_inset, w.y, eff_w - side_inset * 2, eff_h};
        DirtyRect fallback_client = {w.x, w.y, eff_w, eff_h};
        rects[count++] = (fallback.w > 0 && fallback.h > 0) ? fallback : fallback_client;
    }

    *out_count = count;
    for (int i = 0; i < count; i++)
        out_rects[i] = rects[i];
}

DirtyRect window_opaque_bounds(const Window &w)
{
    if (w.transparent)
        return {0, 0, 0, 0};
    int eff_w = window_effective_w(w);
    int eff_h = window_effective_h(w);
    int side_inset = window_safe_side_inset();
    int title_h = wm_title_bar_h();
    int radius = gui_scaled_metric(12) - wm_frame_border();
    if (radius < side_inset)
        radius = side_inset;

    DirtyRect main = {w.x + side_inset, w.y - title_h + radius, eff_w - side_inset * 2,
                      eff_h + title_h - radius - radius};
    if (main.w > 0 && main.h > 0)
        return main;

    DirtyRect fallback = {w.x + side_inset, w.y, eff_w - side_inset * 2, eff_h};
    DirtyRect fallback_client = {w.x, w.y, eff_w, eff_h};
    return (fallback.w > 0 && fallback.h > 0) ? fallback : fallback_client;
}

DirtyRect window_occlusion_bounds(const Window &w)
{
    if (w.transparent)
        return {0, 0, 0, 0};
    return window_opaque_bounds(w);
}

bool is_visible_state(uint32_t state)
{
    return state != WIN_MINIMIZED && state != WIN_HIDDEN;
}

bool is_window_visible(const Window &w)
{
    return w.active && (!w.entry || is_visible_state(w.entry->state));
}

bool is_user_window(const Window &w)
{
    return w.entry && (w.entry->flags & WIN_FLAG_SYSTEM) == 0;
}

