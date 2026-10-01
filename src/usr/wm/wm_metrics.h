#pragma once

#include <../libgui/gui.h>

#include "wm_rect.h"

extern WmMetrics g_metrics;
void refresh_wm_metrics();

static inline int wm_resize_grip()
{
    return g_metrics.resize_grip;
}
static inline int wm_button_size()
{
    return g_metrics.button_size;
}
static inline int wm_button_inset_x()
{
    return g_metrics.button_inset_x;
}
static inline int wm_button_inset_y()
{
    return g_metrics.button_inset_y;
}
static inline int wm_button_spacing()
{
    return g_metrics.button_spacing;
}
static inline int wm_menubar_h()
{
    return g_metrics.menubar_h;
}
static inline int wm_desktop_margin()
{
    return g_metrics.desktop_margin;
}
static inline int wm_dock_reserved_h()
{
    return g_metrics.dock_reserved_h;
}
static inline int wm_default_min_w()
{
    return g_metrics.default_min_w;
}
static inline int wm_default_min_h()
{
    return g_metrics.default_min_h;
}
static inline int wm_frame_border()
{
    return g_metrics.frame_border;
}
static inline int wm_frame_body_inset()
{
    return g_metrics.frame_body_inset;
}
static inline int wm_frame_shadow_offset_x()
{
    return g_metrics.frame_shadow_offset_x;
}
static inline int wm_frame_shadow_offset_y()
{
    return g_metrics.frame_shadow_offset_y;
}

// Metrics functions are now inlined to g_metrics access.
static inline int wm_window_damage_pad()
{
    int pad = gui_scaled_metric(WINDOW_DAMAGE_PAD_BASE) + wm_frame_border() + wm_frame_shadow_offset_y();
    return pad < CURSOR_DAMAGE_PAD ? CURSOR_DAMAGE_PAD : pad;
}
static inline int wm_window_damage_pad_interactive()
{
    int pad = gui_scaled_metric(WINDOW_DAMAGE_PAD_BASE) + wm_frame_border();
    return pad < 2 ? 2 : pad;
}

static inline DirtyRect window_button_bounds(const Window &w, int button_index)
{
    int button_size = wm_button_size();
    // Traffic lights overlay the client area, vertically centered in the
    // unified headerbar region (top gui_headerbar_h() pixels of the client).
    // The client body begins frame_body_inset below the window's top edge, so
    // the band's on-screen origin is offset by the same amount.
    int header_h = gui_headerbar_h();
    int button_y = w.y + wm_frame_body_inset() + (header_h - button_size) / 2 + wm_button_inset_y();
    return {w.x + wm_button_inset_x() + button_index * wm_button_spacing(), button_y, button_size, button_size};
}
static inline void window_button_center(const Window &w, int button_index, int *cx, int *cy)
{
    DirtyRect button = window_button_bounds(w, button_index);
    if (cx)
        *cx = button.x + button.w / 2;
    if (cy)
        *cy = button.y + button.h / 2;
}
