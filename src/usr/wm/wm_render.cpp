#include "wm_core.h"

static inline uint32_t div255(uint32_t x)
{
    return (x + 128u + ((x + 128u) >> 8)) >> 8;
}

static uint8_t scale_alpha_u8(uint8_t alpha, uint8_t coverage)
{
    return (uint8_t)div255((uint32_t)alpha * (uint32_t)coverage);
}

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

static void draw_storage_prompt_overlay_clipped(const DirtyRect &clip);

static bool g_menubar_blur_dirty = false;
static bool g_dock_blur_dirty = false;
static uint64_t g_last_blur_vblank = 0;
static Surface g_blur_scratch = {};
static Surface g_blur_pass_a = {};
static Surface g_blur_pass_b = {};
static Surface g_blur_small_src = {};
static Surface g_blur_small_dst = {};

static uint32_t mix_rgb(uint32_t a, uint32_t b, uint8_t t)
{
    uint32_t ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    uint32_t r = div255(ar * (255u - t) + br * t);
    uint32_t g = div255(ag * (255u - t) + bg * t);
    uint32_t bl = div255(ab * (255u - t) + bb * t);
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static uint32_t mix_rgb_keep_alpha(uint32_t base, uint32_t tint, uint8_t t)
{
    return (base & 0xFF000000u) | (mix_rgb(base, tint, t) & 0x00FFFFFFu);
}

static int color_luma(uint32_t color)
{
    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;
    return (r * 54 + g * 183 + b * 19 + 128) / 256;
}

static uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t coverage)
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

static void blit_alpha_blend_rect(uint32_t *dst, uint32_t dst_stride, const uint32_t *src, uint32_t src_stride, int w,
                                  int h)
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
            if (da == 255) {
                uint32_t inv_sa = 255u - sa;
                uint32_t s_rb = s & 0x00FF00FFu;
                uint32_t s_g = (s >> 8) & 0xFFu;
                uint32_t d_rb = d & 0x00FF00FFu;
                uint32_t d_g = (d >> 8) & 0xFFu;

                uint32_t rb = (s_rb * sa + d_rb * inv_sa + 0x00800080u);
                rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                rb &= 0x00FF00FFu;

                uint32_t g = (s_g * sa + d_g * inv_sa + 127u) / 255u;
                drow[x] = 0xFF000000u | rb | (g << 8);
            } else {
                drow[x] = blend_rgb(d, s, 255);
            }
        }
    }
}

void invalidate_window_decoration_cache(Window &w)
{
    w.decoration_cache_theme_sig = 0;
    w.button_cache_theme_sig = 0;
    w.decoration_cache_w = 0;
    w.decoration_cache_h = 0;
}

static void copy_surface_rect(Surface *dst, int dst_x, int dst_y, const Surface *src, int src_x, int src_y, int w,
                              int h)
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

static bool ensure_surface_capacity(Surface *surface, uint32_t width, uint32_t height)
{
    if (!surface)
        return false;
    if (surface->buffer && surface->width == width && surface->height == height)
        return true;
    gui_destroy_surface(surface);
    *surface = gui_create_surface(width, height);
    return surface->buffer != nullptr;
}

static inline uint8_t clamp_material_channel(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void blur_surface_box(const Surface *src, Surface *dst, int radius)
{
    if (!src || !dst || !src->buffer || !dst->buffer || src->width != dst->width || src->height != dst->height)
        return;
    uint32_t w = src->width;
    uint32_t h = src->height;
    if (w == 0 || h == 0)
        return;
    if (radius <= 0) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }
    if (!ensure_surface_capacity(&g_blur_scratch, w, h)) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }

    const uint32_t src_stride = src->pitch / 4u;
    const uint32_t tmp_stride = g_blur_scratch.pitch / 4u;
    const uint32_t dst_stride = dst->pitch / 4u;
    const uint32_t window = (uint32_t)radius * 2u + 1u;

    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src_row = &src->buffer[(size_t)y * src_stride];
        uint32_t *tmp_row = &g_blur_scratch.buffer[(size_t)y * tmp_stride];

        uint32_t sum_r = 0;
        uint32_t sum_g = 0;
        uint32_t sum_b = 0;
        for (int sample = -radius; sample <= radius; sample++) {
            int clamped = sample;
            if (clamped < 0)
                clamped = 0;
            if (clamped >= (int)w)
                clamped = (int)w - 1;
            uint32_t pixel = src_row[clamped];
            sum_r += (pixel >> 16) & 0xFFu;
            sum_g += (pixel >> 8) & 0xFFu;
            sum_b += pixel & 0xFFu;
        }
        for (uint32_t x = 0; x < w; x++) {
            tmp_row[x] = 0xFF000000u | ((sum_r / window) << 16) | ((sum_g / window) << 8) | (sum_b / window);
            int remove_index = (int)x - radius;
            int add_index = (int)x + radius + 1;
            if (remove_index < 0)
                remove_index = 0;
            if (add_index >= (int)w)
                add_index = (int)w - 1;
            uint32_t remove_pixel = src_row[remove_index];
            uint32_t add_pixel = src_row[add_index];
            sum_r += ((add_pixel >> 16) & 0xFFu) - ((remove_pixel >> 16) & 0xFFu);
            sum_g += ((add_pixel >> 8) & 0xFFu) - ((remove_pixel >> 8) & 0xFFu);
            sum_b += (add_pixel & 0xFFu) - (remove_pixel & 0xFFu);
        }
    }

    static constexpr uint32_t BLUR_TILE_W = 8u;
    for (uint32_t x0 = 0; x0 < w; x0 += BLUR_TILE_W) {
        uint32_t x_end = x0 + BLUR_TILE_W < w ? x0 + BLUR_TILE_W : w;
        for (uint32_t x = x0; x < x_end; x++) {
            uint32_t sum_r = 0;
            uint32_t sum_g = 0;
            uint32_t sum_b = 0;
            for (int sample = -radius; sample <= radius; sample++) {
                int clamped = sample;
                if (clamped < 0)
                    clamped = 0;
                if (clamped >= (int)h)
                    clamped = (int)h - 1;
                uint32_t pixel = g_blur_scratch.buffer[(size_t)clamped * tmp_stride + x];
                sum_r += (pixel >> 16) & 0xFFu;
                sum_g += (pixel >> 8) & 0xFFu;
                sum_b += pixel & 0xFFu;
            }
            for (uint32_t y = 0; y < h; y++) {
                dst->buffer[(size_t)y * dst_stride + x] =
                    0xFF000000u | ((sum_r / window) << 16) | ((sum_g / window) << 8) | (sum_b / window);
                int remove_index = (int)y - radius;
                int add_index = (int)y + radius + 1;
                if (remove_index < 0)
                    remove_index = 0;
                if (add_index >= (int)h)
                    add_index = (int)h - 1;
                uint32_t remove_pixel = g_blur_scratch.buffer[(size_t)remove_index * tmp_stride + x];
                uint32_t add_pixel = g_blur_scratch.buffer[(size_t)add_index * tmp_stride + x];
                sum_r += ((add_pixel >> 16) & 0xFFu) - ((remove_pixel >> 16) & 0xFFu);
                sum_g += ((add_pixel >> 8) & 0xFFu) - ((remove_pixel >> 8) & 0xFFu);
                sum_b += (add_pixel & 0xFFu) - (remove_pixel & 0xFFu);
            }
        }
    }
}

