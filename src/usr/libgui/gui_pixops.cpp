#include "gui_pixops.h"

#include <emmintrin.h>

#include "gui_canvas_utils.h"

void pix_copy_row(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;

    uint32_t i = 0;
    while (i + 4 <= count) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), v);
        i += 4;
    }
    for (; i < count; i++)
        dst[i] = src[i];
}

void pix_fill_row(uint32_t *dst, uint32_t count, uint32_t color)
{
    if (!dst || count == 0)
        return;

    __m128i v = _mm_set1_epi32(static_cast<int>(color));
    uint32_t i = 0;
    while (i + 4 <= count) {
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), v);
        i += 4;
    }
    for (; i < count; i++)
        dst[i] = color;
}

static inline __m128i blend_premult_4(__m128i d, __m128i s)
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i c255 = _mm_set1_epi16(255);
    const __m128i c128 = _mm_set1_epi16(128);

    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);
    __m128i s_lo = _mm_unpacklo_epi8(s, zero);
    __m128i s_hi = _mm_unpackhi_epi8(s, zero);

    // Each channel is a zero-extended 16-bit lane; a pixel's alpha sits in
    // lane 3 of its qword. Broadcast it across all four channel lanes.
    __m128i a_lo = _mm_shufflehi_epi16(_mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
    __m128i a_hi = _mm_shufflehi_epi16(_mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));

    __m128i inv_lo = _mm_sub_epi16(c255, a_lo);
    __m128i inv_hi = _mm_sub_epi16(c255, a_hi);

    // dst * (255 - a) with the /255 rounding approximation; max lane value
    // 255*255 + 128 fits in 16 bits.
    __m128i t_lo = _mm_add_epi16(_mm_mullo_epi16(d_lo, inv_lo), c128);
    t_lo = _mm_srli_epi16(_mm_add_epi16(t_lo, _mm_srli_epi16(t_lo, 8)), 8);
    __m128i t_hi = _mm_add_epi16(_mm_mullo_epi16(d_hi, inv_hi), c128);
    t_hi = _mm_srli_epi16(_mm_add_epi16(t_hi, _mm_srli_epi16(t_hi, 8)), 8);

    __m128i out_lo = _mm_add_epi16(s_lo, t_lo);
    __m128i out_hi = _mm_add_epi16(s_hi, t_hi);
    return _mm_packus_epi16(out_lo, out_hi);
}

void pix_blend_row_premultiplied(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;

    uint32_t i = 0;
    while (i + 4 <= count) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), blend_premult_4(d, s));
        i += 4;
    }
    for (; i < count; i++)
        dst[i] = gui_blend_premultiplied(dst[i], src[i]);
}

void pix_blend_row_premultiplied_opaque_dst(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;

    const __m128i opaque = _mm_set1_epi32(static_cast<int>(0xFF000000u));
    uint32_t i = 0;
    while (i + 4 <= count) {
        __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dst + i));
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + i), _mm_or_si128(blend_premult_4(d, s), opaque));
        i += 4;
    }
    for (; i < count; i++)
        dst[i] = gui_blend_premultiplied_opaque_dst(dst[i], src[i]);
}

void pix_blend_row_premultiplied_ref(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;
    for (uint32_t i = 0; i < count; i++)
        dst[i] = gui_blend_premultiplied(dst[i], src[i]);
}

void pix_blend_row_premultiplied_opaque_dst_ref(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;
    for (uint32_t i = 0; i < count; i++)
        dst[i] = gui_blend_premultiplied_opaque_dst(dst[i], src[i]);
}

namespace {

constexpr uint32_t kSelfTestMax = 512;

uint32_t pixops_lcg(uint32_t &state)
{
    state = state * 1664525u + 1013904223u;
    return state >> 16;
}

uint32_t make_premultiplied_pixel(uint32_t rnd)
{
    uint32_t alpha = rnd & 0xFFu;
    uint32_t r = (rnd >> 8) % (alpha + 1u);
    uint32_t g = (rnd >> 16) % (alpha + 1u);
    uint32_t b = (rnd >> 24) % (alpha + 1u);
    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

bool run_blend_case(uint32_t width, bool opaque_dst, bool use_seed_dst)
{
    if (width == 0 || width > kSelfTestMax)
        return false;

    static uint32_t dst_a[kSelfTestMax];
    static uint32_t dst_b[kSelfTestMax];
    static uint32_t src[kSelfTestMax];

    uint32_t state =
        0xC0FFEEu ^ (static_cast<uint32_t>(width) << 3) ^ (opaque_dst ? 2u : 0u) ^ (use_seed_dst ? 4u : 0u);
    for (uint32_t i = 0; i < width; i++) {
        uint32_t rnd = pixops_lcg(state);
        src[i] = make_premultiplied_pixel(rnd);
        uint32_t d = pixops_lcg(state);
        if (opaque_dst)
            d = 0xFF000000u | (d & 0x00FFFFFFu);
        else if (!use_seed_dst)
            d |= 0xFF000000u;
        dst_a[i] = d;
        dst_b[i] = d;
    }

    if (opaque_dst) {
        pix_blend_row_premultiplied_opaque_dst(dst_a, src, width);
        pix_blend_row_premultiplied_opaque_dst_ref(dst_b, src, width);
    } else {
        pix_blend_row_premultiplied(dst_a, src, width);
        pix_blend_row_premultiplied_ref(dst_b, src, width);
    }

    for (uint32_t i = 0; i < width; i++) {
        if (dst_a[i] != dst_b[i])
            return false;
    }
    return true;
}

bool run_copy_case(uint32_t width)
{
    if (width == 0 || width > kSelfTestMax)
        return false;
    static uint32_t dst[kSelfTestMax];
    static uint32_t src[kSelfTestMax];
    uint32_t state = 0x5EEDu ^ width;
    for (uint32_t i = 0; i < width; i++)
        src[i] = pixops_lcg(state);
    pix_copy_row(dst, src, width);
    for (uint32_t i = 0; i < width; i++) {
        if (dst[i] != src[i])
            return false;
    }
    return true;
}

bool run_fill_case(uint32_t width, uint32_t color)
{
    if (width == 0 || width > kSelfTestMax)
        return false;
    static uint32_t dst[kSelfTestMax];
    for (uint32_t i = 0; i < width; i++)
        dst[i] = 0;
    pix_fill_row(dst, width, color);
    for (uint32_t i = 0; i < width; i++) {
        if (dst[i] != color)
            return false;
    }
    return true;
}

} // namespace

bool gui_pixops_self_test(void)
{
    static const uint32_t widths[] = {1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 33, 64, 100, 127, 128, 255, 257, 511, 512};
    for (uint32_t width : widths) {
        if (!run_blend_case(width, false, false))
            return false;
        if (!run_blend_case(width, false, true))
            return false;
        if (!run_blend_case(width, true, false))
            return false;
        if (!run_copy_case(width))
            return false;
        if (!run_fill_case(width, 0x12345678u))
            return false;
    }
    return true;
}
