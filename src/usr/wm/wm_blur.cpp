#include <emmintrin.h>
#include <string.h>

#include "wm_metrics.h"
#include "wm_render.h"

static Surface g_blur_scratch = {};
static Surface g_blur_pass_a = {};
static Surface g_blur_pass_b = {};
static Surface g_blur_small_src = {};
static Surface g_blur_small_dst = {};

// Column running sums for the vertical pass (interleaved BGRA u16 lanes).
// Sized for the maximum buffer dimension the kernel accepts.
static uint16_t g_blur_vstate[8192 * 4];

// u16 lane sums must stay inside 16 bits: 255 * window <= 65535.
static constexpr int BLUR_MAX_SIMD_WINDOW = 257;

struct MaterialAdjust
{
    int saturation_pct;
    int brightness_bias;
};

static inline uint8_t clamp_material_channel(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

// Use precomputed multiply-shift reciprocal to avoid division on hot path.
struct BlurReciprocal
{
    uint64_t recip;
    int shift;
};

static inline BlurReciprocal make_blur_reciprocal(uint32_t window)
{
    BlurReciprocal r = {1, 0};
    if (window == 0)
        return r;
    r.shift = 24;
    r.recip = (((uint64_t)1 << r.shift) + window - 1) / window;
    return r;
}

static inline uint32_t blur_div(uint32_t sum, const BlurReciprocal &r)
{
    return (uint32_t)((sum * r.recip) >> r.shift);
}

static inline uint16_t blur_recip16(uint32_t window)
{
    if (window == 0)
        return 1;
    return (uint16_t)((65536u + window - 1u) / window);
}

static inline __m128i blur_alpha_mask32()
{
    return _mm_set1_epi32(static_cast<int>(0xFF000000u));
}

// Two pixels (one per row) interleaved into 8 zero-extended u16 lanes.
static inline __m128i load_pixel_pair_u16(uint32_t p0, uint32_t p1)
{
    __m128i a = _mm_cvtsi32_si128(static_cast<int>(p0));
    __m128i b = _mm_cvtsi32_si128(static_cast<int>(p1));
    return _mm_unpacklo_epi8(_mm_unpacklo_epi32(a, b), _mm_setzero_si128());
}

static inline void load_8px_u16(const uint32_t *px, __m128i out[4])
{
    const __m128i zero = _mm_setzero_si128();
    __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(px));
    __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(px + 4));
    out[0] = _mm_unpacklo_epi8(a, zero);
    out[1] = _mm_unpackhi_epi8(a, zero);
    out[2] = _mm_unpacklo_epi8(b, zero);
    out[3] = _mm_unpackhi_epi8(b, zero);
}

static void box_blur_rows_pair(const uint32_t *row0, const uint32_t *row1, uint32_t *out0, uint32_t *out1, uint32_t w,
                               int radius, __m128i recip16)
{
    __m128i state =
        _mm_mullo_epi16(load_pixel_pair_u16(row0[0], row1[0]), _mm_set1_epi16(static_cast<short>(radius + 1)));
    for (int s = 1; s <= radius; s++) {
        int idx = s < (int)w ? s : (int)w - 1;
        state = _mm_add_epi16(state, load_pixel_pair_u16(row0[idx], row1[idx]));
    }
    for (uint32_t x = 0; x < w; x++) {
        __m128i q = _mm_mulhi_epu16(state, recip16);
        __m128i bytes = _mm_packus_epi16(q, q);
        uint32_t px[2] = {};
        _mm_storel_epi64(reinterpret_cast<__m128i *>(px), bytes);
        out0[x] = px[0] | 0xFF000000u;
        out1[x] = px[1] | 0xFF000000u;

        int rem = (int)x - radius;
        if (rem < 0)
            rem = 0;
        int add = (int)x + radius + 1;
        if (add >= (int)w)
            add = (int)w - 1;
        __m128i rp = load_pixel_pair_u16(row0[rem], row1[rem]);
        __m128i ap = load_pixel_pair_u16(row0[add], row1[add]);
        state = _mm_add_epi16(_mm_sub_epi16(state, rp), ap);
    }
}

static void box_blur_row_scalar(const uint32_t *row, uint32_t *out, uint32_t w, int radius, const BlurReciprocal &recip)
{
    uint32_t first = row[0];
    int32_t sum_r = (int32_t)(((first >> 16) & 0xFFu) * (uint32_t)(radius + 1));
    int32_t sum_g = (int32_t)(((first >> 8) & 0xFFu) * (uint32_t)(radius + 1));
    int32_t sum_b = (int32_t)((first & 0xFFu) * (uint32_t)(radius + 1));
    for (int s = 1; s <= radius; s++) {
        int idx = s < (int)w ? s : (int)w - 1;
        uint32_t pixel = row[idx];
        sum_r += (int32_t)((pixel >> 16) & 0xFFu);
        sum_g += (int32_t)((pixel >> 8) & 0xFFu);
        sum_b += (int32_t)(pixel & 0xFFu);
    }
    for (uint32_t x = 0; x < w; x++) {
        out[x] = 0xFF000000u | (blur_div((uint32_t)sum_r, recip) << 16) | (blur_div((uint32_t)sum_g, recip) << 8) |
                 blur_div((uint32_t)sum_b, recip);

        int rem = (int)x - radius;
        if (rem < 0)
            rem = 0;
        int add = (int)x + radius + 1;
        if (add >= (int)w)
            add = (int)w - 1;
        uint32_t rp = row[rem];
        uint32_t ap = row[add];
        sum_r += (int32_t)((ap >> 16) & 0xFFu) - (int32_t)((rp >> 16) & 0xFFu);
        sum_g += (int32_t)((ap >> 8) & 0xFFu) - (int32_t)((rp >> 8) & 0xFFu);
        sum_b += (int32_t)(ap & 0xFFu) - (int32_t)(rp & 0xFFu);
    }
}