static int round_float_to_int(float value)
{
    return value >= 0.0f ? (int)(value + 0.5f) : (int)(value - 0.5f);
}

static void compute_gaussian_box_radii(float sigma, int radii[3])
{
    if (!radii)
        return;
    if (sigma <= 0.0f) {
        radii[0] = radii[1] = radii[2] = 0;
        return;
    }

    const int passes = 3;
    float width_ideal = wm_sqrtf((12.0f * sigma * sigma / (float)passes) + 1.0f);
    int lower_width = (int)width_ideal;
    if ((lower_width & 1) == 0)
        lower_width--;
    if (lower_width < 1)
        lower_width = 1;
    int upper_width = lower_width + 2;

    float lw = (float)lower_width;
    float numerator =
        12.0f * sigma * sigma - (float)passes * lw * lw - 4.0f * (float)passes * lw - 3.0f * (float)passes;
    float denominator = -4.0f * lw - 4.0f;
    int lower_count = round_float_to_int(numerator / denominator);
    if (lower_count < 0)
        lower_count = 0;
    if (lower_count > passes)
        lower_count = passes;

    for (int i = 0; i < passes; i++) {
        int width = i < lower_count ? lower_width : upper_width;
        radii[i] = (width - 1) / 2;
    }
}

static void blur_surface_box_fused(const Surface *src, Surface *dst, int radius, int saturation_pct,
                                   int brightness_bias)
{
    if (!src || !dst || !src->buffer || !dst->buffer || src->width != dst->width || src->height != dst->height)
        return;
    uint32_t w = src->width;
    uint32_t h = src->height;
    if (w == 0 || h == 0)
        return;
    if (radius <= 0) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }
    if (!ensure_surface_capacity(&g_blur_scratch, w, h)) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }

    const uint32_t src_stride = src->pitch / 4u;
    const uint32_t tmp_stride = g_blur_scratch.pitch / 4u;
    const uint32_t dst_stride = dst->pitch / 4u;
    const uint32_t window = (uint32_t)radius * 2u + 1u;

    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src_row = &src->buffer[(size_t)y * src_stride];
        uint32_t *tmp_row = &g_blur_scratch.buffer[(size_t)y * tmp_stride];
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
        for (int sample = -radius; sample <= radius; sample++) {
            int clamped = sample < 0 ? 0 : (sample >= (int)w ? (int)w - 1 : sample);
            uint32_t pixel = src_row[clamped];
            sum_r += (pixel >> 16) & 0xFFu;
            sum_g += (pixel >> 8) & 0xFFu;
            sum_b += pixel & 0xFFu;
        }
        for (uint32_t x = 0; x < w; x++) {
            tmp_row[x] = 0xFF000000u | ((sum_r / window) << 16) | ((sum_g / window) << 8) | (sum_b / window);
            int ri = (int)x - radius, ai = (int)x + radius + 1;
            if (ri < 0)
                ri = 0;
            if (ai >= (int)w)
                ai = (int)w - 1;
            uint32_t rp = src_row[ri], ap = src_row[ai];
            sum_r += ((ap >> 16) & 0xFFu) - ((rp >> 16) & 0xFFu);
            sum_g += ((ap >> 8) & 0xFFu) - ((rp >> 8) & 0xFFu);
            sum_b += (ap & 0xFFu) - (rp & 0xFFu);
        }
    }

    static constexpr uint32_t BLUR_TILE_W = 8u;
    for (uint32_t x0 = 0; x0 < w; x0 += BLUR_TILE_W) {
        uint32_t x_end = x0 + BLUR_TILE_W < w ? x0 + BLUR_TILE_W : w;
        for (uint32_t x = x0; x < x_end; x++) {
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
            for (int sample = -radius; sample <= radius; sample++) {
                int clamped = sample < 0 ? 0 : (sample >= (int)h ? (int)h - 1 : sample);
                uint32_t pixel = g_blur_scratch.buffer[(size_t)clamped * tmp_stride + x];
                sum_r += (pixel >> 16) & 0xFFu;
                sum_g += (pixel >> 8) & 0xFFu;
                sum_b += pixel & 0xFFu;
            }
            for (uint32_t y = 0; y < h; y++) {
                int r = (int)(sum_r / window);
                int g = (int)(sum_g / window);
                int b = (int)(sum_b / window);
                int luma = (r * 54 + g * 183 + b * 19 + 128) / 256;
                r = luma + ((r - luma) * saturation_pct + 50) / 100 + brightness_bias;
                g = luma + ((g - luma) * saturation_pct + 50) / 100 + brightness_bias;
                b = luma + ((b - luma) * saturation_pct + 50) / 100 + brightness_bias;
                dst->buffer[(size_t)y * dst_stride + x] = 0xFF000000u | ((uint32_t)clamp_material_channel(r) << 16) |
                                                          ((uint32_t)clamp_material_channel(g) << 8) |
                                                          clamp_material_channel(b);

                int ri = (int)y - radius, ai = (int)y + radius + 1;
                if (ri < 0)
                    ri = 0;
                if (ai >= (int)h)
                    ai = (int)h - 1;
                uint32_t rp = g_blur_scratch.buffer[(size_t)ri * tmp_stride + x];
                uint32_t ap = g_blur_scratch.buffer[(size_t)ai * tmp_stride + x];
                sum_r += ((ap >> 16) & 0xFFu) - ((rp >> 16) & 0xFFu);
                sum_g += ((ap >> 8) & 0xFFu) - ((rp >> 8) & 0xFFu);
                sum_b += (ap & 0xFFu) - (rp & 0xFFu);
            }
        }
    }
}

