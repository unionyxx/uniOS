#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <uapi/gui.h>

static constexpr const char *WALLPAPER_DEFAULT_PATH = "/usr/share/wallpapers/default.uowp";
static constexpr uint16_t WALLPAPER_UOWP_VARIANT_LIGHT = 1;
static constexpr uint16_t WALLPAPER_UOWP_VARIANT_DARK = 2;

static inline const char *wallpaper_default_path_for_theme(uint32_t theme_mode)
{
    (void)theme_mode;
    return WALLPAPER_DEFAULT_PATH;
}

static inline bool wallpaper_is_default_family_path(const char *path)
{
    return !path || path[0] == '\0' || strcmp(path, WALLPAPER_DEFAULT_PATH) == 0;
}

static inline const char *wallpaper_resolve_path_for_theme(const char *path, uint32_t theme_mode)
{
    return wallpaper_is_default_family_path(path) ? wallpaper_default_path_for_theme(theme_mode) : path;
}

static inline uint16_t wallpaper_uowp_variant_for_theme(uint32_t theme_mode)
{
    return theme_mode == GUI_THEME_LIGHT ? WALLPAPER_UOWP_VARIANT_LIGHT : WALLPAPER_UOWP_VARIANT_DARK;
}
