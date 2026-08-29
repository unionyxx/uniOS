#include "wm_metrics.h"
#include "wm_present.h"
#include "wm_render.h"
#include "wm_settings.h"

static int g_cached_top_r = -1;
static uint8_t g_top_corner_mask_lut[64][64] = {};

void fill_top_rounded_rect_clipped(Surface *dst, int x, int y, int w, int h, int r, uint32_t color)
{
    if (!dst || !dst->buffer || w <= 0 || h <= 0)
        return;

    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h)
        r = h;

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (!base_alpha)
        return;

    const uint32_t pitch = dst->pitch / 4;
    const int dst_w = static_cast<int>(dst->width);
    const int dst_h = static_cast<int>(dst->height);

    int start_y = y < 0 ? 0 : y;
    int end_y = y + h > dst_h ? dst_h : y + h;
    int start_x = x < 0 ? 0 : x;
    int end_x = x + w > dst_w ? dst_w : x + w;

    if (r <= 0) {
        gui_fill_rect(dst, start_x, start_y, end_x - start_x, end_y - start_y, color);
        return;
    }

    static constexpr int kCornerMaskMax = 64;
    int local_r = r > kCornerMaskMax ? kCornerMaskMax : r;
    uint8_t corner_mask[kCornerMaskMax];
    int corner_mask_y = -1;

    const int center_start_x = x + local_r;
    const int center_end_x = x + w - local_r;
    const int top_band_end = y + local_r;
    const bool full_opaque = base_alpha == 255;

    for (int py = start_y; py < end_y; ++py) {
        const int row = py - y;
        uint32_t *dst_row = &dst->buffer[static_cast<size_t>(py) * pitch];

        if (py >= top_band_end) {
            if (full_opaque) {
                int span = end_x - start_x;
                if (span <= 0)
                    continue;
                uint32_t *span_dst = &dst_row[start_x];
                uint32_t v0 = color, v1 = color, v2 = color, v3 = color;
                int i = 0;
                for (; i + 7 < span; i += 8) {
                    span_dst[0] = v0;
                    span_dst[1] = v1;
                    span_dst[2] = v2;
                    span_dst[3] = v3;
                    span_dst[4] = v0;
                    span_dst[5] = v1;
                    span_dst[6] = v2;
                    span_dst[7] = v3;
                    span_dst += 8;
                }
                for (; i < span; ++i)
                    *span_dst++ = v0;
            } else {
                for (int px = start_x; px < end_x; ++px)
                    dst_row[px] = blend_rgb(dst_row[px], color, base_alpha);
            }
            continue;
        }

        if (row != corner_mask_y) {
            if (local_r != g_cached_top_r && local_r <= 64) {
                for (int cy = 0; cy < local_r; ++cy) {
                    for (int cx = 0; cx < local_r; ++cx) {
                        g_top_corner_mask_lut[cy][cx] = gui_rounded_rect_coverage_local(
                            cx, cy, local_r * 2, local_r * 2, local_r, GUI_ROUNDED_EDGE_TOP);
                    }
                }
                g_cached_top_r = local_r;
            }

            if (local_r <= 64 && row < local_r) {
                for (int col = 0; col < local_r; ++col) {
                    corner_mask[col] = g_top_corner_mask_lut[row][col];
                }
            } else {
                for (int col = 0; col < local_r; ++col) {
                    corner_mask[col] = gui_rounded_rect_coverage_local(col, row, local_r * 2, local_r * 2, local_r,
                                                                       GUI_ROUNDED_EDGE_TOP);
                }
            }
            corner_mask_y = row;
        }

        int left_end = center_start_x < end_x ? center_start_x : end_x;
        for (int px = start_x; px < left_end; ++px) {
            int local = px - x;
            uint8_t coverage = corner_mask[local];
            if (!coverage)
                continue;
            if (coverage == 255 && full_opaque)
                dst_row[px] = color;
            else if (coverage)
                dst_row[px] = blend_rgb(dst_row[px], color, coverage);
        }

        int center_lo = start_x > center_start_x ? start_x : center_start_x;
        int center_hi = end_x < center_end_x ? end_x : center_end_x;
        int center_w = center_hi - center_lo;
        if (full_opaque && center_w > 0) {
            uint32_t *center_dst = &dst_row[center_lo];
            uint32_t v0 = color, v1 = color, v2 = color, v3 = color;
            uint32_t *p = center_dst;
            int i = 0;
            for (; i + 7 < center_w; i += 8) {
                p[0] = v0;
                p[1] = v1;
                p[2] = v2;
                p[3] = v3;
                p[4] = v0;
                p[5] = v1;
                p[6] = v2;
                p[7] = v3;
                p += 8;
            }
            for (; i < center_w; ++i)
                *p++ = v0;
        } else if (center_w > 0) {
            for (int px = center_lo; px < center_hi; ++px)
                dst_row[px] = blend_rgb(dst_row[px], color, base_alpha);
        }

        int right_lo = start_x > center_end_x ? start_x : center_end_x;
        for (int px = right_lo; px < end_x; ++px) {
            int local = w - 1 - (px - x);
            uint8_t coverage = corner_mask[local];
            if (!coverage)
                continue;
            if (coverage == 255 && full_opaque)
                dst_row[px] = color;
            else if (coverage)
                dst_row[px] = blend_rgb(dst_row[px], color, coverage);
        }
    }
}