static void postprocess_material_surface(Surface *surface, int saturation_pct, int brightness_bias)
{
    if (!surface || !surface->buffer)
        return;
    uint32_t stride = surface->pitch / 4u;
    for (uint32_t y = 0; y < surface->height; y++) {
        uint32_t *row = &surface->buffer[(size_t)y * stride];
        for (uint32_t x = 0; x < surface->width; x++) {
            uint32_t pixel = row[x];
            uint8_t alpha = (uint8_t)(pixel >> 24);
            int r = (pixel >> 16) & 0xFFu;
            int g = (pixel >> 8) & 0xFFu;
            int b = pixel & 0xFFu;
            int luma = (r * 54 + g * 183 + b * 19 + 128) / 256;

            r = luma + ((r - luma) * saturation_pct + 50) / 100 + brightness_bias;
            g = luma + ((g - luma) * saturation_pct + 50) / 100 + brightness_bias;
            b = luma + ((b - luma) * saturation_pct + 50) / 100 + brightness_bias;

            row[x] = ((uint32_t)alpha << 24) | ((uint32_t)clamp_material_channel(r) << 16) |
                     ((uint32_t)clamp_material_channel(g) << 8) | clamp_material_channel(b);
        }
    }
}

static void downsample_box(const Surface *src, Surface *dst, int factor)
{
    if (!src || !dst || !src->buffer || !dst->buffer || factor <= 1)
        return;
    uint32_t sw = src->width, sh = src->height;
    uint32_t dw = dst->width, dh = dst->height;
    uint32_t ss = src->pitch / 4u, ds = dst->pitch / 4u;
    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t sy0 = dy * (uint32_t)factor;
        uint32_t sy1 = sy0 + (uint32_t)factor;
        if (sy1 > sh)
            sy1 = sh;
        uint32_t *drow = &dst->buffer[(size_t)dy * ds];
        for (uint32_t dx = 0; dx < dw; dx++) {
            uint32_t sx0 = dx * (uint32_t)factor;
            uint32_t sx1 = sx0 + (uint32_t)factor;
            if (sx1 > sw)
                sx1 = sw;
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                const uint32_t *srow = &src->buffer[(size_t)sy * ss];
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    uint32_t p = srow[sx];
                    sum_r += (p >> 16) & 0xFFu;
                    sum_g += (p >> 8) & 0xFFu;
                    sum_b += p & 0xFFu;
                    count++;
                }
            }
            if (count > 0)
                drow[dx] = 0xFF000000u | ((sum_r / count) << 16) | ((sum_g / count) << 8) | (sum_b / count);
        }
    }
}

static void blur_surface_box(Surface *s, int radius)
{
    if (!s || !s->buffer || radius <= 0)
        return;
    uint32_t w = s->width, h = s->height, stride = s->pitch / 4;
    uint32_t *tmp = (uint32_t *)malloc(w * h * 4);
    if (!tmp)
        return;

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *src = &s->buffer[y * stride];
        uint32_t *dst = &tmp[y * w];
        uint32_t r = 0, g = 0, b = 0;
        int count = 0;

        auto add = [&](int x) {
            if (x >= 0 && x < (int)w) {
                uint32_t p = src[x];
                r += (p >> 16) & 0xFF;
                g += (p >> 8) & 0xFF;
                b += p & 0xFF;
                count++;
            }
        };
        auto sub = [&](int x) {
            if (x >= 0 && x < (int)w) {
                uint32_t p = src[x];
                r -= (p >> 16) & 0xFF;
                g -= (p >> 8) & 0xFF;
                b -= p & 0xFF;
                count--;
            }
        };

        for (int x = -radius; x < (int)w; x++) {
            add(x + radius);
            if (x >= 0) {
                dst[x] = 0xFF000000u | ((r / count) << 16) | ((g / count) << 8) | (b / count);
                sub(x - radius);
            }
        }
    }

    static constexpr uint32_t TILE = 16;
    for (uint32_t x0 = 0; x0 < w; x0 += TILE) {
        uint32_t x_end = (x0 + TILE > w) ? w : x0 + TILE;
        for (uint32_t x = x0; x < x_end; x++) {
            uint32_t r = 0, g = 0, b = 0;
            int count = 0;

            auto add = [&](int y) {
                if (y >= 0 && y < (int)h) {
                    uint32_t p = tmp[y * w + x];
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                    count++;
                }
            };
            auto sub = [&](int y) {
                if (y >= 0 && y < (int)h) {
                    uint32_t p = tmp[y * w + x];
                    r -= (p >> 16) & 0xFF;
                    g -= (p >> 8) & 0xFF;
                    b -= p & 0xFF;
                    count--;
                }
            };

            for (int y = -radius; y < (int)h; y++) {
                add(y + radius);
                if (y >= 0) {
                    s->buffer[y * stride + x] = 0xFF000000u | ((r / count) << 16) | ((g / count) << 8) | (b / count);
                    sub(y - radius);
                }
            }
        }
    }

    free(tmp);
}

static void upsample_bilinear(const Surface *src, Surface *dst, float factor)
{
    if (!src || !src->buffer || !dst || !dst->buffer || factor <= 0.0f)
        return;

    uint32_t sw = src->width, sh = src->height;
    uint32_t dw = dst->width, dh = dst->height;
    uint32_t ss = src->pitch / 4, ds = dst->pitch / 4;

    uint32_t scale_x_fp = (uint32_t)((1.0f / factor) * 65536.0f);
    uint32_t scale_y_fp = (uint32_t)((1.0f / factor) * 65536.0f);

    for (uint32_t y = 0; y < dh; y++) {
        uint32_t *drow = &dst->buffer[(size_t)y * ds];
        uint32_t fy_fp = y * scale_y_fp;
        uint32_t iy0 = fy_fp >> 16;
        uint32_t ify = (fy_fp >> 8) & 0xFFu;
        uint32_t ify_inv = 256u - ify;

        uint32_t iy1 = (iy0 >= sh - 1) ? iy0 : iy0 + 1;
        const uint32_t *srow0 = &src->buffer[(size_t)iy0 * ss];
        const uint32_t *srow1 = &src->buffer[(size_t)iy1 * ss];

        for (uint32_t x = 0; x < dw; x++) {
            uint32_t fx_fp = x * scale_x_fp;
            uint32_t ix0 = fx_fp >> 16;
            uint32_t ifx = (fx_fp >> 8) & 0xFFu;
            uint32_t ifx_inv = 256u - ifx;

            uint32_t ix1 = (ix0 >= sw - 1) ? ix0 : ix0 + 1;

            uint32_t p00 = srow0[ix0], p10 = srow0[ix1];
            uint32_t p01 = srow1[ix0], p11 = srow1[ix1];

            uint32_t w00 = (ifx_inv * ify_inv);
            uint32_t w10 = (ifx * ify_inv);
            uint32_t w01 = (ifx_inv * ify);
            uint32_t w11 = (ifx * ify);

            auto interp = [&](int shift) {
                return (uint32_t)((((p00 >> shift) & 0xFF) * w00 + ((p10 >> shift) & 0xFF) * w10 +
                                   ((p01 >> shift) & 0xFF) * w01 + ((p11 >> shift) & 0xFF) * w11 + 32768u) >>
                                  16);
            };

            drow[x] = (interp(24) << 24) | (interp(16) << 16) | (interp(8) << 8) | interp(0);
        }
    }
}