static void box_blur_horizontal(const Surface *src, Surface *dst, int radius)
{
    const uint32_t w = src->width;
    const uint32_t h = src->height;
    const uint32_t src_stride = src->pitch / 4u;
    const uint32_t dst_stride = dst->pitch / 4u;

    if (radius <= 0) {
        for (uint32_t y = 0; y < h; y++)
            memcpy(&dst->buffer[(size_t)y * dst_stride], &src->buffer[(size_t)y * src_stride],
                   (size_t)w * sizeof(uint32_t));
        return;
    }

    const uint32_t window = (uint32_t)radius * 2u + 1u;
    const BlurReciprocal recip32 = make_blur_reciprocal(window);

    if (window <= (uint32_t)BLUR_MAX_SIMD_WINDOW) {
        const __m128i recip16 = _mm_set1_epi16(static_cast<short>(blur_recip16(window)));
        uint32_t y = 0;
        for (; y + 2 <= h; y += 2)
            box_blur_rows_pair(&src->buffer[(size_t)y * src_stride], &src->buffer[(size_t)(y + 1) * src_stride],
                               &dst->buffer[(size_t)y * dst_stride], &dst->buffer[(size_t)(y + 1) * dst_stride], w,
                               radius, recip16);
        if (y < h)
            box_blur_row_scalar(&src->buffer[(size_t)y * src_stride], &dst->buffer[(size_t)y * dst_stride], w, radius,
                                recip32);
        return;
    }

    for (uint32_t y = 0; y < h; y++)
        box_blur_row_scalar(&src->buffer[(size_t)y * src_stride], &dst->buffer[(size_t)y * dst_stride], w, radius,
                            recip32);
}

// Per-pixel saturation/brightness on interleaved u16 BGRA lanes (2 pixels).
// luma = (r*54 + g*183 + b*19 + 128) >> 8; c' = luma + (c-luma)*sat/100 + bias.
// The alpha lane is garbage on output; callers force 0xFF at pack time.
static inline __m128i material_adjust_u16(__m128i q, __m128i coeffs, __m128i sat_q14, __m128i bias16)
{
    __m128i v = _mm_madd_epi16(q, coeffs);
    __m128i luma32 = _mm_add_epi32(v, _mm_srli_si128(v, 4));
    luma32 = _mm_srli_epi32(_mm_add_epi32(luma32, _mm_set1_epi32(128)), 8);
    __m128i xx = _mm_shuffle_epi32(luma32, _MM_SHUFFLE(2, 2, 0, 0));
    __m128i luma16 = _mm_packs_epi32(xx, xx);
    // Sequential shuffles (never OR'd): replicate pixel0 luma across the low
    // qword lanes and pixel1 luma across the high qword lanes.
    __m128i luma_rep = _mm_shufflelo_epi16(luma16, _MM_SHUFFLE(0, 0, 0, 0));
    luma_rep = _mm_shufflehi_epi16(luma_rep, _MM_SHUFFLE(2, 2, 2, 2));

    __m128i d = _mm_sub_epi16(q, luma_rep);
    __m128i scaled = _mm_slli_epi16(_mm_mulhi_epi16(d, sat_q14), 2);
    __m128i c = _mm_add_epi16(_mm_add_epi16(luma_rep, scaled), bias16);
    __m128i neg = _mm_cmpgt_epi16(_mm_setzero_si128(), c);
    return _mm_andnot_si128(neg, c);
}

static inline int material_sat_q14(int saturation_pct)
{
    if (saturation_pct < 0)
        saturation_pct = 0;
    if (saturation_pct > 199)
        saturation_pct = 199;
    return (saturation_pct * 16384 + 50) / 100;
}

static inline uint32_t material_adjust_pixel(uint32_t pixel, int saturation_pct, int brightness_bias)
{
    int r = (int)((pixel >> 16) & 0xFFu);
    int g = (int)((pixel >> 8) & 0xFFu);
    int b = (int)(pixel & 0xFFu);
    int luma = (r * 54 + g * 183 + b * 19 + 128) / 256;
    r = luma + ((r - luma) * saturation_pct + 50) / 100 + brightness_bias;
    g = luma + ((g - luma) * saturation_pct + 50) / 100 + brightness_bias;
    b = luma + ((b - luma) * saturation_pct + 50) / 100 + brightness_bias;
    return 0xFF000000u | ((uint32_t)clamp_material_channel(r) << 16) | ((uint32_t)clamp_material_channel(g) << 8) |
           clamp_material_channel(b);
}

