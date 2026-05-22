#pragma once
#include <stdint.h>

static constexpr int32_t U = 8;

static constexpr int32_t MENU_BAR_HEIGHT = U * 3;
static constexpr int32_t TITLE_BAR_HEIGHT = U * 3;
static constexpr int32_t DOCK_HEIGHT = U * 6;
static constexpr int32_t DOCK_ICON_SIZE = U * 4;
static constexpr int32_t DOCK_ICON_GAP = U * 2;
static constexpr int32_t DOCK_PADDING = U * 1;
static constexpr int32_t DOCK_BOTTOM_GAP = U * 1;
static constexpr int32_t CLOSE_BTN_SIZE = U + (U / 2);
static constexpr int32_t CLOSE_BTN_INSET = U * 1;
static constexpr int32_t TRAFFIC_LIGHT_GAP = U * 1;
static constexpr int32_t MENUBAR_LOGO_X = U * 2;
static constexpr int32_t MENUBAR_ITEM_GAP = U * 2;

static constexpr int32_t FONT_W = 8;
static constexpr int32_t FONT_H = 16;
static constexpr int32_t CURSOR_W = 12;
static constexpr int32_t CURSOR_H = 19;

static constexpr int32_t MAX_WINDOWS = 16;
static constexpr int32_t MAX_TITLE_CHARS = 32;

namespace Kernel
{
struct Theme
{
    uint32_t desktop_top;
    uint32_t desktop_bottom;
    uint32_t window_bg;
    uint32_t surface_elevated;
    uint32_t title_active;
    uint32_t title_inactive;
    uint32_t border;
    uint32_t shadow;
    uint32_t text_primary;
    uint32_t text_dim;
    uint32_t text_app_name;
    uint32_t menubar_bg;
    uint32_t dock_bg;
    uint32_t accent;
    uint32_t separator;
    uint32_t btn_close;
    uint32_t btn_minimize;
    uint32_t btn_maximize;
    uint32_t term_bg;
};

inline constexpr Theme THEME_DEFAULT = {
    .desktop_top = 0xFF111214,
    .desktop_bottom = 0xFF111214,
    .window_bg = 0xFF111214,
    .surface_elevated = 0xFF1C1E21,
    .title_active = 0xFF2F343A,
    .title_inactive = 0xFF1A1C20,
    .border = 0xFF3D444D,
    .shadow = 0xFF000000,
    .text_primary = 0xFFF2F2F0,
    .text_dim = 0xFF9A9FA7,
    .text_app_name = 0xFFF2F2F0,
    .menubar_bg = 0xFF15171A,
    .dock_bg = 0xFF15171A,
    .accent = 0xFF6E7784,
    .separator = 0xFF2F343A,
    .btn_close = 0xFFFF5F57,
    .btn_minimize = 0xFFFBBE2C,
    .btn_maximize = 0xFF2AC744,
    .term_bg = 0xFF000000 // Pure black for classic terminal feel
};

inline const Theme *g_theme = &THEME_DEFAULT;
}

#define COLOR_ACCENT Kernel::g_theme->accent
#define COLOR_TEXT_PRIMARY Kernel::g_theme->text_primary
#define COLOR_TEXT_DIM Kernel::g_theme->text_dim
#define COLOR_TEXT_APP_NAME Kernel::g_theme->text_app_name
#define COLOR_WINDOW_BG Kernel::g_theme->window_bg
#define COLOR_SURFACE_ELEV Kernel::g_theme->surface_elevated
#define COLOR_TITLE_ACTIVE Kernel::g_theme->title_active
#define COLOR_TITLE_INACTIVE Kernel::g_theme->title_inactive
#define COLOR_BORDER Kernel::g_theme->border
#define COLOR_SHADOW Kernel::g_theme->shadow
#define COLOR_MENUBAR_BG Kernel::g_theme->menubar_bg
#define COLOR_DOCK_BG Kernel::g_theme->dock_bg
#define COLOR_CLOSE_BTN Kernel::g_theme->btn_close
#define COLOR_RED 0xFFFF5F57
#define COLOR_MIN_BTN Kernel::g_theme->btn_minimize
#define COLOR_MAX_BTN Kernel::g_theme->btn_maximize
#define COLOR_DESKTOP_TOP Kernel::g_theme->desktop_top
#define COLOR_DESKTOP_BOTTOM Kernel::g_theme->desktop_bottom
#define COLOR_BLACK 0xFF000000
#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_SUCCESS 0xFFA6E3A1
#define COLOR_YELLOW 0xFFF9E2AF
#define COLOR_CYAN 0xFF89DCEB
#define COLOR_PURPLE 0xFFCBA6F7
#define COLOR_GRAY 0xFF585B70
#define COLOR_GREEN COLOR_SUCCESS
#define COLOR_DIM_GRAY COLOR_GRAY
#define COLOR_TIMESTAMP Kernel::g_theme->text_dim
#define COLOR_TEXT Kernel::g_theme->text_primary
#define COLOR_MUTED Kernel::g_theme->text_dim
#define COLOR_WARNING COLOR_YELLOW
#define COLOR_ERROR COLOR_RED
#define COLOR_BG COLOR_BLACK
#define COLOR_HELP_HEADER COLOR_PURPLE
#define COLOR_FIELD Kernel::g_theme->surface_elevated
#define COLOR_FIELD_ACTIVE Kernel::g_theme->title_active
#define COLOR_ERR 0xFFEBA0AC

using Kernel::g_theme;