static void blur_surface_material(const Surface *src, Surface *dst, float sigma, int saturation_pct,
                                  int brightness_bias)
{
    if (!src || !dst || !src->buffer || !dst->buffer || src->width != dst->width || src->height != dst->height)
        return;

    int radii[3] = {};
    compute_gaussian_box_radii(sigma, radii);
    if (radii[0] <= 0 && radii[1] <= 0 && radii[2] <= 0) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)src->width, (int)src->height);
        postprocess_material_surface(dst, saturation_pct, brightness_bias);
        return;
    }

    static constexpr int DOWNSAMPLE_FACTOR = 4;
    uint32_t small_w = (src->width + DOWNSAMPLE_FACTOR - 1) / DOWNSAMPLE_FACTOR;
    uint32_t small_h = (src->height + DOWNSAMPLE_FACTOR - 1) / DOWNSAMPLE_FACTOR;

    if (small_w >= 4 && small_h >= 4 && src->width >= 64 && src->height >= 16 &&
        ensure_surface_capacity(&g_blur_small_src, small_w, small_h) &&
        ensure_surface_capacity(&g_blur_small_dst, small_w, small_h) &&
        ensure_surface_capacity(&g_blur_pass_a, small_w, small_h) &&
        ensure_surface_capacity(&g_blur_pass_b, small_w, small_h)) {
        downsample_box(src, &g_blur_small_src, DOWNSAMPLE_FACTOR);

        float adjusted_sigma = sigma / (float)DOWNSAMPLE_FACTOR;
        int small_radii[3] = {};
        compute_gaussian_box_radii(adjusted_sigma, small_radii);

        blur_surface_box(&g_blur_small_src, &g_blur_pass_a, small_radii[0]);
        blur_surface_box(&g_blur_pass_a, &g_blur_pass_b, small_radii[1]);
        blur_surface_box_fused(&g_blur_pass_b, &g_blur_small_dst, small_radii[2], saturation_pct, brightness_bias);

        upsample_bilinear(&g_blur_small_dst, dst, DOWNSAMPLE_FACTOR);
        return;
    }

    if (!ensure_surface_capacity(&g_blur_pass_a, src->width, src->height) ||
        !ensure_surface_capacity(&g_blur_pass_b, src->width, src->height)) {
        blur_surface_box(src, dst, radii[0]);
        postprocess_material_surface(dst, saturation_pct, brightness_bias);
        return;
    }

    blur_surface_box(src, &g_blur_pass_a, radii[0]);
    blur_surface_box(&g_blur_pass_a, &g_blur_pass_b, radii[1]);
    blur_surface_box_fused(&g_blur_pass_b, dst, radii[2], saturation_pct, brightness_bias);
}

static void mark_shell_blur_dirty(Registry *registry, const DirtyRect &screen_rect)
{
    if (!registry || !g_backbuffer.buffer)
        return;

    DirtyRect menubar_rect = {0, 0, (int)g_screen.width, wm_menubar_h()};
    DirtyRect overlap = {};
    if (g_menubar_blur_source.buffer && rect_intersection(screen_rect, menubar_rect, &overlap)) {
        copy_surface_rect(&g_menubar_blur_source, overlap.x, overlap.y, &g_backbuffer, overlap.x, overlap.y, overlap.w,
                          overlap.h);
        g_menubar_blur_dirty = true;
    }

    if (g_dock_blur_source.buffer && registry->window_count > 1) {
        DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                               registry->windows[1].h};
        if (clip_dirty_rect_to_screen(dock_rect) && rect_intersection(screen_rect, dock_rect, &overlap)) {
            copy_surface_rect(&g_dock_blur_source, overlap.x - dock_rect.x, overlap.y - dock_rect.y, &g_backbuffer,
                              overlap.x, overlap.y, overlap.w, overlap.h);
            g_dock_blur_dirty = true;
        }
    }
}

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
    uint32_t pitch = dst->pitch / 4;

    int start_y = y < 0 ? 0 : y;
    int end_y = y + h;
    if (end_y > (int)dst->height)
        end_y = (int)dst->height;
    int start_x = x < 0 ? 0 : x;
    int end_x = x + w;
    if (end_x > (int)dst->width)
        end_x = (int)dst->width;

    for (int py = start_y; py < end_y; py++) {
        int row = py - y;
        uint32_t *dst_row = &dst->buffer[(size_t)py * pitch];
        for (int px = start_x; px < end_x; px++) {
            uint8_t coverage = gui_rounded_rect_coverage_local(px - x, row, w, h, r, GUI_ROUNDED_EDGE_TOP);
            if (coverage == 0)
                continue;
            uint32_t &dst_px = dst_row[px];
            if (coverage == 255 && base_alpha == 255)
                dst_px = color;
            else
                dst_px = blend_rgb(dst_px, color, coverage);
        }
    }
}

