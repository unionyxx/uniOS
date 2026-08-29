#include "wm_render.h"

uint32_t mix_rgb(uint32_t a, uint32_t b, uint8_t t)
{
    uint32_t inv = 255u - t;
    uint32_t ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    return 0xFF000000u | (div255(ar * inv + br * t) << 16) | (div255(ag * inv + bg * t) << 8) |
           div255(ab * inv + bb * t);
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
    return (r * 54 + g * 183 + b * 19 + 128) >> 8;
}

uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t coverage)
{
    return gui_blend_pixel(dst, src, coverage);
}

#include <emmintrin.h>

void blit_alpha_blend_rect(uint32_t *__restrict__ dst, uint32_t dst_stride, const uint32_t *__restrict__ src,
                           uint32_t src_stride, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    const __m128i alpha_mask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
    const __m128i zero = _mm_setzero_si128();
    const __m128i v_255 = _mm_set1_epi16(255);
    const __m128i v_128 = _mm_set1_epi16(128);

    for (int y = 0; y < h; ++y) {
        uint32_t *drow = &dst[static_cast<size_t>(y) * dst_stride];
        const uint32_t *srow = &src[static_cast<size_t>(y) * src_stride];

        int x = 0;
        for (; x <= w - 4; x += 4) {
            __m128i s_vec = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&srow[x]));

            __m128i s_alpha = _mm_and_si128(s_vec, alpha_mask);
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(s_alpha, _mm_setzero_si128())) == 0xFFFF) {
                continue;
            }

            __m128i d_vec = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&drow[x]));
            __m128i d_alpha = _mm_and_si128(d_vec, alpha_mask);
            __m128i trans_mask = _mm_cmpeq_epi32(d_alpha, _mm_setzero_si128());
            __m128i opaque_dst_mask = _mm_cmpeq_epi32(d_alpha, alpha_mask);
            __m128i fast_mask = _mm_or_si128(opaque_dst_mask, trans_mask);

            if (_mm_movemask_epi8(_mm_cmpeq_epi32(fast_mask, _mm_setzero_si128())) != 0) {
                for (int k = 0; k < 4; ++k) {
                    uint32_t s = srow[x + k];
                    uint32_t sa = s >> 24;
                    if (!sa)
                        continue;
                    if (sa == 255) {
                        drow[x + k] = s;
                        continue;
                    }
                    uint32_t d = drow[x + k];
                    uint32_t da = d >> 24;
                    if (da == 255) {
                        uint32_t inv_sa = 255u - sa;
                        uint32_t rb = (s & 0x00FF00FFu) * sa + (d & 0x00FF00FFu) * inv_sa + 0x00800080u;
                        rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                        uint32_t g_acc = ((s >> 8) & 0xFFu) * sa + ((d >> 8) & 0xFFu) * inv_sa + 0x80u;
                        uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
                        drow[x + k] = 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
                    } else {
                        drow[x + k] = blend_rgb(d, s, 255);
                    }
                }
                continue;
            }

            __m128i s_lo = _mm_unpacklo_epi8(s_vec, zero);
            __m128i s_hi = _mm_unpackhi_epi8(s_vec, zero);
            __m128i d_lo = _mm_unpacklo_epi8(d_vec, zero);
            __m128i d_hi = _mm_unpackhi_epi8(d_vec, zero);

            __m128i alpha_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
            alpha_lo = _mm_shufflehi_epi16(alpha_lo, _MM_SHUFFLE(3, 3, 3, 3));

            __m128i alpha_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
            alpha_hi = _mm_shufflehi_epi16(alpha_hi, _MM_SHUFFLE(3, 3, 3, 3));

            __m128i inv_alpha_lo = _mm_sub_epi16(v_255, alpha_lo);
            __m128i inv_alpha_hi = _mm_sub_epi16(v_255, alpha_hi);

            __m128i src_part_lo = _mm_mullo_epi16(s_lo, alpha_lo);
            __m128i src_part_hi = _mm_mullo_epi16(s_hi, alpha_hi);

            __m128i dst_part_lo = _mm_mullo_epi16(d_lo, inv_alpha_lo);
            __m128i dst_part_hi = _mm_mullo_epi16(d_hi, inv_alpha_hi);

            __m128i sum_lo = _mm_add_epi16(_mm_add_epi16(src_part_lo, dst_part_lo), v_128);
            __m128i sum_hi = _mm_add_epi16(_mm_add_epi16(src_part_hi, dst_part_hi), v_128);

            __m128i sum_lo_shift = _mm_srli_epi16(sum_lo, 8);
            __m128i sum_hi_shift = _mm_srli_epi16(sum_hi, 8);

            __m128i final_lo = _mm_srli_epi16(_mm_add_epi16(sum_lo, sum_lo_shift), 8);
            __m128i final_hi = _mm_srli_epi16(_mm_add_epi16(sum_hi, sum_hi_shift), 8);

            __m128i blended_vec = _mm_packus_epi16(final_lo, final_hi);

            blended_vec = _mm_or_si128(blended_vec, alpha_mask);
            blended_vec = _mm_or_si128(_mm_and_si128(trans_mask, s_vec), _mm_andnot_si128(trans_mask, blended_vec));

            _mm_storeu_si128(reinterpret_cast<__m128i *>(&drow[x]), blended_vec);
        }

        for (; x < w; ++x) {
            uint32_t s = srow[x];
            uint32_t sa = s >> 24;
            if (!sa)
                continue;
            if (sa == 255) {
                drow[x] = s;
                continue;
            }

            uint32_t d = drow[x];
            uint32_t da = d >> 24;

            if (da == 255) {
                uint32_t inv_sa = 255u - sa;
                uint32_t rb = (s & 0x00FF00FFu) * sa + (d & 0x00FF00FFu) * inv_sa + 0x00800080u;
                rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                uint32_t g_acc = ((s >> 8) & 0xFFu) * sa + ((d >> 8) & 0xFFu) * inv_sa + 0x80u;
                uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
                drow[x] = 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
            } else if (!da) {
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

    if (src_x < 0) {
        dst_x -= src_x;
        w += src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        dst_y -= src_y;
        h += src_y;
        src_y = 0;
    }
    if (dst_x < 0) {
        src_x -= dst_x;
        w += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y -= dst_y;
        h += dst_y;
        dst_y = 0;
    }

    if (w <= 0 || h <= 0)
        return;
    if (static_cast<uint32_t>(src_x) >= src->width || static_cast<uint32_t>(src_y) >= src->height ||
        static_cast<uint32_t>(dst_x) >= dst->width || static_cast<uint32_t>(dst_y) >= dst->height)
        return;

    if (src_x + w > static_cast<int>(src->width))
        w = static_cast<int>(src->width) - src_x;
    if (src_y + h > static_cast<int>(src->height))
        h = static_cast<int>(src->height) - src_y;
    if (dst_x + w > static_cast<int>(dst->width))
        w = static_cast<int>(dst->width) - dst_x;
    if (dst_y + h > static_cast<int>(dst->height))
        h = static_cast<int>(dst->height) - dst_y;

    if (w <= 0 || h <= 0)
        return;

    uint32_t dst_stride = dst->pitch / 4u;
    uint32_t src_stride = src->pitch / 4u;

    const bool overlap = (dst->buffer == src->buffer) && !((dst_x + w) <= src_x || (src_x + w) <= dst_x ||
                                                           (dst_y + h) <= src_y || (src_y + h) <= dst_y);

    int start_y = 0, end_y = h, step_y = 1;
    if (overlap && dst_y > src_y) {
        start_y = h - 1;
        end_y = -1;
        step_y = -1;
    }

    for (int y = start_y; y != end_y; y += step_y) {
        uint32_t *drow = &dst->buffer[static_cast<size_t>(dst_y + y) * dst_stride + dst_x];
        const uint32_t *srow = &src->buffer[static_cast<size_t>(src_y + y) * src_stride + src_x];
        if (overlap)
            memmove(drow, srow, static_cast<size_t>(w) * sizeof(uint32_t));
        else
            memcpy(drow, srow, static_cast<size_t>(w) * sizeof(uint32_t));
    }
}

bool ensure_surface_capacity(Surface *surface, uint32_t width, uint32_t height)
{
    if (!surface)
        return false;
    if (surface->buffer && surface->width >= width && surface->height >= height) {
        surface->width = width;
        surface->height = height;
        return true;
    }

    gui_destroy_surface(surface);
    uint32_t padded_w = (width + 255u) & ~255u;
    uint32_t padded_h = (height + 255u) & ~255u;
    *surface = gui_create_surface(padded_w, padded_h);
    if (surface->buffer) {
        surface->width = width;
        surface->height = height;
        return true;
    }
    return false;
}