void paint_desktop_base(Surface *surface)
{
    if (!surface || !surface->buffer || surface->pitch == 0)
        return;

    // Theme-aware fallback gradient (no wallpaper): derived from the desktop
    // background token so dark and light themes stay consistent.
    const uint32_t base = g_gui_chrome.desktop_bg;
    const uint32_t color_top = mix_rgb(base, 0xFF000000u, 40);
    const uint32_t color_mid = base;
    const uint32_t color_bottom = mix_rgb(base, 0xFF000000u, 110);
    const uint32_t stride = surface->pitch / 4u;
    const uint32_t height = surface->height;
    const uint32_t width = surface->width;

    if (height == 0 || width == 0)
        return;

    uint32_t y_mid = (132u * (height - 1u)) / 255u;
    if (y_mid > height)
        y_mid = height;

    {
        int32_t r_fp = static_cast<int32_t>((color_top >> 16) & 0xFFu) << 16;
        int32_t g_fp = static_cast<int32_t>((color_top >> 8) & 0xFFu) << 16;
        int32_t b_fp = static_cast<int32_t>(color_top & 0xFFu) << 16;

        int32_t step_r =
            y_mid > 0
                ? (((static_cast<int32_t>((color_mid >> 16) & 0xFFu) - static_cast<int32_t>((color_top >> 16) & 0xFFu)))
                   << 16) /
                      static_cast<int32_t>(y_mid)
                : 0;
        int32_t step_g =
            y_mid > 0
                ? (((static_cast<int32_t>((color_mid >> 8) & 0xFFu) - static_cast<int32_t>((color_top >> 8) & 0xFFu)))
                   << 16) /
                      static_cast<int32_t>(y_mid)
                : 0;
        int32_t step_b =
            y_mid > 0 ? (((static_cast<int32_t>(color_mid & 0xFFu) - static_cast<int32_t>(color_top & 0xFFu))) << 16) /
                            static_cast<int32_t>(y_mid)
                      : 0;

        for (uint32_t y = 0; y < y_mid; ++y) {
            uint32_t row_color = 0xFF000000u | ((static_cast<uint32_t>(r_fp >> 16) & 0xFFu) << 16) |
                                 ((static_cast<uint32_t>(g_fp >> 16) & 0xFFu) << 8) |
                                 (static_cast<uint32_t>(b_fp >> 16) & 0xFFu);
            uint32_t *row = &surface->buffer[static_cast<size_t>(y) * stride];
            for (uint32_t x = 0; x < width; ++x)
                row[x] = row_color;
            r_fp += step_r;
            g_fp += step_g;
            b_fp += step_b;
        }
    }

    {
        uint32_t h2 = height - y_mid;
        int32_t r_fp = static_cast<int32_t>((color_mid >> 16) & 0xFFu) << 16;
        int32_t g_fp = static_cast<int32_t>((color_mid >> 8) & 0xFFu) << 16;
        int32_t b_fp = static_cast<int32_t>(color_mid & 0xFFu) << 16;

        int32_t step_r = h2 > 1 ? (((static_cast<int32_t>((color_bottom >> 16) & 0xFFu) -
                                     static_cast<int32_t>((color_mid >> 16) & 0xFFu)))
                                   << 16) /
                                      static_cast<int32_t>(h2 - 1u)
                                : 0;
        int32_t step_g = h2 > 1 ? (((static_cast<int32_t>((color_bottom >> 8) & 0xFFu) -
                                     static_cast<int32_t>((color_mid >> 8) & 0xFFu)))
                                   << 16) /
                                      static_cast<int32_t>(h2 - 1u)
                                : 0;
        int32_t step_b =
            h2 > 1 ? (((static_cast<int32_t>(color_bottom & 0xFFu) - static_cast<int32_t>(color_mid & 0xFFu))) << 16) /
                         static_cast<int32_t>(h2 - 1u)
                   : 0;

        for (uint32_t y = y_mid; y < height; ++y) {
            uint32_t row_color = 0xFF000000u | ((static_cast<uint32_t>(r_fp >> 16) & 0xFFu) << 16) |
                                 ((static_cast<uint32_t>(g_fp >> 16) & 0xFFu) << 8) |
                                 (static_cast<uint32_t>(b_fp >> 16) & 0xFFu);
            uint32_t *row = &surface->buffer[static_cast<size_t>(y) * stride];
            for (uint32_t x = 0; x < width; ++x)
                row[x] = row_color;
            r_fp += step_r;
            g_fp += step_g;
            b_fp += step_b;
        }
    }
}