void paint_desktop_base(Surface *surface)
{
    if (!surface || !surface->buffer)
        return;
    uint32_t color_top = 0xFF0B1533;
    uint32_t color_mid = 0xFF2C1F54;
    uint32_t color_bottom = 0xFF140A12;
    for (uint32_t y = 0; y < surface->height; y++) {
        uint8_t t = surface->height > 1 ? (uint8_t)((y * 255u) / (surface->height - 1u)) : 0u;
        uint32_t row_color = t < 132 ? mix_rgb(color_top, color_mid, (uint8_t)((uint32_t)t * 255u / 132u))
                                     : mix_rgb(color_mid, color_bottom, (uint8_t)(((uint32_t)t - 132u) * 255u / 123u));
        gui_fill_rect(surface, 0, (int)y, surface->width, 1, row_color);
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

    g_menubar_blur.buffer = (uint32_t *)mb_map;
    g_menubar_blur.width = g_screen.width;
    g_menubar_blur.height = (uint32_t)menubar_h;
    g_menubar_blur.pitch = g_screen.width * 4u;
    g_menubar_blur.owns_buffer = false;

    g_dock_blur.buffer = (uint32_t *)dk_map;
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
        blur_surface_material(&g_menubar_blur_source, &g_menubar_blur, 30.0f, is_light ? 120 : 110, is_light ? 4 : 0);
        registry->mb_blur_generation = registry->mb_blur_generation + 1u;
        g_menubar_blur_dirty = false;
    }
    if (g_dock_blur_dirty && g_dock_blur.buffer && g_dock_blur_source.buffer) {
        blur_surface_material(&g_dock_blur_source, &g_dock_blur, 24.0f, is_light ? 118 : 108, is_light ? 4 : 0);
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
    int button_size = wm_button_size();
    int space_1 = gui_space_1();
    int space_2 = gui_space_2();
    int border = wm_frame_border();
    int detail_inset = gui_scaled_metric(1);
    if (detail_inset < 1)
        detail_inset = 1;
    int radius = gui_scaled_metric(13);
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

void draw_window_client_clipped(Surface *dst, const Window &w, const DirtyRect &clip)
{
    int ix, iy, iw, ih;
    if (!gui_intersect_rect(w.x, w.y, w.w, w.h, clip.x, clip.y, clip.w, clip.h, &ix, &iy, &iw, &ih))
        return;
    if (w.buffer_w <= 0 || w.buffer_h <= 0 || !w.buffer)
        return;

    int src_x = ix - w.x + w.scroll_x, src_y = iy - w.y + w.scroll_y;
    int copy_w = iw, copy_h = ih;
    if (src_x + copy_w > w.buffer_w)
        copy_w = w.buffer_w - src_x;
    if (src_y + copy_h > w.buffer_h)
        copy_h = w.buffer_h - src_y;
    if (copy_w <= 0 || copy_h <= 0)
        return;

    uint32_t dst_stride = dst->pitch / 4;
    if (w.transparent) {
        blit_alpha_blend_rect(&dst->buffer[(size_t)iy * dst_stride + ix], dst_stride,
                              &w.buffer[(size_t)src_y * w.buffer_w + src_x], w.buffer_w, copy_w, copy_h);
        return;
    }

    int inner_left = w.x + wm_frame_border(), inner_top = w.y;
    int inner_w = w.w - wm_frame_border() * 2, inner_h = w.h - wm_frame_border();
    int inner_r = gui_scaled_metric(12);
    if (inner_r > inner_w / 2)
        inner_r = inner_w / 2;
    if (inner_r > inner_h / 2)
        inner_r = inner_h / 2;

    int rx, ry, rw, rh;
    if (!gui_intersect_rect(ix, iy, copy_w, copy_h, inner_left, inner_top, inner_w, inner_h, &rx, &ry, &rw, &rh))
        return;

    if (inner_r <= 0) {
        Surface src_surface = {w.buffer, (uint32_t)w.buffer_w, (uint32_t)w.buffer_h, (uint32_t)w.buffer_w * 4, false,
                               0};
        copy_surface_rect(dst, rx, ry, &src_surface, src_x + (rx - ix), src_y + (ry - iy), rw, rh);
    } else {
        const int rounded_start_y = inner_top + inner_h - inner_r;
        const int center_start_x = inner_left + inner_r;
        const int center_end_x = inner_left + inner_w - inner_r;

        for (int py = 0; py < rh; py++) {
            int dst_y = ry + py;
            int src_row_base = src_y + (dst_y - iy);
            if (dst_y < 0 || dst_y >= (int)dst->height || src_row_base < 0 || src_row_base >= w.buffer_h)
                continue;

            uint32_t *dst_ptr = &dst->buffer[(size_t)dst_y * dst_stride];
            const uint32_t *src_ptr = &w.buffer[(size_t)src_row_base * w.buffer_w];

            if (dst_y < rounded_start_y) {
                memcpy(&dst_ptr[rx], &src_ptr[src_x + (rx - ix)], (size_t)rw * 4u);
            } else {
                for (int dst_x = rx; dst_x < rx + rw;) {
                    if (dst_x >= center_start_x && dst_x < center_end_x) {
                        int run_end = center_end_x;
                        if (run_end > rx + rw)
                            run_end = rx + rw;
                        int run_len = run_end - dst_x;
                        if (run_len > 0) {
                            memcpy(&dst_ptr[dst_x], &src_ptr[src_x + (dst_x - ix)], (size_t)run_len * 4u);
                            dst_x += run_len;
                            continue;
                        }
                    }

                    int src_col = src_x + (dst_x - ix);
                    if (src_col < 0 || src_col >= w.buffer_w) {
                        dst_x++;
                        continue;
                    }
                    uint8_t coverage = gui_rounded_rect_coverage_local(dst_x - inner_left, dst_y - inner_top, inner_w,
                                                                       inner_h, inner_r, GUI_ROUNDED_EDGE_BOTTOM);
                    if (coverage > 0) {
                        uint32_t &dp = dst_ptr[dst_x];
                        uint32_t sp = src_ptr[src_col];
                        if (coverage == 255) {
                            dp = sp;
                        } else {
                            uint32_t inv = 255u - coverage;
                            uint32_t dr = (dp >> 16) & 0xFFu, dg = (dp >> 8) & 0xFFu, db = dp & 0xFFu;
                            uint32_t sr = (sp >> 16) & 0xFFu, sg = (sp >> 8) & 0xFFu, sb = sp & 0xFFu;
                            uint32_t r = (sr * coverage + dr * inv + 127u) / 255u;
                            uint32_t g = (sg * coverage + dg * inv + 127u) / 255u;
                            uint32_t b = (sb * coverage + db * inv + 127u) / 255u;
                            dp = 0xFF000000u | (r << 16) | (g << 8) | b;
                        }
                    }
                    dst_x++;
                }
            }
        }
    }
}

void compose_rect_unclipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                            const Registry *registry)
{
    int start_index = 0;
    bool covered_by_opaque = false;
    for (int i = g_window_count - 1; i >= 0; i--) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || w.transparent || !w.buffer)
            continue;
        DirtyRect outer = window_occlusion_bounds(w);
        if (r.x >= outer.x && r.y >= outer.y && r.x + r.w <= outer.x + outer.w && r.y + r.h <= outer.y + outer.h) {
            start_index = i;
            covered_by_opaque = true;
            break;
        }
    }

    if (!covered_by_opaque)
        gui_blit_rect(&g_backbuffer, &g_wallpaper, r.x, r.y, r.x, r.y, r.w, r.h);

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || w.transparent)
            continue;
        DirtyRect outer = g_window_outer_cache[i];
        if (!(r.x >= outer.x + outer.w || r.x + r.w <= outer.x || r.y >= outer.y + outer.h || r.y + r.h <= outer.y)) {
            draw_window_decoration_clipped(&g_backbuffer, w, r, (i == focused_index), (i == hover_frame_index),
                                           (i == hover_frame_index) ? hover_button : -1);
        }
        draw_window_client_clipped(&g_backbuffer, w, r);
    }

    capture_shell_backdrop_for_rect(r, const_cast<Registry *>(registry));

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || !w.transparent)
            continue;
        draw_window_client_clipped(&g_backbuffer, w, r);
    }

    if (g_context_menu.open && rect_intersection(r, context_menu_bounds(), nullptr))
        draw_context_menu_overlay(registry);
    if (g_storage_prompt.visible)
        draw_storage_prompt_overlay_clipped(r);
    if (g_index.active)
        draw_index_overlay_clipped(r, registry);
    if (g_control_center.open)
        draw_control_center_overlay_clipped(r);
}

