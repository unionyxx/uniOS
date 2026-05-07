#include "wm_core.h"

static Surface g_icon_close = {};
static Surface g_icon_minimize = {};
static Surface g_icon_maximize = {};
static int g_icons_scale = -1;

static void scale_surface_alpha(Surface *s, uint8_t scale)
{
    if (!s || !s->buffer)
        return;
    uint32_t count = s->width * s->height;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t p = s->buffer[i];
        uint8_t a = scale_alpha_u8((uint8_t)(p >> 24), scale);
        uint8_t r = scale_alpha_u8((uint8_t)(p >> 16), scale);
        uint8_t g = scale_alpha_u8((uint8_t)(p >> 8), scale);
        uint8_t b = scale_alpha_u8((uint8_t)p, scale);
        s->buffer[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

static void ensure_button_icons()
{
    int scale = gui_ui_scale_pct();
    if (g_icons_scale == scale && g_icon_close.buffer)
        return;

    if (g_icon_close.buffer)
        gui_destroy_surface(&g_icon_close);
    if (g_icon_minimize.buffer)
        gui_destroy_surface(&g_icon_minimize);
    if (g_icon_maximize.buffer)
        gui_destroy_surface(&g_icon_maximize);

    gui_load_uoic("/usr/share/wm/close.uoic", (uint32_t)BTN_SIZE, (uint32_t)scale, &g_icon_close);
    gui_load_uoic("/usr/share/wm/minimize.uoic", (uint32_t)BTN_SIZE, (uint32_t)scale, &g_icon_minimize);
    gui_load_uoic("/usr/share/wm/maximize.uoic", (uint32_t)BTN_SIZE, (uint32_t)scale, &g_icon_maximize);

    scale_surface_alpha(&g_icon_close, 166);
    scale_surface_alpha(&g_icon_minimize, 166);
    scale_surface_alpha(&g_icon_maximize, 166);

    g_icons_scale = scale;
}

static bool g_menubar_blur_dirty = false;
static bool g_dock_blur_dirty = false;
static uint64_t g_last_blur_vblank = 0;

void invalidate_window_decoration_cache(Window &w)
{
    w.decoration_cache_theme_sig = 0;
    w.button_cache_theme_sig = 0;
    w.decoration_cache_w = 0;
    w.decoration_cache_h = 0;
}

uint32_t mix_rgb(uint32_t a, uint32_t b, uint8_t t)
{
    uint32_t ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    uint32_t r = div255(ar * (255u - t) + br * t);
    uint32_t g = div255(ag * (255u - t) + bg * t);
    uint32_t bl = div255(ab * (255u - t) + bb * t);
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

uint32_t mix_rgb_keep_alpha(uint32_t base, uint32_t tint, uint8_t t)
{
    return (base & 0xFF000000u) | (mix_rgb(base, tint, t) & 0x00FFFFFFu);
}

int color_luma(uint32_t color)
{
    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;
    return (r * 54 + g * 183 + b * 19 + 128) / 256;
}

uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t coverage)
{
    uint8_t src_alpha = scale_alpha_u8((uint8_t)(src >> 24), coverage);
    if (src_alpha == 0)
        return dst;
    uint8_t dst_alpha = (uint8_t)(dst >> 24);
    if (dst_alpha == 0)
        return ((uint32_t)src_alpha << 24) | (src & 0x00FFFFFFu);
    if (src_alpha == 255)
        return 0xFF000000u | (src & 0x00FFFFFFu);

    if (dst_alpha == 255) {
        uint32_t inv = 255u - src_alpha;
        uint32_t dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;
        uint32_t sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
        uint32_t r = div255(dr * inv + sr * src_alpha);
        uint32_t g = div255(dg * inv + sg * src_alpha);
        uint32_t b = div255(db * inv + sb * src_alpha);
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    uint32_t inv = 255u - src_alpha;
    uint32_t out_alpha = (uint32_t)src_alpha + div255((uint32_t)dst_alpha * inv);
    if (out_alpha == 0)
        return 0;
    uint32_t dr_p = ((dst >> 16) & 0xFFu) * dst_alpha, dg_p = ((dst >> 8) & 0xFFu) * dst_alpha,
             db_p = (dst & 0xFFu) * dst_alpha;
    uint32_t sr_p = ((src >> 16) & 0xFFu) * src_alpha, sg_p = ((src >> 8) & 0xFFu) * src_alpha,
             sb_p = (src & 0xFFu) * src_alpha;
    uint32_t r = (sr_p + div255(dr_p * inv) + out_alpha / 2u) / out_alpha;
    uint32_t g = (sg_p + div255(dg_p * inv) + out_alpha / 2u) / out_alpha;
    uint32_t b = (sb_p + div255(db_p * inv) + out_alpha / 2u) / out_alpha;
    return (out_alpha << 24) | (r << 16) | (g << 8) | b;
}

// Blit with alpha blend.
static void blit_alpha_blend_rect(uint32_t *__restrict__ dst, uint32_t dst_stride,
                                  const uint32_t *__restrict__ src, uint32_t src_stride, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    for (int y = 0; y < h; y++) {
        uint32_t *drow = &dst[(size_t)y * dst_stride];
        const uint32_t *srow = &src[(size_t)y * src_stride];
        for (int x = 0; x < w; x++) {
            uint32_t s = srow[x];
            uint32_t sa = s >> 24;
            if (sa == 0)
                continue;
            if (sa == 255) {
                drow[x] = s;
                continue;
            }

            uint32_t d = drow[x];
            uint32_t da = d >> 24;
            // Opaque destination fast path.
            if (da == 255) {
                uint32_t inv_sa = 255u - sa;
                uint32_t s_rb = s & 0x00FF00FFu;
                uint32_t s_g = (s >> 8) & 0xFFu;
                uint32_t d_rb = d & 0x00FF00FFu;
                uint32_t d_g = (d >> 8) & 0xFFu;

                uint32_t rb = s_rb * sa + d_rb * inv_sa + 0x00800080u;
                rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                rb &= 0x00FF00FFu;

                uint32_t g_acc = s_g * sa + d_g * inv_sa + 0x80u;
                uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
                drow[x] = 0xFF000000u | rb | (g << 8);
            } else if (da == 0) {
                // Empty destination: store source directly with its own alpha.
                drow[x] = s;
            } else {
                drow[x] = blend_rgb(d, s, 255);
            }
        }
    }
}

void copy_surface_rect(Surface *dst, int dst_x, int dst_y, const Surface *src, int src_x, int src_y, int w, int h)
{
    if (!dst || !src || !dst->buffer || !src->buffer || dst->pitch == 0 || src->pitch == 0 || w <= 0 || h <= 0)
        return;

    int64_t dx = dst_x;
    int64_t dy = dst_y;
    int64_t sx = src_x;
    int64_t sy = src_y;
    int64_t cw = w;
    int64_t ch = h;

    if (sx < 0) {
        dx -= sx;
        cw += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy;
        ch += sy;
        sy = 0;
    }
    if (dx < 0) {
        sx -= dx;
        cw += dx;
        dx = 0;
    }
    if (dy < 0) {
        sy -= dy;
        ch += dy;
        dy = 0;
    }
    if (cw <= 0 || ch <= 0)
        return;
    if (sx >= src->width || sy >= src->height || dx >= dst->width || dy >= dst->height)
        return;
    if (sx + cw > src->width)
        cw = (int64_t)src->width - sx;
    if (sy + ch > src->height)
        ch = (int64_t)src->height - sy;
    if (dx + cw > dst->width)
        cw = (int64_t)dst->width - dx;
    if (dy + ch > dst->height)
        ch = (int64_t)dst->height - dy;
    if (cw <= 0 || ch <= 0)
        return;

    dst_x = (int)dx;
    dst_y = (int)dy;
    src_x = (int)sx;
    src_y = (int)sy;
    w = (int)cw;
    h = (int)ch;

    uint32_t dst_stride = dst->pitch / 4u;
    uint32_t src_stride = src->pitch / 4u;
    const bool same_buffer = dst->buffer == src->buffer;
    const bool overlap =
        same_buffer && !((dst_x + w) <= src_x || (src_x + w) <= dst_x || (dst_y + h) <= src_y || (src_y + h) <= dst_y);
    int start_y = 0, end_y = h, step_y = 1;
    if (overlap && dst_y > src_y) {
        start_y = h - 1;
        end_y = -1;
        step_y = -1;
    }

    for (int y = start_y; y != end_y; y += step_y) {
        uint32_t *drow = &dst->buffer[(size_t)(dst_y + y) * dst_stride + dst_x];
        const uint32_t *srow = &src->buffer[(size_t)(src_y + y) * src_stride + src_x];
        if (overlap)
            memmove(drow, srow, (size_t)w * sizeof(uint32_t));
        else
            memcpy(drow, srow, (size_t)w * sizeof(uint32_t));
    }
}

bool ensure_surface_capacity(Surface *surface, uint32_t width, uint32_t height)
{
    if (!surface)
        return false;
    if (surface->buffer && surface->width == width && surface->height == height)
        return true;
    gui_destroy_surface(surface);
    *surface = gui_create_surface(width, height);
    return surface->buffer != nullptr;
}

static void compose_desktop_for_blur(Surface *dst, const DirtyRect &clip, int offset_x, int offset_y)
{
    DirtyRect shifted_clip = {clip.x - offset_x, clip.y - offset_y, clip.w, clip.h};

    int start_index = -1;
    bool covered = false;
    for (int i = g_window_count - 1; i >= 2; i--) {
        if (!g_window_visible_cache[i] || g_windows[i].transparent || !g_windows[i].buffer)
            continue;
        DirtyRect outer = window_occlusion_bounds(g_windows[i]);
        if (clip.x >= outer.x && clip.y >= outer.y && clip.x + clip.w <= outer.x + outer.w &&
            clip.y + clip.h <= outer.y + outer.h) {
            start_index = i;
            covered = true;
            break;
        }
    }

    if (!covered)
        gui_blit_rect(dst, &g_wallpaper, shifted_clip.x, shifted_clip.y, clip.x, clip.y, clip.w, clip.h);

    if (start_index < 0)
        start_index = 2;
    if (start_index < 2)
        start_index = 2;

    for (int i = start_index; i < g_window_count; i++) {
        if (!g_window_visible_cache[i] || !g_windows[i].buffer)
            continue;
        if (!dirty_rects_intersect(clip, g_window_outer_cache[i]))
            continue;
        Window local = g_windows[i];
        local.x -= offset_x;
        local.y -= offset_y;
        draw_window_client_clipped(dst, local, shifted_clip);
    }
}

static void mark_shell_blur_dirty(Registry *registry, const DirtyRect &screen_rect)
{
    if (!registry || !g_backbuffer.buffer)
        return;

    DirtyRect menubar_rect = {0, 0, (int)g_screen.width, wm_menubar_h()};
    DirtyRect overlap = {};
    if (g_menubar_blur_source.buffer && rect_intersection(screen_rect, menubar_rect, &overlap)) {
        compose_desktop_for_blur(&g_menubar_blur_source, overlap, 0, 0);
        g_menubar_blur_dirty = true;
    }

    if (g_dock_blur_source.buffer && registry->window_count > 1) {
        DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                               registry->windows[1].h};
        if (clip_dirty_rect_to_screen(dock_rect) && rect_intersection(screen_rect, dock_rect, &overlap)) {
            compose_desktop_for_blur(&g_dock_blur_source, overlap, dock_rect.x, dock_rect.y);
            g_dock_blur_dirty = true;
        }
    }
}

// Fill top-rounded rect.
static void fill_top_rounded_rect_clipped(Surface *dst, int x, int y, int w, int h, int r, uint32_t color)
{
    if (!dst || !dst->buffer || w <= 0 || h <= 0)
        return;
    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h)
        r = h;

    uint8_t base_alpha = (uint8_t)(color >> 24);
    if (base_alpha == 0)
        return;
    const uint32_t pitch = dst->pitch / 4;

    int start_y = y < 0 ? 0 : y;
    int end_y = y + h;
    if (end_y > (int)dst->height)
        end_y = (int)dst->height;
    int start_x = x < 0 ? 0 : x;
    int end_x = x + w;
    if (end_x > (int)dst->width)
        end_x = (int)dst->width;

    // No corner: degenerate to a simple fill. (Below we handle scanline splits.)
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

    for (int py = start_y; py < end_y; py++) {
        const int row = py - y;
        uint32_t *dst_row = &dst->buffer[(size_t)py * pitch];

        // Below the rounded band: solid fill across the row.
        if (py >= top_band_end) {
            if (full_opaque) {
                for (int px = start_x; px < end_x; px++)
                    dst_row[px] = color;
            } else {
                for (int px = start_x; px < end_x; px++)
                    dst_row[px] = blend_rgb(dst_row[px], color, base_alpha);
            }
            continue;
        }

        if (row != corner_mask_y) {
            for (int col = 0; col < local_r; col++) {
                corner_mask[col] = gui_rounded_rect_coverage_local(col, row, w, h, local_r, GUI_ROUNDED_EDGE_TOP);
            }
            corner_mask_y = row;
        }

        // Left rounded corner.
        int left_end = center_start_x < end_x ? center_start_x : end_x;
        for (int px = start_x; px < left_end; px++) {
            int local = px - x;
            if (local < 0 || local >= local_r)
                continue;
            uint8_t coverage = corner_mask[local];
            if (coverage == 0)
                continue;
            uint32_t &dst_px = dst_row[px];
            if (coverage == 255 && full_opaque)
                dst_px = color;
            else
                dst_px = blend_rgb(dst_px, color, coverage);
        }

        // Opaque center.
        int center_lo = start_x > center_start_x ? start_x : center_start_x;
        int center_hi = end_x < center_end_x ? end_x : center_end_x;
        if (full_opaque) {
            for (int px = center_lo; px < center_hi; px++)
                dst_row[px] = color;
        } else {
            for (int px = center_lo; px < center_hi; px++)
                dst_row[px] = blend_rgb(dst_row[px], color, base_alpha);
        }

        // Right rounded corner (mirrored).
        int right_lo = start_x > center_end_x ? start_x : center_end_x;
        for (int px = right_lo; px < end_x; px++) {
            int local = w - 1 - (px - x);
            if (local < 0 || local >= local_r)
                continue;
            uint8_t coverage = corner_mask[local];
            if (coverage == 0)
                continue;
            uint32_t &dst_px = dst_row[px];
            if (coverage == 255 && full_opaque)
                dst_px = color;
            else
                dst_px = blend_rgb(dst_px, color, coverage);
        }
    }
}

void paint_desktop_base(Surface *surface)
{
    if (!surface || !surface->buffer || surface->pitch == 0)
        return;
    const uint32_t color_top = 0xFF0B1533u;
    const uint32_t color_mid = 0xFF2C1F54u;
    const uint32_t color_bottom = 0xFF140A12u;
    const uint32_t stride = surface->pitch / 4u;
    const uint32_t height = surface->height;
    const uint32_t width = surface->width;
    if (height == 0 || width == 0)
        return;

    // Gradient fill.
    for (uint32_t y = 0; y < height; y++) {
        uint8_t t = height > 1 ? (uint8_t)((y * 255u) / (height - 1u)) : 0u;
        uint32_t row_color = t < 132 ? mix_rgb(color_top, color_mid, (uint8_t)((uint32_t)t * 255u / 132u))
                                     : mix_rgb(color_mid, color_bottom, (uint8_t)(((uint32_t)t - 132u) * 255u / 123u));
        uint32_t *row = &surface->buffer[(size_t)y * stride];
        for (uint32_t x = 0; x < width; x++)
            row[x] = row_color;
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
    return registry && registry->theme_mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK;
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

bool init_shell_blur_buffers(Registry *registry, uint32_t dock_w, uint32_t dock_h)
{
    if (!registry || !gui_shm_id_is_valid(registry->mb_blur_shm_id) || !gui_shm_id_is_valid(registry->dk_blur_shm_id))
        return false;

    int menubar_h = wm_menubar_h();
    if (menubar_h <= 0 || dock_w == 0 || dock_h == 0)
        return false;

    uint64_t mb_map = syscall1(SYS_SHM_MAP, (uint64_t)registry->mb_blur_shm_id);
    uint64_t dk_map = syscall1(SYS_SHM_MAP, (uint64_t)registry->dk_blur_shm_id);
    if (mb_map == 0 || mb_map == (uint64_t)-1 || dk_map == 0 || dk_map == (uint64_t)-1) {
        if (mb_map != 0 && mb_map != (uint64_t)-1)
            syscall1(SYS_SHM_UNMAP, (uint64_t)registry->mb_blur_shm_id);
        if (dk_map != 0 && dk_map != (uint64_t)-1)
            syscall1(SYS_SHM_UNMAP, (uint64_t)registry->dk_blur_shm_id);
        return false;
    }

    gui_destroy_surface(&g_menubar_blur_source);
    gui_destroy_surface(&g_dock_blur_source);
    g_menubar_blur_source = gui_create_surface(g_screen.width, (uint32_t)menubar_h);
    g_dock_blur_source = gui_create_surface(dock_w, dock_h);
    if (!g_menubar_blur_source.buffer || !g_dock_blur_source.buffer) {
        gui_destroy_surface(&g_menubar_blur_source);
        gui_destroy_surface(&g_dock_blur_source);
        syscall1(SYS_SHM_UNMAP, (uint64_t)registry->mb_blur_shm_id);
        syscall1(SYS_SHM_UNMAP, (uint64_t)registry->dk_blur_shm_id);
        g_menubar_blur = {};
        g_dock_blur = {};
        return false;
    }

    g_menubar_blur.buffer = reinterpret_cast<uint32_t *>(mb_map);
    g_menubar_blur.width = g_screen.width;
    g_menubar_blur.height = (uint32_t)menubar_h;
    g_menubar_blur.pitch = g_screen.width * 4u;
    g_menubar_blur.owns_buffer = false;

    g_dock_blur.buffer = reinterpret_cast<uint32_t *>(dk_map);
    g_dock_blur.width = dock_w;
    g_dock_blur.height = dock_h;
    g_dock_blur.pitch = dock_w * 4u;
    g_dock_blur.owns_buffer = false;

    memset(g_menubar_blur.buffer, 0, (size_t)g_menubar_blur.pitch * g_menubar_blur.height);
    memset(g_dock_blur.buffer, 0, (size_t)g_dock_blur.pitch * g_dock_blur.height);
    memset(g_menubar_blur_source.buffer, 0, (size_t)g_menubar_blur_source.pitch * g_menubar_blur_source.height);
    memset(g_dock_blur_source.buffer, 0, (size_t)g_dock_blur_source.pitch * g_dock_blur_source.height);

    g_menubar_blur_dirty = true;
    g_dock_blur_dirty = true;
    return true;
}

void capture_shell_backdrop_for_rect(const DirtyRect &rect, Registry *registry)
{
    mark_shell_blur_dirty(registry, rect);
}

void flush_shell_blur_updates(Registry *registry)
{
    if (!registry)
        return;
    if (registry->transparency_level >= 255) {
        registry->mb_blur_generation = 0;
        registry->dk_blur_generation = 0;
        g_menubar_blur_dirty = false;
        g_dock_blur_dirty = false;
        asm volatile("sfence" ::: "memory");
        return;
    }

    bool active_drag = g_input.pointer_down && g_input.drag_index >= 2;
    bool hover_resize = g_input.hover_resize_edges != RESIZE_NONE;
    if (active_drag || hover_resize)
        return;

    g_last_blur_vblank = g_display_queue.vblank_count;

    bool is_light = registry->theme_mode == GUI_THEME_LIGHT;

    if (g_menubar_blur_dirty && g_menubar_blur.buffer && g_menubar_blur_source.buffer) {
        blur_surface_material(&g_menubar_blur_source, &g_menubar_blur, 48.0f, is_light ? 85 : 80, is_light ? 8 : 12);
        registry->mb_blur_generation = registry->mb_blur_generation + 1u;
        g_menubar_blur_dirty = false;
    }
    if (g_dock_blur_dirty && g_dock_blur.buffer && g_dock_blur_source.buffer) {
        blur_surface_material(&g_dock_blur_source, &g_dock_blur, 36.0f, is_light ? 82 : 78, is_light ? 8 : 10);
        registry->dk_blur_generation = registry->dk_blur_generation + 1u;
        g_dock_blur_dirty = false;
    }
    asm volatile("sfence" ::: "memory");
}

static uint32_t window_decoration_theme_signature()
{
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t value) {
        sig ^= value;
        sig *= 16777619u;
    };
    mix(g_gui_style.border);
    mix(g_gui_style.border_focus);
    mix(g_gui_style.border_hover);
    mix(g_gui_chrome.window_bar_active);
    mix(g_gui_chrome.window_bar_inactive);
    mix(g_gui_chrome.window_bar_hover);
    mix(g_gui_chrome.window_title_active);
    mix(g_gui_chrome.window_title_inactive);
    mix(g_gui_chrome.frame_shadow);
    mix(g_gui_chrome.frame_outline);
    mix((uint32_t)wm_title_bar_h());
    mix((uint32_t)wm_button_size());
    mix((uint32_t)wm_button_inset_x());
    mix((uint32_t)wm_button_inset_y());
    mix((uint32_t)wm_button_spacing());
    mix((uint32_t)gui_scaled_metric(12));
    mix((uint32_t)wm_frame_border());
    mix((uint32_t)wm_frame_shadow_offset_x());
    mix((uint32_t)wm_frame_shadow_offset_y());
    mix((uint32_t)gui_scaled_metric(1));
    return sig;
}

static void draw_window_decoration_frame(Surface *dst, const Window &w, const DirtyRect &clip, bool focused)
{
    if (w.transparent)
        return;

    int title_bar_h = wm_title_bar_h();
    int space_1 = gui_space_1();
    int space_2 = gui_space_2();
    int border = wm_frame_border();
    int detail_inset = gui_scaled_metric(1);
    if (detail_inset < 1)
        detail_inset = 1;
    int radius = gui_radius_xl();
    int body_inset = border + detail_inset;
    int frame_radius = radius - border;
    if (frame_radius < 0)
        frame_radius = 0;
    int body_radius = radius - body_inset;
    if (body_radius < 0)
        body_radius = 0;

    uint32_t outline_color = focused ? g_gui_style.border_hover : g_gui_style.border;
    uint32_t bar_color = focused ? g_gui_chrome.window_bar_active : g_gui_chrome.window_bar_inactive;
    uint32_t body_color = g_gui_style.app_bg;
    uint32_t title_color = focused ? g_gui_chrome.window_title_active : g_gui_chrome.window_title_inactive;
    uint32_t frame_fill_color = mix_rgb(outline_color, body_color, focused ? 236 : 242);
    uint32_t inner_stroke_color = mix_rgb(body_color, 0xFFFFFFFFu, focused ? 18 : 12);
    uint32_t separator_color = mix_rgb(outline_color, body_color, focused ? 172 : 190);

    int lx = (dst->buffer != g_backbuffer.buffer) ? 0 : w.x;
    int ly = (dst->buffer != g_backbuffer.buffer) ? 0 : w.y - title_bar_h;
    int sx = lx;
    int sy = ly;
    int sw = w.w;
    int sh = w.h + title_bar_h;

    int shadow_offset = focused ? gui_scaled_metric(3) : gui_scaled_metric(2);
    uint32_t shadow_color = focused ? 0x20000000u : 0x12000000u;
    gui_fill_rounded_rect(dst, sx, sy + shadow_offset, sw, sh, radius + gui_scaled_metric(1), shadow_color);

    gui_fill_rounded_rect(dst, sx, sy, sw, sh, radius, outline_color);
    if (sw > border * 2 && sh > border * 2) {
        gui_fill_rounded_rect(dst, sx + border, sy + border, sw - border * 2, sh - border * 2, frame_radius,
                              frame_fill_color);
    }
    if (sw > body_inset * 2 && sh > body_inset * 2) {
        gui_fill_rounded_rect(dst, sx + body_inset, sy + body_inset, sw - body_inset * 2, sh - body_inset * 2,
                              body_radius, body_color);
        gui_draw_rounded_rect(dst, sx + border, sy + border, sw - border * 2, sh - border * 2, frame_radius,
                              inner_stroke_color);
    }

    int title_fill_x = sx + border;
    int title_fill_y = sy + border;
    int title_fill_w = sw - border * 2;
    int title_fill_h = title_bar_h;
    if (title_fill_w > 0 && title_fill_h > 0) {
        int title_radius = radius - border;
        if (title_radius < 0)
            title_radius = 0;
        if (title_radius > title_fill_w / 2)
            title_radius = title_fill_w / 2;
        if (title_radius > title_fill_h)
            title_radius = title_fill_h;
        fill_top_rounded_rect_clipped(dst, title_fill_x, title_fill_y, title_fill_w, title_fill_h, title_radius,
                                      bar_color);
        gui_fill_rect(dst, title_fill_x, sy + title_bar_h, title_fill_w, detail_inset, separator_color);
    }

    const GuiFont *title_font = gui_font_title();
    int title_h = gui_font_line_height(title_font);
    int title_y = gui_align_text_y(title_font, sy, title_bar_h);
    DirtyRect last_button = window_button_bounds(w, 2);
    int buttons_right = last_button.x + last_button.w;
    int title_left = buttons_right + space_1;
    int title_right = sx + w.w - space_2;
    int available_w = title_right - title_left;
    if (available_w > 0) {
        int raw_title_w = gui_measure_text(title_font, w.title);
        int centered_x = sx + (w.w - raw_title_w) / 2;
        if (centered_x < title_left)
            centered_x = title_left;
        if (centered_x + raw_title_w > title_right)
            centered_x = title_right - raw_title_w;

        int ix, iy, iw, ih;
        if (gui_intersect_rect(clip.x, clip.y, clip.w, clip.h, centered_x, title_y, available_w, title_h, &ix, &iy, &iw,
                               &ih)) {
            gui_draw_text_rect_clipped(dst, title_font, centered_x, title_y, available_w, clip.x, clip.y, clip.w,
                                       clip.h, w.title, title_color, bar_color);
        }
    }
}

static void draw_window_decoration_buttons(Surface *dst, const Window &w, bool focused, int hovered_button)
{
    if (w.transparent)
        return;

    ensure_button_icons();

    uint32_t bar_color = focused ? g_gui_chrome.window_bar_active : g_gui_chrome.window_bar_inactive;
    uint32_t button_colors[3] = {g_gui_chrome.button_close, g_gui_chrome.button_minimize, g_gui_chrome.button_maximize};
    uint32_t button_outline = focused ? 0x65000000u : 0x38000000u;
    int button_size = wm_button_size();

    DirtyRect b0 = window_button_bounds(w, 0);
    int offset_x = b0.x;
    int offset_y = b0.y;

    Surface *icons[3] = {&g_icon_close, &g_icon_minimize, &g_icon_maximize};

    for (int i = 0; i < 3; i++) {
        int cx = 0, cy = 0;
        window_button_center(w, i, &cx, &cy);
        cx -= offset_x;
        cy -= offset_y;
        int r = button_size / 2;
        uint32_t button_fill = focused ? button_colors[i] : mix_rgb(button_colors[i], bar_color, 138);
        if (hovered_button == i)
            button_fill = 0xFF000000u | (mix_rgb(button_colors[i], 0xFFFFFFFFu, focused ? 22 : 16) & 0x00FFFFFFu);
        gui_fill_circle(dst, cx, cy, r, button_fill);
        gui_draw_circle_stroke(dst, cx, cy, r, 1, button_outline);

        if (hovered_button >= 0 && icons[i]->buffer) {
            int ix = cx - (int)icons[i]->width / 2;
            int iy = cy - (int)icons[i]->height / 2;
            gui_blit_alpha(dst, icons[i], ix, iy);
        }
    }
}

static void ensure_window_decoration_cache(Window &w, bool focused, bool hovered_frame, int hovered_button)
{
    (void)hovered_frame;
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    uint32_t theme_sig = window_decoration_theme_signature();

    bool frame_needs_rebuild = !w.decoration_cache.buffer || w.decoration_cache_w != outer.w ||
                               w.decoration_cache_h != outer.h || w.decoration_cache_theme_sig != theme_sig ||
                               w.decoration_cache_focused != focused || strcmp(w.decoration_cache_title, w.title) != 0;

    if (frame_needs_rebuild) {
        bool needs_alloc =
            !w.decoration_cache.buffer || outer.w > w.decoration_cache_alloc_w || outer.h > w.decoration_cache_alloc_h;

        if (needs_alloc) {
            gui_destroy_surface(&w.decoration_cache);
            int aw = (outer.w + 63) & ~63;
            int ah = (outer.h + 31) & ~31;
            w.decoration_cache = gui_create_surface((uint32_t)aw, (uint32_t)ah);
            w.decoration_cache_alloc_w = aw;
            w.decoration_cache_alloc_h = ah;
        }

        w.decoration_cache_w = outer.w;
        w.decoration_cache_h = outer.h;

        if (w.decoration_cache.buffer) {
            Surface view = w.decoration_cache;
            view.width = outer.w;
            view.height = outer.h;
            gui_fill_rect(&view, 0, 0, outer.w, outer.h, 0);

            Window local = w;
            local.x = 0;
            local.y = wm_title_bar_h();
            DirtyRect full = {0, 0, outer.w, outer.h};
            draw_window_decoration_frame(&view, local, full, focused);

            w.decoration_cache_theme_sig = theme_sig;
            w.decoration_cache_focused = focused;
            strncpy(w.decoration_cache_title, w.title, sizeof(w.decoration_cache_title) - 1);
            w.decoration_cache_title[sizeof(w.decoration_cache_title) - 1] = '\0';
        }
    }

    DirtyRect b0 = window_button_bounds(w, 0);
    DirtyRect b2 = window_button_bounds(w, 2);
    int buttons_w = (b2.x + b2.w) - b0.x;
    int buttons_h = b0.h;

    bool buttons_needs_rebuild = !w.button_cache.buffer || w.button_cache_w != buttons_w ||
                                 w.button_cache_h != buttons_h || w.button_cache_theme_sig != theme_sig ||
                                 w.button_cache_focused != focused || w.button_cache_hovered_button != hovered_button;

    if (buttons_needs_rebuild) {
        bool needs_alloc =
            !w.button_cache.buffer || buttons_w > w.button_cache_alloc_w || buttons_h > w.button_cache_alloc_h;
        if (needs_alloc) {
            gui_destroy_surface(&w.button_cache);
            int aw = (buttons_w + 15) & ~15;
            int ah = (buttons_h + 15) & ~15;
            w.button_cache = gui_create_surface((uint32_t)aw, (uint32_t)ah);
            w.button_cache_alloc_w = aw;
            w.button_cache_alloc_h = ah;
        }
        w.button_cache_w = buttons_w;
        w.button_cache_h = buttons_h;

        if (w.button_cache.buffer) {
            Surface view = w.button_cache;
            view.width = buttons_w;
            view.height = buttons_h;
            gui_fill_rect(&view, 0, 0, buttons_w, buttons_h, 0);
            draw_window_decoration_buttons(&view, w, focused, hovered_button);
            w.button_cache_theme_sig = theme_sig;
            w.button_cache_focused = focused;
            w.button_cache_hovered_button = hovered_button;
        }
    }
}

void draw_window_decoration_clipped(Surface *dst, Window &w, const DirtyRect &clip, bool focused, bool hovered_frame,
                                    int hovered_button)
{
    if (!dst || !dst->buffer || w.transparent)
        return;
    ensure_window_decoration_cache(w, focused, hovered_frame, hovered_button);

    DirtyRect outer = window_outer_bounds(w);

    if (w.decoration_cache.buffer) {
        DirtyRect visible = {};
        if (rect_intersection(outer, clip, &visible)) {
            int src_x = visible.x - outer.x, src_y = visible.y - outer.y;
            uint32_t cache_stride = w.decoration_cache.pitch / 4;
            blit_alpha_blend_rect(&dst->buffer[(size_t)visible.y * (dst->pitch / 4) + visible.x], dst->pitch / 4,
                                  &w.decoration_cache.buffer[(size_t)src_y * cache_stride + src_x], cache_stride,
                                  visible.w, visible.h);
        }
    }

    if (w.button_cache.buffer) {
        DirtyRect b0 = window_button_bounds(w, 0);
        DirtyRect buttons_rect = {b0.x, b0.y, w.button_cache_w, w.button_cache_h};
        DirtyRect visible = {};
        if (rect_intersection(buttons_rect, clip, &visible)) {
            int src_x = visible.x - buttons_rect.x, src_y = visible.y - buttons_rect.y;
            uint32_t cache_stride = w.button_cache.pitch / 4;
            blit_alpha_blend_rect(&dst->buffer[(size_t)visible.y * (dst->pitch / 4) + visible.x], dst->pitch / 4,
                                  &w.button_cache.buffer[(size_t)src_y * cache_stride + src_x], cache_stride, visible.w,
                                  visible.h);
        }
    }
}

static inline uint32_t blend_coverage_rgb(uint32_t dst_px, uint32_t src_px, uint8_t coverage)
{
    if (coverage == 0)
        return dst_px;
    if (coverage == 255)
        return src_px;
    uint32_t inv = 255u - coverage;
    uint32_t dr = (dst_px >> 16) & 0xFFu;
    uint32_t dg = (dst_px >> 8) & 0xFFu;
    uint32_t db = dst_px & 0xFFu;
    uint32_t sr = (src_px >> 16) & 0xFFu;
    uint32_t sg = (src_px >> 8) & 0xFFu;
    uint32_t sb = src_px & 0xFFu;
    uint32_t r = (sr * coverage + dr * inv + 127u) / 255u;
    uint32_t g = (sg * coverage + dg * inv + 127u) / 255u;
    uint32_t b = (sb * coverage + db * inv + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Compute bottom corner row coverage.
static void compute_bottom_corner_row(int local_y, int inner_w, int inner_h, int inner_r, uint8_t *out_mask)
{
    if (inner_r <= 0 || !out_mask)
        return;
    for (int col = 0; col < inner_r; col++) {
        out_mask[col] =
            gui_rounded_rect_coverage_local(col, local_y, inner_w, inner_h, inner_r, GUI_ROUNDED_EDGE_BOTTOM);
    }
}

void draw_window_client_clipped(Surface *dst, const Window &w, const DirtyRect &clip)
{
    int ix, iy, iw, ih;
    if (!dst || !dst->buffer ||
        !gui_intersect_rect(w.x, w.y, w.w, w.h, clip.x, clip.y, clip.w, clip.h, &ix, &iy, &iw, &ih))
        return;

    const uint32_t dst_stride = dst->pitch / 4;
    int radius = gui_radius_xl();
    int border = wm_frame_border();
    int detail_inset = gui_scaled_metric(1);
    if (detail_inset < 1)
        detail_inset = 1;
    int body_inset = border + detail_inset;
    int inner_r = radius - body_inset;
    int inner_left = w.x + border;
    int inner_top = w.y;
    int inner_w = w.w - border * 2;
    int inner_h = w.h - border;
    if (inner_w <= 0 || inner_h <= 0) {
        inner_left = w.x;
        inner_top = w.y;
        inner_w = w.w;
        inner_h = w.h;
        inner_r = 0;
    }
    if (inner_r > inner_w / 2)
        inner_r = inner_w / 2;
    if (inner_r > inner_h / 2)
        inner_r = inner_h / 2;
    if (inner_r < 0)
        inner_r = 0;

    // Per-row corner mask.
    static constexpr int kCornerMaskMax = 64;
    if (inner_r > kCornerMaskMax)
        inner_r = kCornerMaskMax;

    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (!gui_intersect_rect(ix, iy, iw, ih, inner_left, inner_top, inner_w, inner_h, &rx, &ry, &rw, &rh))
        return;

    const int rounded_start_y = inner_top + inner_h - inner_r;
    const int center_start_x = inner_left + inner_r;
    const int center_end_x = inner_left + inner_w - inner_r;
    const int dst_height_int = (int)dst->height;

    uint8_t corner_mask[kCornerMaskMax];
    int corner_mask_y = -1;
    auto refresh_corner_mask = [&](int local_y) {
        if (inner_r <= 0 || local_y == corner_mask_y)
            return;
        compute_bottom_corner_row(local_y, inner_w, inner_h, inner_r, corner_mask);
        corner_mask_y = local_y;
    };

    if (!w.transparent) {
        // Repair visible region.
        const uint32_t fill = g_gui_style.app_bg ? g_gui_style.app_bg : 0xFF15171Au;
        if (inner_r <= 0) {
            gui_fill_rect(dst, rx, ry, rw, rh, fill);
        } else {
            const int rect_right = rx + rw;
            for (int py = 0; py < rh; py++) {
                const int dst_y = ry + py;
                if (dst_y < 0 || dst_y >= dst_height_int)
                    continue;
                uint32_t *dst_ptr = &dst->buffer[(size_t)dst_y * dst_stride];
                if (dst_y < rounded_start_y) {
                    for (int x = rx; x < rect_right; x++)
                        dst_ptr[x] = fill;
                    continue;
                }

                refresh_corner_mask(dst_y - inner_top);

                // Left rounded corner: x in [inner_left, center_start_x)
                int left_end = center_start_x < rect_right ? center_start_x : rect_right;
                for (int x = rx; x < left_end; x++) {
                    int local = x - inner_left;
                    if (local < 0 || local >= inner_r)
                        continue;
                    uint8_t coverage = corner_mask[local];
                    if (coverage == 255)
                        dst_ptr[x] = fill;
                    else if (coverage > 0)
                        dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                }

                // Center: opaque, single fill loop.
                int center_lo = rx > center_start_x ? rx : center_start_x;
                int center_hi = rect_right < center_end_x ? rect_right : center_end_x;
                for (int x = center_lo; x < center_hi; x++)
                    dst_ptr[x] = fill;

                // Right rounded corner: mirror of left mask.
                int right_lo = rx > center_end_x ? rx : center_end_x;
                for (int x = right_lo; x < rect_right; x++) {
                    int local = inner_w - 1 - (x - inner_left);
                    if (local < 0 || local >= inner_r)
                        continue;
                    uint8_t coverage = corner_mask[local];
                    if (coverage == 255)
                        dst_ptr[x] = fill;
                    else if (coverage > 0)
                        dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                }
            }
        }
    }

    if (w.buffer_w <= 0 || w.buffer_h <= 0 || !w.buffer)
        return;

    int copy_x = w.transparent ? ix : rx;
    int copy_y = w.transparent ? iy : ry;
    int copy_w = w.transparent ? iw : rw;
    int copy_h = w.transparent ? ih : rh;
    int src_x = copy_x - w.x + w.scroll_x;
    int src_y = copy_y - w.y + w.scroll_y;

    if (src_x < 0) {
        int delta = -src_x;
        copy_x += delta;
        copy_w -= delta;
        src_x = 0;
    }
    if (src_y < 0) {
        int delta = -src_y;
        copy_y += delta;
        copy_h -= delta;
        src_y = 0;
    }
    if (src_x + copy_w > w.buffer_w)
        copy_w = w.buffer_w - src_x;
    if (src_y + copy_h > w.buffer_h)
        copy_h = w.buffer_h - src_y;
    if (copy_w <= 0 || copy_h <= 0)
        return;

    if (w.transparent) {
        blit_alpha_blend_rect(&dst->buffer[(size_t)copy_y * dst_stride + copy_x], dst_stride,
                              &w.buffer[(size_t)src_y * w.buffer_w + src_x], w.buffer_w, copy_w, copy_h);
        return;
    }

    if (inner_r <= 0) {
        Surface src_surface = {w.buffer, (uint32_t)w.buffer_w, (uint32_t)w.buffer_h, (uint32_t)w.buffer_w * 4, false,
                               0};
        copy_surface_rect(dst, copy_x, copy_y, &src_surface, src_x, src_y, copy_w, copy_h);
        return;
    }

    // Body copy.
    const int copy_right = copy_x + copy_w;
    for (int py = 0; py < copy_h; py++) {
        const int dst_y = copy_y + py;
        const int src_row_base = src_y + py;
        if (dst_y < 0 || dst_y >= dst_height_int || src_row_base < 0 || src_row_base >= w.buffer_h)
            continue;

        uint32_t *dst_ptr = &dst->buffer[(size_t)dst_y * dst_stride];
        const uint32_t *src_ptr = &w.buffer[(size_t)src_row_base * w.buffer_w];

        if (dst_y < rounded_start_y) {
            memcpy(&dst_ptr[copy_x], &src_ptr[src_x], (size_t)copy_w * sizeof(uint32_t));
            continue;
        }

        refresh_corner_mask(dst_y - inner_top);

        // Left rounded corner.
        int left_end = center_start_x < copy_right ? center_start_x : copy_right;
        for (int x = copy_x; x < left_end; x++) {
            int local = x - inner_left;
            if (local < 0 || local >= inner_r)
                continue;
            int src_col = src_x + (x - copy_x);
            if ((unsigned)src_col >= (unsigned)w.buffer_w)
                continue;
            uint8_t coverage = corner_mask[local];
            if (coverage == 255)
                dst_ptr[x] = src_ptr[src_col];
            else if (coverage > 0)
                dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], src_ptr[src_col], coverage);
        }

        // Opaque center: one memcpy.
        int center_lo = copy_x > center_start_x ? copy_x : center_start_x;
        int center_hi = copy_right < center_end_x ? copy_right : center_end_x;
        if (center_hi > center_lo) {
            int src_col_start = src_x + (center_lo - copy_x);
            int center_w = center_hi - center_lo;
            if (src_col_start < 0) {
                center_lo += -src_col_start;
                center_w -= -src_col_start;
                src_col_start = 0;
            }
            if (src_col_start + center_w > w.buffer_w)
                center_w = w.buffer_w - src_col_start;
            if (center_w > 0)
                memcpy(&dst_ptr[center_lo], &src_ptr[src_col_start], (size_t)center_w * sizeof(uint32_t));
        }

        // Right rounded corner.
        int right_lo = copy_x > center_end_x ? copy_x : center_end_x;
        for (int x = right_lo; x < copy_right; x++) {
            int local = inner_w - 1 - (x - inner_left);
            if (local < 0 || local >= inner_r)
                continue;
            int src_col = src_x + (x - copy_x);
            if ((unsigned)src_col >= (unsigned)w.buffer_w)
                continue;
            uint8_t coverage = corner_mask[local];
            if (coverage == 255)
                dst_ptr[x] = src_ptr[src_col];
            else if (coverage > 0)
                dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], src_ptr[src_col], coverage);
        }
    }
}
