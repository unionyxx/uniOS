#pragma once

#include "libgui/gui.h"

static constexpr int SHELL_DOCK_ITEM_COUNT = 4;
static constexpr int SHELL_DOCK_ICON_SIZE = 48;
static constexpr int SHELL_DOCK_ICON_SPACING = 14;
static constexpr int SHELL_DOCK_INDICATOR_SIZE = 3;
static constexpr int SHELL_DOCK_INDICATOR_BOTTOM_PAD = 6;

static constexpr int SHELL_DOCK_PANEL_PADDING_X = 16;
static constexpr int SHELL_DOCK_PANEL_PADDING_Y = 10;

static constexpr int SHELL_DOCK_SHADOW_MARGIN_X = 38;
static constexpr int SHELL_DOCK_SHADOW_MARGIN_Y = 6;
static constexpr int SHELL_DOCK_BOTTOM_INSET = 4;

static inline int shell_dock_icon_size()
{
    return gui_scaled_metric(SHELL_DOCK_ICON_SIZE);
}
static inline int shell_dock_icon_spacing()
{
    return gui_scaled_metric(SHELL_DOCK_ICON_SPACING);
}
static inline int shell_dock_indicator_size()
{
    return gui_scaled_metric(SHELL_DOCK_INDICATOR_SIZE);
}

static inline int shell_dock_total_icons_w(int item_count)
{
    if (item_count < 1)
        item_count = 1;
    return item_count * shell_dock_icon_size() + (item_count - 1) * shell_dock_icon_spacing();
}

// Panel dimensions (the actual blurred glass shape, ignoring the canvas size)
static inline int shell_dock_panel_w(uint32_t /*dock_w*/)
{
    return shell_dock_total_icons_w(SHELL_DOCK_ITEM_COUNT) + (gui_scaled_metric(SHELL_DOCK_PANEL_PADDING_X) * 2);
}
static inline int shell_dock_panel_h(uint32_t /*dock_h*/)
{
    return shell_dock_icon_size() + (gui_scaled_metric(SHELL_DOCK_PANEL_PADDING_Y) * 2);
}

static inline int shell_dock_panel_x(uint32_t dock_w)
{
    int panel_w = shell_dock_panel_w(dock_w);
    int x = ((int)dock_w - panel_w) / 2;
    return x < 0 ? 0 : x;
}
static inline int shell_dock_panel_y()
{
    return gui_scaled_metric(30);
}

static inline int shell_dock_icon_y(uint32_t /*dock_h*/)
{
    return shell_dock_panel_y() + gui_scaled_metric(SHELL_DOCK_PANEL_PADDING_Y);
}
static inline int shell_dock_indicator_y(uint32_t dock_h)
{
    return shell_dock_panel_y() + shell_dock_panel_h(dock_h) - gui_scaled_metric(SHELL_DOCK_INDICATOR_BOTTOM_PAD) -
           shell_dock_indicator_size();
}

static inline int shell_dock_window_w(int item_count)
{
    return shell_dock_total_icons_w(item_count) + (gui_scaled_metric(SHELL_DOCK_PANEL_PADDING_X) * 2) +
           (gui_scaled_metric(SHELL_DOCK_SHADOW_MARGIN_X) * 2);
}
static inline int shell_dock_window_h()
{
    return shell_dock_panel_h(0) + shell_dock_panel_y() + gui_scaled_metric(SHELL_DOCK_SHADOW_MARGIN_Y);
}
static inline int shell_dock_bottom_inset()
{
    return gui_scaled_metric(SHELL_DOCK_BOTTOM_INSET);
}
static inline int shell_dock_reserved_h()
{
    return shell_dock_panel_h(0) + gui_scaled_metric(SHELL_DOCK_SHADOW_MARGIN_Y) + shell_dock_bottom_inset() +
           gui_scaled_metric(12);
}