bool compose_rect_clipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                          const Registry *registry)
{
    if (!g_backbuffer.buffer || g_window_visible_region_overflow)
        return false;

    int start_index = find_top_opaque_covering_window(r);
    if (start_index < 0) {
        gui_blit_rect(&g_backbuffer, &g_wallpaper, r.x, r.y, r.x, r.y, r.w, r.h);
        start_index = 0;
    }

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || w.transparent)
            continue;

        if (!dirty_rects_intersect(r, g_window_outer_cache[i]))
            continue;

        int region_count = g_window_visible_region_count[i];
        if (region_count > MAX_VISIBLE_REGIONS)
            region_count = MAX_VISIBLE_REGIONS;
        for (int region = 0; region < region_count; region++) {
            DirtyRect visible = {};
            if (!rect_intersection(g_window_visible_regions[i][region], r, &visible))
                continue;
            bool focused = (i == focused_index);
            bool hovered_frame = (i == hover_frame_index);
            draw_window_decoration_clipped(&g_backbuffer, w, visible, focused, hovered_frame,
                                           hovered_frame ? hover_button : -1);
            if (rect_intersection(visible, g_window_client_cache[i], nullptr)) {
                draw_window_client_clipped(&g_backbuffer, w, visible);
            }
        }
    }

    capture_shell_backdrop_for_rect(r, const_cast<Registry *>(registry));

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || !w.transparent)
            continue;

        if (!dirty_rects_intersect(r, g_window_outer_cache[i]))
            continue;

        draw_window_client_clipped(&g_backbuffer, w, r);
    }

    if (g_context_menu.open && rect_intersection(r, context_menu_bounds(), nullptr))
        draw_context_menu_overlay(registry);
    if (g_storage_prompt.visible)
        draw_storage_prompt_overlay_clipped(r);
    if (g_index.active)
        draw_index_overlay_clipped(r, registry);
    if (g_control_center.open)
        draw_control_center_overlay_clipped(r);
    return true;
}

bool move_backbuffer_rect(const DirtyRect &old_rect, const DirtyRect &new_rect)
{
    if (!g_backbuffer.buffer || old_rect.w != new_rect.w || old_rect.h != new_rect.h)
        return false;
    DirtyRect src = old_rect;
    DirtyRect dst = new_rect;
    if (!clip_dirty_rect_to_screen(src) || !clip_dirty_rect_to_screen(dst))
        return false;
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
        return false;
    if (src.w != dst.w || src.h != dst.h)
        return false;

    uint32_t stride = g_backbuffer.pitch / 4;
    int start = 0, end = src.h, step = 1;
    if (dst.y > src.y) {
        start = src.h - 1;
        end = -1;
        step = -1;
    }

    for (int row = start; row != end; row += step) {
        memmove(&g_backbuffer.buffer[(dst.y + row) * stride + dst.x],
                &g_backbuffer.buffer[(src.y + row) * stride + src.x], (size_t)src.w * sizeof(uint32_t));
    }
    return true;
}

static uint32_t storage_prompt_scrim_color()
{
    uint32_t base = g_gui_style.app_bg ? g_gui_style.app_bg : g_gui_chrome.desktop_bg;
    bool light = color_luma(base) >= 128;
    uint32_t material = mix_rgb(base, g_gui_style.chrome_bg, light ? 14 : 8);
    uint8_t alpha = light ? 96 : 156;
    return ((uint32_t)alpha << 24) | (material & 0x00FFFFFFu);
}

void draw_context_menu_overlay(const Registry *registry)
{
    if (!g_context_menu.open)
        return;
    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count > 0)
        gui_draw_popup_menu(&g_backbuffer, g_context_menu.x, g_context_menu.y, g_context_menu.w, items, count,
                            g_context_menu.hovered_index);
}

