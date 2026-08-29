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
    g_metrics.title_bar_h = gui_title_bar_h();
    g_metrics.menubar_h = gui_menubar_h();
    g_metrics.desktop_margin = gui_scaled_metric(DESKTOP_MARGIN);
    g_metrics.dock_reserved_h = shell_dock_reserved_h();
    g_metrics.default_min_w = gui_scaled_metric(MIN_WINDOW_W);
    g_metrics.default_min_h = gui_scaled_metric(MIN_WINDOW_H);
    int border = gui_scaled_metric(FRAME_BORDER);
    g_metrics.frame_border = border < 1 ? 1 : border;
    int shadow_x = gui_scaled_metric(1);
    g_metrics.frame_shadow_offset_x = shadow_x < 1 ? 1 : shadow_x;
    int shadow_y = gui_scaled_metric(3);
    g_metrics.frame_shadow_offset_y = shadow_y < 1 ? 1 : shadow_y;
}
