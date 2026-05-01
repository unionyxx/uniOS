#include <kernel/theme.h>

const Theme THEME_MOCHA = {.desktop_top = 0xFF111111,
                           .desktop_bottom = 0xFF111111,

                           .window_bg = 0xFF111111,
                           .surface_elevated = 0xFF1A1A1A,
                           .title_active = 0xFF333333,
                           .title_inactive = 0xFF222222,
                           .border = 0xFF444444,
                           .shadow = 0xFF000000,

                           .text_primary = 0xFFFFFFFF,
                           .text_dim = 0xFFAAAAAA,
                           .text_app_name = 0xFFFFFFFF,

                           .menubar_bg = 0xFF1A1A1A,
                           .dock_bg = 0xFF1A1A1A,
                           .accent = 0xFFE6E6E6,
                           .separator = 0xFF444444,

                           .btn_close = 0xFFFF5F57,
                           .btn_minimize = 0xFFFBBE2C,
                           .btn_maximize = 0xFF2AC744,

                           .term_bg = 0xFF000000};

const Theme *g_theme = &THEME_MOCHA;