static void material_rows(const uint32_t *srow, uint32_t *drow, uint32_t w, const MaterialAdjust &mat)
{
    const __m128i coeffs = _mm_set_epi16(0, 54, 183, 19, 0, 54, 183, 19);
    const __m128i sat_q14 = _mm_set1_epi16(static_cast<short>(material_sat_q14(mat.saturation_pct)));
    const __m128i bias16 = _mm_set1_epi16(static_cast<short>(mat.brightness_bias));
    const __m128i alpha = blur_alpha_mask32();

    uint32_t x = 0;
    for (; x + 8 <= w; x += 8) {
        __m128i q[4];
        load_8px_u16(srow + x, q);
        q[0] = material_adjust_u16(q[0], coeffs, sat_q14, bias16);
        q[1] = material_adjust_u16(q[1], coeffs, sat_q14, bias16);
        q[2] = material_adjust_u16(q[2], coeffs, sat_q14, bias16);
        q[3] = material_adjust_u16(q[3], coeffs, sat_q14, bias16);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(drow + x), _mm_or_si128(_mm_packus_epi16(q[0], q[1]), alpha));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(drow + x + 4), _mm_or_si128(_mm_packus_epi16(q[2], q[3]), alpha));
    }
    for (; x < w; x++)
        drow[x] = material_adjust_pixel(srow[x], mat.saturation_pct, mat.brightness_bias);
}

static void box_blur_column_scalar(const Surface *src, Surface *dst, uint32_t x, int radius,
                                   const BlurReciprocal &recip, const MaterialAdjust *mat)
{
    const uint32_t h = src->height;
    const uint32_t src_stride = src->pitch / 4u;
    const uint32_t dst_stride = dst->pitch / 4u;

    uint32_t first = src->buffer[x];
    int32_t sum_r = (int32_t)(((first >> 16) & 0xFFu) * (uint32_t)(radius + 1));
    int32_t sum_g = (int32_t)(((first >> 8) & 0xFFu) * (uint32_t)(radius + 1));
    int32_t sum_b = (int32_t)((first & 0xFFu) * (uint32_t)(radius + 1));
    for (int s = 1; s <= radius; s++) {
        int idx = s < (int)h ? s : (int)h - 1;
        uint32_t pixel = src->buffer[(size_t)idx * src_stride + x];
        sum_r += (int32_t)((pixel >> 16) & 0xFFu);
        sum_g += (int32_t)((pixel >> 8) & 0xFFu);
        sum_b += (int32_t)(pixel & 0xFFu);
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t out = 0xFF000000u | (blur_div((uint32_t)sum_r, recip) << 16) |
                       (blur_div((uint32_t)sum_g, recip) << 8) | blur_div((uint32_t)sum_b, recip);
        dst->buffer[(size_t)y * dst_stride + x] =
            mat ? material_adjust_pixel(out, mat->saturation_pct, mat->brightness_bias) : out;

        int rem = (int)y - radius;
        if (rem < 0)
            rem = 0;
        int add = (int)y + radius + 1;
        if (add >= (int)h)
            add = (int)h - 1;
        uint32_t rp = src->buffer[(size_t)rem * src_stride + x];
        uint32_t ap = src->buffer[(size_t)add * src_stride + x];
        sum_r += (int32_t)((ap >> 16) & 0xFFu) - (int32_t)((rp >> 16) & 0xFFu);
        sum_g += (int32_t)((ap >> 8) & 0xFFu) - (int32_t)((rp >> 8) & 0xFFu);
        sum_b += (int32_t)(ap & 0xFFu) - (int32_t)(rp & 0xFFu);
    }
}

