#include "wm_metrics.h"

WmMetrics g_metrics = {};

void refresh_wm_metrics()
{
    int scale = gui_ui_scale_pct();
    g_metrics.resize_grip = gui_scaled_metric(RESIZE_GRIP);
    g_metrics.button_size = gui_scaled_metric(BTN_SIZE);
    g_metrics.button_inset_x = gui_scaled_metric(BTN_INSET_X);
    g_metrics.button_inset_y = gui_scaled_metric(BTN_INSET_Y);
    g_metrics.button_spacing = gui_scaled_metric(BTN_SPACING);
    g_metrics.menubar_h = gui_menubar_h();
    g_metrics.desktop_margin = gui_scaled_metric(DESKTOP_MARGIN);
    g_metrics.dock_reserved_h = shell_dock_reserved_h();
    g_metrics.default_min_w = gui_scaled_metric(MIN_WINDOW_W);
    g_metrics.default_min_h = gui_scaled_metric(MIN_WINDOW_H);
    int border = gui_scaled_metric(FRAME_BORDER);
    g_metrics.frame_border = border < 1 ? 1 : border;
    // A single 1-px hairline edge: the client is inset by just the border, so
    // the outline is one razor-thin semi-transparent stroke (no inner rim).
    g_metrics.frame_body_inset = g_metrics.frame_border;
    // Symmetric shadow pad: the soft shadow spreads equally on all four sides.
    int shadow_pad = gui_scaled_metric(12);
    if (shadow_pad < 1)
        shadow_pad = 1;
    g_metrics.frame_shadow_offset_x = shadow_pad;
    g_metrics.frame_shadow_offset_y = shadow_pad;
}