static void draw_storage_prompt_overlay_clipped(const DirtyRect &clip)
{
    if (!g_storage_prompt.visible || !g_backbuffer.buffer)
        return;
    DirtyRect screen = {0, 0, (int)g_backbuffer.width, (int)g_backbuffer.height};
    DirtyRect dim = {};
    if (!rect_intersection(screen, clip, &dim))
        return;

    uint32_t stride = g_backbuffer.pitch / 4;
    uint32_t scrim = storage_prompt_scrim_color();
    for (int y = dim.y; y < dim.y + dim.h; y++) {
        uint32_t *row = &g_backbuffer.buffer[(size_t)y * stride + dim.x];
        for (int x = 0; x < dim.w; x++) {
            row[x] = blend_rgb(row[x], scrim, 255);
        }
    }

    StoragePromptLayout layout = storage_prompt_layout();
    if (!rect_intersection(clip, layout.box, nullptr))
        return;

    int box_r = gui_radius_lg();
    gui_fill_rounded_rect(&g_backbuffer, layout.box.x, layout.box.y + gui_scaled_metric(10), layout.box.w, layout.box.h,
                          box_r, 0x18000000u);
    gui_fill_rounded_rect(&g_backbuffer, layout.box.x, layout.box.y + gui_scaled_metric(3), layout.box.w, layout.box.h,
                          box_r, g_gui_chrome.frame_shadow);
    gui_draw_panel_inset(&g_backbuffer, layout.box.x, layout.box.y, layout.box.w, layout.box.h, g_gui_style.app_surface,
                         g_gui_style.border_focus, g_gui_style.chrome_bg_alt);
    gui_draw_card_header(&g_backbuffer, layout.box.x + 1, layout.box.y + 1, layout.box.w - 2, "Storage Mode",
                         "Choose how uniOS should expose AHCI and ATA storage");

    int text_x = layout.box.x + gui_space_2();
    int content_y = layout.box.y + gui_card_header_h() + gui_space_2();
    int text_w = layout.box.w - gui_space_4();

    content_y +=
        gui_draw_wrapped_value(&g_backbuffer, text_x, content_y, text_w,
                               "Off hides persistent storage from apps. Read-Only allows browsing without disk writes. "
                               "Writable enables normal file changes and seeds standard user folders in /data.",
                               g_gui_style.text, g_gui_style.app_surface);

    int note_y = content_y + gui_space_1_5();
    int note_h = gui_scaled_metric(52);
    if (note_y + note_h < layout.off_button.y - gui_space_1()) {
        gui_fill_rounded_rect(&g_backbuffer, text_x, note_y, text_w, note_h, gui_radius_md(), g_gui_style.chrome_bg);
        gui_draw_rounded_rect(&g_backbuffer, text_x, note_y, text_w, note_h, gui_radius_md(), g_gui_style.border);
        gui_draw_badge(&g_backbuffer, text_x + gui_space_1(), note_y + gui_scaled_metric(8), "CAUTION",
                       g_gui_style.warning, g_gui_style.app_surface);
        int note_text_x = text_x + gui_scaled_metric(92);
        gui_draw_wrapped_value(
            &g_backbuffer, note_text_x, note_y + gui_scaled_metric(8), text_w - (note_text_x - text_x) - gui_space_1(),
            "Choose Writable only if you are intentionally testing on hardware you are prepared to modify.",
            g_gui_style.text_dim, g_gui_style.chrome_bg);
    }

    int footer_y = layout.off_button.y - gui_space_1();
    gui_draw_separator_h(&g_backbuffer, layout.box.x + 1, footer_y, layout.box.w - 2, g_gui_style.chrome_edge);

    gui_app_draw_button(&g_backbuffer, layout.off_button.x, layout.off_button.y, layout.off_button.w,
                        layout.off_button.h, "Off", false, false, g_storage_prompt.hovered_button == 0);
    gui_app_draw_button(&g_backbuffer, layout.readonly_button.x, layout.readonly_button.y, layout.readonly_button.w,
                        layout.readonly_button.h, "Read-Only", false, false, g_storage_prompt.hovered_button == 1);
    gui_app_draw_button(&g_backbuffer, layout.writable_button.x, layout.writable_button.y, layout.writable_button.w,
                        layout.writable_button.h, "Writable", true, false, g_storage_prompt.hovered_button == 2);
}

void draw_storage_prompt_overlay()
{
    DirtyRect full = {0, 0, (int)g_backbuffer.width, (int)g_backbuffer.height};
    draw_storage_prompt_overlay_clipped(full);
}

static int wm_index_result_item_h()
{
    int h = gui_scaled_metric(52);
    return h < gui_scaled_metric(40) ? gui_scaled_metric(40) : h;
}

static DirtyRect wm_index_search_bounds()
{
    DirtyRect box = index_overlay_bounds();
    int pad = gui_space_2();
    int h = gui_scaled_metric(44);
    return {box.x + pad, box.y + pad, box.w - pad * 2, h};
}

static int wm_index_results_start_y()
{
    DirtyRect search = wm_index_search_bounds();
    return search.y + search.h + gui_space_1();
}

void draw_index_overlay_clipped(const DirtyRect &clip, const Registry *registry)
{
    (void)registry;
    if (!g_index.active || !g_backbuffer.buffer)
        return;

    DirtyRect box = index_overlay_bounds();
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));
    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = gui_radius_lg();
    int shadow = gui_scaled_metric(6);
    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + shadow, box.w, box.h, radius, g_gui_chrome.frame_shadow);
    gui_draw_panel_inset(&g_backbuffer, box.x, box.y, box.w, box.h, g_gui_style.app_surface, g_gui_style.border_focus,
                         g_gui_style.chrome_bg_alt);

    DirtyRect search = wm_index_search_bounds();
    const char *query = g_index.query_len > 0 ? g_index.query : "";
    gui_app_draw_text_field(&g_backbuffer, search.x, search.y, search.w, search.h, query, g_index.query_len > 0, false);
    if (g_index.query_len == 0) {
        int placeholder_y = gui_align_text_y(gui_font_default(), search.y, search.h);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), search.x + gui_space_1(), placeholder_y,
                              search.w - gui_space_2(), "Search apps, commands, settings", g_gui_style.text_muted,
                              g_gui_style.app_surface);
    }

    int hint_w = gui_measure_text(gui_font_default(), "Enter");
    int hint_x = search.x + search.w - hint_w - gui_space_1();
    if (hint_x > search.x + search.w / 2) {
        int hint_y = gui_align_text_y(gui_font_default(), search.y, search.h);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), hint_x, hint_y,
                              search.x + search.w - hint_x - gui_space_1(), "Enter", g_gui_style.text_muted,
                              g_gui_style.app_surface);
    }

    int pad = gui_space_2();
    int row_y = wm_index_results_start_y();
    int row_h = wm_index_result_item_h();
    int row_gap = gui_scaled_metric(2);
    int bottom = box.y + box.h - pad;

    if (g_index.result_count <= 0) {
        int empty_y = row_y + gui_space_2();
        gui_draw_text_clipped(&g_backbuffer, gui_font_title(), box.x + pad, empty_y, box.w - pad * 2, "No matches",
                              g_gui_style.text_dim, g_gui_style.app_surface);
        gui_draw_text_clipped(&g_backbuffer, gui_font_default(), box.x + pad,
                              empty_y + gui_line_height() + gui_scaled_metric(4), box.w - pad * 2,
                              "Try an app name, setting, or command.", g_gui_style.text_muted, g_gui_style.app_surface);
        return;
    }

    for (int i = 0; i < g_index.result_count; i++) {
        if (row_y + row_h > bottom)
            break;
        bool selected = i == g_index.selected_index;
        bool hovered = i == g_index.hovered_index;
        const IndexResult &result = g_index.results[i];
        const char *badge = result.is_app ? "APP" : "CMD";
        const char *detail = result.detail[0] ? result.detail : result.path;
        gui_app_draw_list_row(&g_backbuffer, box.x + pad, row_y, box.w - pad * 2, row_h, badge, result.title, detail,
                              selected, hovered, false);
        row_y += row_h + row_gap;
    }
}

static int wm_control_panel_card_h()
{
    int h = gui_scaled_metric(58);
    return h < gui_scaled_metric(46) ? gui_scaled_metric(46) : h;
}