static void box_blur_vertical(const Surface *src, Surface *dst, int radius, const MaterialAdjust *mat)
{
    const uint32_t w = src->width;
    const uint32_t h = src->height;
    const uint32_t src_stride = src->pitch / 4u;
    const uint32_t dst_stride = dst->pitch / 4u;

    if (radius <= 0) {
        if (mat) {
            for (uint32_t y = 0; y < h; y++)
                material_rows(&src->buffer[(size_t)y * src_stride], &dst->buffer[(size_t)y * dst_stride], w, *mat);
        } else {
            for (uint32_t y = 0; y < h; y++)
                memcpy(&dst->buffer[(size_t)y * dst_stride], &src->buffer[(size_t)y * src_stride],
                       (size_t)w * sizeof(uint32_t));
        }
        return;
    }

    const uint32_t window = (uint32_t)radius * 2u + 1u;
    const BlurReciprocal recip32 = make_blur_reciprocal(window);
    const bool simd =
        window <= (uint32_t)BLUR_MAX_SIMD_WINDOW && (size_t)w * 4 <= sizeof(g_blur_vstate) / sizeof(g_blur_vstate[0]);

    if (!simd) {
        for (uint32_t x = 0; x < w; x++)
            box_blur_column_scalar(src, dst, x, radius, recip32, mat);
        return;
    }

    const __m128i recip16 = _mm_set1_epi16(static_cast<short>(blur_recip16(window)));
    const __m128i alpha = blur_alpha_mask32();
    const bool has_mat = mat != nullptr;
    const __m128i coeffs = _mm_set_epi16(0, 54, 183, 19, 0, 54, 183, 19);
    const __m128i sat_q14 =
        has_mat ? _mm_set1_epi16(static_cast<short>(material_sat_q14(mat->saturation_pct))) : _mm_setzero_si128();
    const __m128i bias16 = has_mat ? _mm_set1_epi16(static_cast<short>(mat->brightness_bias)) : _mm_setzero_si128();

    const uint32_t simd_w = w & ~7u;

    // Initialize per-column window sums for output row 0.
    for (uint32_t x0 = 0; x0 < simd_w; x0 += 8) {
        __m128i st[4];
        load_8px_u16(&src->buffer[x0], st);
        const __m128i weight0 = _mm_set1_epi16(static_cast<short>(radius + 1));
        st[0] = _mm_mullo_epi16(st[0], weight0);
        st[1] = _mm_mullo_epi16(st[1], weight0);
        st[2] = _mm_mullo_epi16(st[2], weight0);
        st[3] = _mm_mullo_epi16(st[3], weight0);
        for (int s = 1; s <= radius; s++) {
            int idx = s < (int)h ? s : (int)h - 1;
            __m128i r[4];
            load_8px_u16(&src->buffer[(size_t)idx * src_stride + x0], r);
            st[0] = _mm_add_epi16(st[0], r[0]);
            st[1] = _mm_add_epi16(st[1], r[1]);
            st[2] = _mm_add_epi16(st[2], r[2]);
            st[3] = _mm_add_epi16(st[3], r[3]);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 0]), st[0]);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 8]), st[1]);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 16]), st[2]);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 24]), st[3]);
    }

    for (uint32_t y = 0; y < h; y++) {
        uint32_t *drow = &dst->buffer[(size_t)y * dst_stride];
        int in_idx = (int)y + radius + 1;
        if (in_idx >= (int)h)
            in_idx = (int)h - 1;
        int out_idx = (int)y - radius;
        if (out_idx < 0)
            out_idx = 0;
        const uint32_t *in_row = &src->buffer[(size_t)in_idx * src_stride];
        const uint32_t *out_row = &src->buffer[(size_t)out_idx * src_stride];

        for (uint32_t x0 = 0; x0 < simd_w; x0 += 8) {
            __m128i st[4];
            st[0] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&g_blur_vstate[x0 * 4 + 0]));
            st[1] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&g_blur_vstate[x0 * 4 + 8]));
            st[2] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&g_blur_vstate[x0 * 4 + 16]));
            st[3] = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&g_blur_vstate[x0 * 4 + 24]));

            __m128i q0 = _mm_mulhi_epu16(st[0], recip16);
            __m128i q1 = _mm_mulhi_epu16(st[1], recip16);
            __m128i q2 = _mm_mulhi_epu16(st[2], recip16);
            __m128i q3 = _mm_mulhi_epu16(st[3], recip16);

            if (has_mat) {
                q0 = material_adjust_u16(q0, coeffs, sat_q14, bias16);
                q1 = material_adjust_u16(q1, coeffs, sat_q14, bias16);
                q2 = material_adjust_u16(q2, coeffs, sat_q14, bias16);
                q3 = material_adjust_u16(q3, coeffs, sat_q14, bias16);
            }

            _mm_storeu_si128(reinterpret_cast<__m128i *>(drow + x0), _mm_or_si128(_mm_packus_epi16(q0, q1), alpha));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(drow + x0 + 4), _mm_or_si128(_mm_packus_epi16(q2, q3), alpha));

            if (y + 1 < h) {
                __m128i in_px[4], out_px[4];
                load_8px_u16(in_row + x0, in_px);
                load_8px_u16(out_row + x0, out_px);
                st[0] = _mm_add_epi16(st[0], _mm_sub_epi16(in_px[0], out_px[0]));
                st[1] = _mm_add_epi16(st[1], _mm_sub_epi16(in_px[1], out_px[1]));
                st[2] = _mm_add_epi16(st[2], _mm_sub_epi16(in_px[2], out_px[2]));
                st[3] = _mm_add_epi16(st[3], _mm_sub_epi16(in_px[3], out_px[3]));
                _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 0]), st[0]);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 8]), st[1]);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 16]), st[2]);
                _mm_storeu_si128(reinterpret_cast<__m128i *>(&g_blur_vstate[x0 * 4 + 24]), st[3]);
            }
        }
    }

    // Tail columns (and full fallback for narrow surfaces).
    for (uint32_t x = simd_w; x < w; x++)
        box_blur_column_scalar(src, dst, x, radius, recip32, mat);
}

void blur_surface_box(const Surface *src, Surface *dst, int radius)
{
    if (!src || !dst || !src->buffer || !dst->buffer || src->width != dst->width || src->height != dst->height)
        return;
    const uint32_t w = src->width;
    const uint32_t h = src->height;
    if (w == 0 || h == 0)
        return;
    if (radius <= 0) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }
    int radius_w = radius;
    int radius_h = radius;
    if ((uint32_t)radius_w >= w)
        radius_w = (int)w - 1;
    if ((uint32_t)radius_h >= h)
        radius_h = (int)h - 1;
    if (radius_w <= 0 && radius_h <= 0) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }
    if (!ensure_surface_capacity(&g_blur_scratch, w, h)) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }

    box_blur_horizontal(src, &g_blur_scratch, radius_w);
    box_blur_vertical(&g_blur_scratch, dst, radius_h, nullptr);
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