static void publish_wallpaper_state(Registry *registry, uint32_t status, const char *path)
{
    if (!registry)
        return;
    registry->wallpaper_status = status;
    registry->wallpaper_active[0] = '\0';
    if (path && *path) {
        strncpy(registry->wallpaper_active, path, sizeof(registry->wallpaper_active) - 1);
        registry->wallpaper_active[sizeof(registry->wallpaper_active) - 1] = '\0';
    }
    asm volatile("sfence" ::: "memory");
}

static bool apply_wallpaper_image(const char *path, uint32_t theme_mode)
{
    if (!path || !*path)
        return false;
    Surface image = {};
    if (!gui_load_uowp(path, wallpaper_uowp_variant_for_theme(theme_mode), g_screen.width, g_screen.height, &image))
        return false;
    gui_blit_scaled_cover(&g_wallpaper, &image);
    gui_destroy_surface(&image);
    return true;
}

static uint32_t registry_theme_mode(const Registry *registry)
{
    return (registry && registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
}

static void copy_resolved_wallpaper_path(const Registry *registry, const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    const char *resolved = wallpaper_resolve_path_for_theme(path, registry_theme_mode(registry));
    if (!resolved || !*resolved)
        return;
    strncpy(out, resolved, out_size - 1);
    out[out_size - 1] = '\0';
}

void init_wallpaper()
{
    g_wallpaper = gui_create_surface(g_screen.width, g_screen.height);
    paint_desktop_base(&g_wallpaper);
}

void reload_wallpaper(Registry *registry, bool prefer_requested)
{
    uint32_t status = WALLPAPER_STATUS_SOLID;
    char requested[256] = {};
    char configured[256] = {};
    const char *active_path = nullptr;

    paint_desktop_base(&g_wallpaper);
    if (prefer_requested && registry && registry->wallpaper_requested[0]) {
        strncpy(requested, registry->wallpaper_requested, sizeof(requested) - 1);
        requested[sizeof(requested) - 1] = '\0';
    } else {
        VNodeStat st = {};
        const char *config_path = (stat(WALLPAPER_CONFIG_PATH, &st) == 0 && !st.is_dir)
                                      ? WALLPAPER_CONFIG_PATH
                                      : WALLPAPER_BOOTSTRAP_CONFIG_PATH;
        if (cfg_read_first_line(config_path, requested, sizeof(requested)) && registry) {
            strncpy(registry->wallpaper_requested, requested, sizeof(registry->wallpaper_requested) - 1);
            registry->wallpaper_requested[sizeof(registry->wallpaper_requested) - 1] = '\0';
        }
    }
    copy_resolved_wallpaper_path(registry, requested, configured, sizeof(configured));

    uint32_t theme_mode = registry_theme_mode(registry);
    if (configured[0] && apply_wallpaper_image(configured, theme_mode)) {
        status = wallpaper_is_default_family_path(configured) ? WALLPAPER_STATUS_DEFAULT : WALLPAPER_STATUS_CUSTOM;
        active_path = configured;
    } else if (apply_wallpaper_image(wallpaper_default_path_for_theme(theme_mode), theme_mode)) {
        status = WALLPAPER_STATUS_DEFAULT;
        active_path = wallpaper_default_path_for_theme(theme_mode);
    }
    publish_wallpaper_state(registry, status, active_path);
}
