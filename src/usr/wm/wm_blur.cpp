#include "wm_core.h"

static Surface g_blur_scratch = {};
static Surface g_blur_pass_a = {};
static Surface g_blur_pass_b = {};
static Surface g_blur_small_src = {};
static Surface g_blur_small_dst = {};

static inline uint8_t clamp_material_channel(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

void blur_surface_box(const Surface *src, Surface *dst, int radius)
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

void blur_surface_material(const Surface *src, Surface *dst, float sigma, int saturation_pct, int brightness_bias)
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

    if (small_w >= 128 && small_h >= 64 && src->width >= 512 && src->height >= 256 &&
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