// Third blur pass fused with saturation/brightness adjustment.
static void blur_surface_box_fused(const Surface *src, Surface *dst, int radius, int saturation_pct,
                                   int brightness_bias)
{
    if (!src || !dst || !src->buffer || !dst->buffer || src->width != dst->width || src->height != dst->height)
        return;
    const uint32_t w = src->width;
    const uint32_t h = src->height;
    if (w == 0 || h == 0)
        return;
    MaterialAdjust mat = {saturation_pct, brightness_bias};
    if (radius <= 0) {
        for (uint32_t y = 0; y < h; y++)
            material_rows(&src->buffer[(size_t)y * (src->pitch / 4u)], &dst->buffer[(size_t)y * (dst->pitch / 4u)], w,
                          mat);
        return;
    }
    int radius_w = radius;
    int radius_h = radius;
    if ((uint32_t)radius_w >= w)
        radius_w = (int)w - 1;
    if ((uint32_t)radius_h >= h)
        radius_h = (int)h - 1;
    if (radius_w <= 0)
        radius_w = 1;
    if (radius_h <= 0)
        radius_h = 1;
    if (!ensure_surface_capacity(&g_blur_scratch, w, h)) {
        copy_surface_rect(dst, 0, 0, src, 0, 0, (int)w, (int)h);
        return;
    }

    box_blur_horizontal(src, &g_blur_scratch, radius_w);
    box_blur_vertical(&g_blur_scratch, dst, radius_h, &mat);
}

static void postprocess_material_surface(Surface *surface, int saturation_pct, int brightness_bias)
{
    if (!surface || !surface->buffer)
        return;
    MaterialAdjust mat = {saturation_pct, brightness_bias};
    uint32_t stride = surface->pitch / 4u;
    for (uint32_t y = 0; y < surface->height; y++)
        material_rows(&surface->buffer[(size_t)y * stride], &surface->buffer[(size_t)y * stride], surface->width, mat);
}

// Edge-replicated box average downsample. Every output pixel averages exactly
// factor*factor samples (edge pixels duplicated), so a shift divides exactly.
static void downsample_box(const Surface *src, Surface *dst, int factor)
{
    if (!src || !dst || !src->buffer || !dst->buffer || factor <= 1)
        return;
    const uint32_t sw = src->width, sh = src->height;
    const uint32_t dw = dst->width, dh = dst->height;
    const uint32_t ss = src->pitch / 4u, ds = dst->pitch / 4u;
    const uint32_t ff = (uint32_t)factor * (uint32_t)factor;
    const BlurReciprocal recip = make_blur_reciprocal(ff);
    const uint32_t full_cols = sw / (uint32_t)factor;
    const int avg_shift = (factor == 4) ? 4 : 2;

    for (uint32_t dy = 0; dy < dh; dy++) {
        uint32_t sy_base = dy * (uint32_t)factor;
        const uint32_t *srows[4];
        for (int k = 0; k < factor; k++) {
            uint32_t sy = sy_base + (uint32_t)k;
            if (sy >= sh)
                sy = sh - 1;
            srows[k] = &src->buffer[(size_t)sy * ss];
        }
        uint32_t *drow = &dst->buffer[(size_t)dy * ds];

        uint32_t dx = 0;
        if (factor == 4) {
            for (; dx + 2 <= full_cols; dx += 2) {
                __m128i r[4];
                load_8px_u16(srows[0] + (size_t)dx * 4u, r);
                for (int k = 1; k < 4; k++) {
                    __m128i t[4];
                    load_8px_u16(srows[k] + (size_t)dx * 4u, t);
                    r[0] = _mm_add_epi16(r[0], t[0]);
                    r[1] = _mm_add_epi16(r[1], t[1]);
                    r[2] = _mm_add_epi16(r[2], t[2]);
                    r[3] = _mm_add_epi16(r[3], t[3]);
                }
                // Fold each adjacent column pair into per-pixel sums; the swap
                // moves whole pixels (two epi32 lanes each), so channels stay
                // aligned and both 64-bit halves carry the same sum.
                __m128i s01 = _mm_add_epi16(r[0], _mm_shuffle_epi32(r[0], _MM_SHUFFLE(1, 0, 3, 2)));
                __m128i s23 = _mm_add_epi16(r[1], _mm_shuffle_epi32(r[1], _MM_SHUFFLE(1, 0, 3, 2)));
                __m128i s45 = _mm_add_epi16(r[2], _mm_shuffle_epi32(r[2], _MM_SHUFFLE(1, 0, 3, 2)));
                __m128i s67 = _mm_add_epi16(r[3], _mm_shuffle_epi32(r[3], _MM_SHUFFLE(1, 0, 3, 2)));
                __m128i px_a = _mm_srli_epi16(_mm_add_epi16(s01, s23), avg_shift);
                __m128i px_b = _mm_srli_epi16(_mm_add_epi16(s45, s67), avg_shift);
                uint32_t px[4] = {};
                _mm_storeu_si128(reinterpret_cast<__m128i *>(px), _mm_packus_epi16(px_a, px_b));
                drow[dx] = px[0] | 0xFF000000u;
                drow[dx + 1] = px[2] | 0xFF000000u;
            }
        } else {
            const __m128i zero = _mm_setzero_si128();
            for (; dx + 2 <= full_cols; dx += 2) {
                __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srows[0] + (size_t)dx * 2u));
                __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srows[1] + (size_t)dx * 2u));
                __m128i r0 = _mm_add_epi16(_mm_unpacklo_epi8(a, zero), _mm_unpacklo_epi8(b, zero));
                __m128i r1 = _mm_add_epi16(_mm_unpackhi_epi8(a, zero), _mm_unpackhi_epi8(b, zero));
                r0 = _mm_srli_epi16(_mm_add_epi16(r0, _mm_shuffle_epi32(r0, _MM_SHUFFLE(1, 0, 3, 2))), avg_shift);
                r1 = _mm_srli_epi16(_mm_add_epi16(r1, _mm_shuffle_epi32(r1, _MM_SHUFFLE(1, 0, 3, 2))), avg_shift);
                uint32_t px[4] = {};
                _mm_storeu_si128(reinterpret_cast<__m128i *>(px), _mm_packus_epi16(r0, r1));
                drow[dx] = px[0] | 0xFF000000u;
                drow[dx + 1] = px[2] | 0xFF000000u;
            }
        }

        for (; dx < dw; dx++) {
            uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
            for (int k = 0; k < factor; k++) {
                const uint32_t *srow = srows[k];
                for (int cx = 0; cx < factor; cx++) {
                    uint32_t sx = dx * (uint32_t)factor + (uint32_t)cx;
                    if (sx >= sw)
                        sx = sw - 1;
                    uint32_t p = srow[sx];
                    sum_r += (p >> 16) & 0xFFu;
                    sum_g += (p >> 8) & 0xFFu;
                    sum_b += p & 0xFFu;
                }
            }
            drow[dx] =
                0xFF000000u | (blur_div(sum_r, recip) << 16) | (blur_div(sum_g, recip) << 8) | blur_div(sum_b, recip);
        }
    }
}