static DirtyRect wm_control_item_rect(ControlPanelItem item)
{
    DirtyRect box = control_center_bounds();
    int pad = gui_space_2();
    int gap = gui_space_1();
    int header_h = gui_card_header_h();
    int card_h = wm_control_panel_card_h();
    int half_w = (box.w - pad * 2 - gap) / 2;
    int y = box.y + header_h + pad;

    if (item == CONTROL_ITEM_NETWORK)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_DARK_MODE)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_DESKTOP_GRID)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_CLOCK_SECONDS)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_ANIMATIONS)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_TRANSPARENCY)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_VOLUME)
        return {box.x + pad, y, box.w - pad * 2, gui_scaled_metric(72)};

    int action_h = gui_app_control_h();
    int action_y = box.y + box.h - pad - action_h;
    int action_w = (box.w - pad * 2 - gap) / 2;
    if (item == CONTROL_ITEM_STORAGE)
        return {box.x + pad, action_y, action_w, action_h};
    if (item == CONTROL_ITEM_SETTINGS)
        return {box.x + pad + action_w + gap, action_y, action_w, action_h};

    return {0, 0, 0, 0};
}

static void draw_control_toggle(ControlPanelItem item, const char *label, const char *detail, bool on)
{
    DirtyRect r = wm_control_item_rect(item);
    gui_app_draw_toggle_row(&g_backbuffer, r.x, r.y, r.w, r.h, label, detail, on, false,
                            g_control_center.hovered_item == item);
}

static void draw_control_volume_card()
{
    DirtyRect r = wm_control_item_rect(CONTROL_ITEM_VOLUME);
    bool hovered = g_control_center.hovered_item == CONTROL_ITEM_VOLUME || g_control_center.volume_dragging;
    uint32_t bg = hovered ? g_gui_style.app_surface_alt : g_gui_style.app_surface;
    gui_fill_rounded_rect(&g_backbuffer, r.x, r.y, r.w, r.h, gui_radius_md(), bg);
    gui_draw_rounded_rect(&g_backbuffer, r.x, r.y, r.w, r.h, gui_radius_md(),
                          hovered ? g_gui_style.border_hover : g_gui_style.border);

    char value[16];
    snprintf(value, sizeof(value), "%u%%", (unsigned)g_control_center.volume);
    int pad = gui_space_2();
    int label_y = r.y + gui_space_1();
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), r.x + pad, label_y, r.w / 2, "Volume", g_gui_style.text,
                          bg);
    int value_w = gui_measure_text(gui_font_default(), value);
    gui_draw_text_clipped(&g_backbuffer, gui_font_default(), r.x + r.w - pad - value_w, label_y, value_w, value,
                          g_gui_style.text_dim, bg);

    int track_h = gui_scaled_metric(18);
    int track_x = r.x + pad;
    int track_y = r.y + r.h - pad - track_h;
    int track_w = r.w - pad * 2;
    int track_r = gui_corner_radius(track_w, track_h, track_h / 2);
    gui_fill_rounded_rect(&g_backbuffer, track_x, track_y, track_w, track_h, track_r, g_gui_style.chrome_bg_alt);
    gui_draw_rounded_rect(&g_backbuffer, track_x, track_y, track_w, track_h, track_r, g_gui_style.border);
    int fill_w = (int)((uint64_t)track_w * g_control_center.volume / 100u);
    if (fill_w > 0)
        gui_fill_rounded_rect(&g_backbuffer, track_x, track_y, fill_w, track_h, track_r, g_gui_style.accent);
    int knob_d = track_h + gui_scaled_metric(4);
    int knob_x = track_x + fill_w - knob_d / 2;
    if (knob_x < track_x)
        knob_x = track_x;
    if (knob_x + knob_d > track_x + track_w)
        knob_x = track_x + track_w - knob_d;
    gui_fill_rounded_rect(&g_backbuffer, knob_x, track_y - gui_scaled_metric(2), knob_d, knob_d, knob_d / 2,
                          g_gui_style.app_surface);
    gui_draw_rounded_rect(&g_backbuffer, knob_x, track_y - gui_scaled_metric(2), knob_d, knob_d, knob_d / 2,
                          g_gui_style.border_focus);
}

void draw_control_center_overlay_clipped(const DirtyRect &clip)
{
    if (!g_control_center.open || !g_backbuffer.buffer)
        return;

    DirtyRect box = control_center_bounds();
    DirtyRect damage = rect_expand(box, gui_scaled_metric(14));
    if (!rect_intersection(clip, damage, nullptr))
        return;

    int radius = gui_radius_lg();
    int shadow = gui_scaled_metric(6);
    gui_fill_rounded_rect(&g_backbuffer, box.x, box.y + shadow, box.w, box.h, radius, g_gui_chrome.frame_shadow);
    gui_draw_panel_inset(&g_backbuffer, box.x, box.y, box.w, box.h, g_gui_style.app_surface, g_gui_style.border,
                         g_gui_style.chrome_bg_alt);
    gui_draw_card_header(&g_backbuffer, box.x + 1, box.y + 1, box.w - 2, "Control Panel", "uniOS");

    draw_control_toggle(CONTROL_ITEM_NETWORK, "Network", g_control_center.network_enabled ? "Ethernet" : "Disconnected",
                        g_control_center.network_enabled);
    draw_control_toggle(CONTROL_ITEM_DARK_MODE, "Dark", g_control_center.dark_mode ? "On" : "Off",
                        g_control_center.dark_mode);
    draw_control_toggle(CONTROL_ITEM_DESKTOP_GRID, "Grid", g_control_center.desktop_grid ? "Shown" : "Hidden",
                        g_control_center.desktop_grid);
    draw_control_toggle(CONTROL_ITEM_CLOCK_SECONDS, "Seconds", g_control_center.clock_seconds ? "Show" : "Hide",
                        g_control_center.clock_seconds);
    draw_control_toggle(CONTROL_ITEM_ANIMATIONS, "Motion", g_control_center.animations_enabled ? "On" : "Off",
                        g_control_center.animations_enabled);
    draw_control_toggle(CONTROL_ITEM_TRANSPARENCY, "Transparency",
                        g_control_center.transparency_level < 255 ? "On" : "Off",
                        g_control_center.transparency_level < 255);
    draw_control_volume_card();

    DirtyRect storage = wm_control_item_rect(CONTROL_ITEM_STORAGE);
    DirtyRect settings = wm_control_item_rect(CONTROL_ITEM_SETTINGS);
    gui_app_draw_button(&g_backbuffer, storage.x, storage.y, storage.w, storage.h, "Storage", false, false,
                        g_control_center.hovered_item == CONTROL_ITEM_STORAGE);
    gui_app_draw_button(&g_backbuffer, settings.x, settings.y, settings.w, settings.h, "Settings", true, false,
                        g_control_center.hovered_item == CONTROL_ITEM_SETTINGS);
}