// Center-aligned bilinear upsample: destination pixel (x, y) maps to source
// coordinate ((x + 0.5) / factor - 0.5), keeping the upscaled backdrop in
// register with the content it was captured from.
static void upsample_bilinear(const Surface *src, Surface *dst, int factor)
{
    if (!src || !src->buffer || !dst || !dst->buffer || factor <= 1)
        return;
    const uint32_t sw = src->width, sh = src->height;
    const uint32_t dw = dst->width, dh = dst->height;
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return;
    const uint32_t ss = src->pitch / 4u, ds = dst->pitch / 4u;
    const int fshift = (factor == 4) ? 2 : 1;
    const int coord_shift = 15 - fshift;

    for (uint32_t y = 0; y < dh; y++) {
        uint32_t *drow = &dst->buffer[(size_t)y * ds];

        int64_t fy = ((int64_t)(2 * y + 1) << coord_shift) - 32768;
        if (fy < 0)
            fy = 0;
        uint32_t iy0 = (uint32_t)(fy >> 16);
        if (iy0 >= sh)
            iy0 = sh - 1;
        uint32_t iy1 = (iy0 + 1 < sh) ? iy0 + 1 : iy0;
        uint32_t ify = (uint32_t)((fy >> 8) & 0xFF);
        const uint32_t *srow0 = &src->buffer[(size_t)iy0 * ss];
        const uint32_t *srow1 = &src->buffer[(size_t)iy1 * ss];

        for (uint32_t x = 0; x < dw; x++) {
            int64_t fx = ((int64_t)(2 * x + 1) << coord_shift) - 32768;
            if (fx < 0)
                fx = 0;
            uint32_t ix0 = (uint32_t)(fx >> 16);
            if (ix0 >= sw)
                ix0 = sw - 1;
            uint32_t ix1 = (ix0 + 1 < sw) ? ix0 + 1 : ix0;
            uint32_t ifx = (uint32_t)((fx >> 8) & 0xFF);

            uint32_t p00 = srow0[ix0], p10 = srow0[ix1];
            uint32_t p01 = srow1[ix0], p11 = srow1[ix1];

            uint32_t w00 = (256u - ifx) * (256u - ify);
            uint32_t w10 = ifx * (256u - ify);
            uint32_t w01 = (256u - ifx) * ify;
            uint32_t w11 = ifx * ify;

            auto interp = [&](int shift) {
                return (uint32_t)((((p00 >> shift) & 0xFFu) * w00 + ((p10 >> shift) & 0xFFu) * w10 +
                                   ((p01 >> shift) & 0xFFu) * w01 + ((p11 >> shift) & 0xFFu) * w11 + 32768u) >>
                                  16);
            };

            drow[x] = 0xFF000000u | (interp(16) << 16) | (interp(8) << 8) | interp(0);
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

    // Choose downsample factor based on dimensions for performance.
    int downsample_factor = 0;
    uint32_t small_w = 0, small_h = 0;
    auto try_factor = [&](int factor) -> bool {
        if (factor <= 1)
            return false;
        uint32_t cw = (src->width + (uint32_t)factor - 1u) / (uint32_t)factor;
        uint32_t ch = (src->height + (uint32_t)factor - 1u) / (uint32_t)factor;
        // Ensure minimum dimensions for quality.
        if (cw < 32u || ch < 8u)
            return false;
        if (!ensure_surface_capacity(&g_blur_small_src, cw, ch) ||
            !ensure_surface_capacity(&g_blur_small_dst, cw, ch) || !ensure_surface_capacity(&g_blur_pass_a, cw, ch) ||
            !ensure_surface_capacity(&g_blur_pass_b, cw, ch))
            return false;
        downsample_factor = factor;
        small_w = cw;
        small_h = ch;
        return true;
    };

    if (src->width >= 256 && src->height >= 16) {
        if (!try_factor(4) && !try_factor(2))
            downsample_factor = 0;
    }

    if (downsample_factor > 1) {
        downsample_box(src, &g_blur_small_src, downsample_factor);

        float adjusted_sigma = sigma / (float)downsample_factor;
        int small_radii[3] = {};
        compute_gaussian_box_radii(adjusted_sigma, small_radii);

        blur_surface_box(&g_blur_small_src, &g_blur_pass_a, small_radii[0]);
        blur_surface_box(&g_blur_pass_a, &g_blur_pass_b, small_radii[1]);
        blur_surface_box_fused(&g_blur_pass_b, &g_blur_small_dst, small_radii[2], saturation_pct, brightness_bias);

        upsample_bilinear(&g_blur_small_dst, dst, downsample_factor);

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

// --- Self-test ------------------------------------------------------------

static uint32_t blur_test_lcg(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state;
}

// Naive edge-replicated box blur reference with exact division.
static void ref_box_blur(const uint32_t *src, uint32_t ss, uint32_t *tmp, uint32_t *dst, uint32_t ds, uint32_t w,
                         uint32_t h, int radius)
{
    int rx = radius < (int)w ? radius : (int)w - 1;
    int ry = radius < (int)h ? radius : (int)h - 1;
    if (rx < 0)
        rx = 0;
    if (ry < 0)
        ry = 0;

    if (rx == 0) {
        for (uint32_t y = 0; y < h; y++)
            memcpy(&tmp[(size_t)y * w], &src[(size_t)y * ss], (size_t)w * sizeof(uint32_t));
    } else {
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *row = &src[(size_t)y * ss];
            uint32_t *trow = &tmp[(size_t)y * w];
            for (uint32_t x = 0; x < w; x++) {
                uint32_t sr = 0, sg = 0, sb = 0;
                for (int k = -rx; k <= rx; k++) {
                    int xi = (int)x + k;
                    if (xi < 0)
                        xi = 0;
                    if (xi >= (int)w)
                        xi = (int)w - 1;
                    uint32_t p = row[xi];
                    sr += (p >> 16) & 0xFFu;
                    sg += (p >> 8) & 0xFFu;
                    sb += p & 0xFFu;
                }
                uint32_t win = (uint32_t)(2 * rx + 1);
                trow[x] = 0xFF000000u | ((sr / win) << 16) | ((sg / win) << 8) | (sb / win);
            }
        }
    }

    if (ry == 0) {
        for (uint32_t y = 0; y < h; y++)
            memcpy(&dst[(size_t)y * ds], &tmp[(size_t)y * w], (size_t)w * sizeof(uint32_t));
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *drow = &dst[(size_t)y * ds];
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int k = -ry; k <= ry; k++) {
                int yi = (int)y + k;
                if (yi < 0)
                    yi = 0;
                if (yi >= (int)h)
                    yi = (int)h - 1;
                uint32_t p = tmp[(size_t)yi * w + x];
                sr += (p >> 16) & 0xFFu;
                sg += (p >> 8) & 0xFFu;
                sb += p & 0xFFu;
            }
            uint32_t win = (uint32_t)(2 * ry + 1);
            drow[x] = 0xFF000000u | ((sr / win) << 16) | ((sg / win) << 8) | (sb / win);
        }
    }
}

static bool blur_max_channel_diff(const uint32_t *a, const uint32_t *b, uint32_t count, int tolerance)
{
    for (uint32_t i = 0; i < count; i++) {
        if ((a[i] | 0xFF000000u) == (b[i] | 0xFF000000u))
            continue;
        for (int shift = 0; shift <= 16; shift += 8) {
            int da = (int)((a[i] >> shift) & 0xFFu);
            int db = (int)((b[i] >> shift) & 0xFFu);
            int diff = da - db;
            if (diff < 0)
                diff = -diff;
            if (diff > tolerance)
                return false;
        }
    }
    return true;
}

bool blur_self_test(void)
{
    static const uint32_t TW = 256;
    static const uint32_t TH = 32;
    static uint32_t src_buf[TW * TH];
    static uint32_t dst_buf[TW * TH];
    static uint32_t ref_buf[TW * TH];
    static uint32_t tmp_buf[TW * TH];
    static uint32_t small_a[TW * TH];
    static uint32_t small_b[TW * TH];
    static uint32_t small_c[TW * TH];

    Surface src = {};
    src.buffer = src_buf;
    src.width = TW;
    src.height = TH;
    src.pitch = TW * 4u;
    Surface dst = {};
    dst.buffer = dst_buf;
    dst.width = TW;
    dst.height = TH;
    dst.pitch = TW * 4u;

    // 1. Box blur of a constant surface must reproduce it exactly.
    for (uint32_t i = 0; i < TW * TH; i++)
        src_buf[i] = 0xFF3C7A21u;
    memset(dst_buf, 0, sizeof(dst_buf));
    blur_surface_box(&src, &dst, 6);
    for (uint32_t i = 0; i < TW * TH; i++) {
        if ((dst_buf[i] | 0xFF000000u) != 0xFF3C7A21u)
            return false;
    }

    // 2. Material blur of a flat gray stays flat: luma == channel, saturation
    // contributes nothing, only the brightness bias may shift it.
    for (uint32_t i = 0; i < TW * TH; i++)
        src_buf[i] = 0xFF808080u;
    memset(dst_buf, 0, sizeof(dst_buf));
    blur_surface_material(&src, &dst, 56.0f, 140, 10);
    for (uint32_t i = 0; i < TW * TH; i++) {
        if ((dst_buf[i] | 0xFF000000u) != 0xFF8A8A8Au)
            return false;
    }

    // 3. SIMD pipeline vs naive exact reference on random content.
    uint32_t state = 0xB1A57u;
    for (uint32_t i = 0; i < TW * TH; i++)
        src_buf[i] = blur_test_lcg(state) | 0xFF000000u;

    memset(dst_buf, 0, sizeof(dst_buf));
    blur_surface_material(&src, &dst, 56.0f, 140, 10);

    // Reference pipeline: downsample, three box passes, material, upsample.
    const int F = 4;
    const uint32_t rw = (TW + F - 1) / F;
    const uint32_t rh = (TH + F - 1) / F;

    for (uint32_t dy = 0; dy < rh; dy++) {
        for (uint32_t dx = 0; dx < rw; dx++) {
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int ky = 0; ky < F; ky++) {
                uint32_t sy = dy * F + (uint32_t)ky;
                if (sy >= TH)
                    sy = TH - 1;
                for (int kx = 0; kx < F; kx++) {
                    uint32_t sx = dx * F + (uint32_t)kx;
                    if (sx >= TW)
                        sx = TW - 1;
                    uint32_t p = src_buf[(size_t)sy * TW + sx];
                    sr += (p >> 16) & 0xFFu;
                    sg += (p >> 8) & 0xFFu;
                    sb += p & 0xFFu;
                }
            }
            small_a[(size_t)dy * rw + dx] =
                0xFF000000u | ((sr / (F * F)) << 16) | ((sg / (F * F)) << 8) | (sb / (F * F));
        }
    }

    int radii[3] = {};
    compute_gaussian_box_radii(56.0f / (float)F, radii);
    ref_box_blur(small_a, rw, tmp_buf, small_b, rw, rw, rh, radii[0]);
    ref_box_blur(small_b, rw, tmp_buf, small_c, rw, rw, rh, radii[1]);
    ref_box_blur(small_c, rw, tmp_buf, small_b, rw, rw, rh, radii[2]);
    for (uint32_t i = 0; i < rw * rh; i++)
        small_b[i] = material_adjust_pixel(small_b[i], 140, 10);

    for (uint32_t y = 0; y < TH; y++) {
        for (uint32_t x = 0; x < TW; x++) {
            int64_t fy = ((int64_t)(2 * y + 1) << 13) - 32768;
            if (fy < 0)
                fy = 0;
            int64_t fx = ((int64_t)(2 * x + 1) << 13) - 32768;
            if (fx < 0)
                fx = 0;
            uint32_t iy0 = (uint32_t)(fy >> 16);
            if (iy0 >= rh)
                iy0 = rh - 1;
            uint32_t iy1 = (iy0 + 1 < rh) ? iy0 + 1 : iy0;
            uint32_t ix0 = (uint32_t)(fx >> 16);
            if (ix0 >= rw)
                ix0 = rw - 1;
            uint32_t ix1 = (ix0 + 1 < rw) ? ix0 + 1 : ix0;
            uint32_t ify = (uint32_t)((fy >> 8) & 0xFF);
            uint32_t ifx = (uint32_t)((fx >> 8) & 0xFF);
            uint32_t w00 = (256u - ifx) * (256u - ify);
            uint32_t w10 = ifx * (256u - ify);
            uint32_t w01 = (256u - ifx) * ify;
            uint32_t w11 = ifx * ify;
            uint32_t p00 = small_b[(size_t)iy0 * rw + ix0];
            uint32_t p10 = small_b[(size_t)iy0 * rw + ix1];
            uint32_t p01 = small_b[(size_t)iy1 * rw + ix0];
            uint32_t p11 = small_b[(size_t)iy1 * rw + ix1];
            uint32_t out = 0xFF000000u;
            for (int shift = 0; shift <= 16; shift += 8) {
                uint32_t v = ((((p00 >> shift) & 0xFFu) * w00 + ((p10 >> shift) & 0xFFu) * w10 +
                               ((p01 >> shift) & 0xFFu) * w01 + ((p11 >> shift) & 0xFFu) * w11 + 32768u) >>
                              16);
                out |= v << shift;
            }
            ref_buf[(size_t)y * TW + x] = out;
        }
    }
    if (!blur_max_channel_diff(dst_buf, ref_buf, TW * TH, 6))
        return false;

    // 4. Plain box blur vs exact reference at a few radii.
    static const int radii_set[] = {1, 3, 7};
    for (int radius : radii_set) {
        memset(dst_buf, 0, sizeof(dst_buf));
        blur_surface_box(&src, &dst, radius);
        ref_box_blur(src_buf, TW, tmp_buf, ref_buf, TW, TW, TH, radius);
        if (!blur_max_channel_diff(dst_buf, ref_buf, TW * TH, 2))
            return false;
    }

    return true;
}
