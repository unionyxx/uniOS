#pragma once

#include <stdint.h>
#include <string.h>

#include "gui.h"

static inline bool gui_clamp_rect_to_canvas(Rect *rect, int canvas_w, int canvas_h)
{
    if (!rect || canvas_w <= 0 || canvas_h <= 0 || rect->w <= 0 || rect->h <= 0)
        return false;

    int64_t left = rect->x;
    int64_t top = rect->y;
    int64_t right = left + (int64_t)rect->w;
    int64_t bottom = top + (int64_t)rect->h;

    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > canvas_w)
        right = canvas_w;
    if (bottom > canvas_h)
        bottom = canvas_h;
    if (right <= left || bottom <= top)
        return false;

    rect->x = (int)left;
    rect->y = (int)top;
    rect->w = (int)(right - left);
    rect->h = (int)(bottom - top);
    return true;
}

static inline void gui_copy_to_canvas(volatile uint32_t *dst, const uint32_t *src, uint32_t w, uint32_t h)
{
    if (!dst || !src || w == 0 || h == 0)
        return;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *d_row = reinterpret_cast<uint32_t *>(const_cast<uint32_t *>(dst) + ((size_t)y * w));
        const uint32_t *s_row = src + ((size_t)y * w);
        memcpy(d_row, s_row, (size_t)w * sizeof(uint32_t));
    }
}

static inline void gui_copy_rect_to_canvas(volatile uint32_t *dst, uint32_t dst_stride, const uint32_t *src,
                                           uint32_t src_stride, Rect rect)
{
    if (!dst || !src || dst_stride == 0 || src_stride == 0 || rect.w <= 0 || rect.h <= 0 || rect.x < 0 || rect.y < 0)
        return;
    for (int py = 0; py < rect.h; py++) {
        uint32_t *d_row = reinterpret_cast<uint32_t *>(const_cast<uint32_t *>(dst) + (size_t)(rect.y + py) * dst_stride + rect.x);
        const uint32_t *s_row = src + (size_t)(rect.y + py) * src_stride + rect.x;
        memcpy(d_row, s_row, (size_t)rect.w * sizeof(uint32_t));
    }
}

static inline uint32_t gui_blend_premultiplied(uint32_t dst, uint32_t src)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return src;

    uint32_t inv = 255u - alpha;
    
    uint32_t dst_rb = dst & 0x00FF00FFu;
    uint32_t dst_ag = (dst >> 8) & 0x00FF00FFu;
    
    uint32_t rb = dst_rb * inv + 0x00800080u;
    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
    rb &= 0x00FF00FFu;
    
    uint32_t ag = dst_ag * inv + 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00FF00FFu)) >> 8;
    ag &= 0x00FF00FFu;
    
    uint32_t src_rb = src & 0x00FF00FFu;
    uint32_t src_ag = (src >> 8) & 0x00FF00FFu;
    
    uint32_t out_rb = src_rb + rb;
    uint32_t out_ag = src_ag + ag;
    
    return ((out_ag << 8) & 0xFF00FF00u) | (out_rb & 0x00FF00FFu);
}

static inline uint32_t gui_blend_premultiplied_opaque_dst(uint32_t dst, uint32_t src)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return src;

    uint32_t inv = 255u - alpha;
    
    uint32_t dst_rb = dst & 0x00FF00FFu;
    uint32_t dst_ag = (dst >> 8) & 0x00FF00FFu;
    
    uint32_t rb = dst_rb * inv + 0x00800080u;
    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
    rb &= 0x00FF00FFu;
    
    uint32_t ag = dst_ag * inv + 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00FF00FFu)) >> 8;
    ag &= 0x00FF00FFu;
    
    uint32_t src_rb = src & 0x00FF00FFu;
    uint32_t src_ag = (src >> 8) & 0x00FF00FFu;
    
    uint32_t out_rb = src_rb + rb;
    uint32_t out_ag = src_ag + ag;
    
    return 0xFF000000u | ((out_ag << 8) & 0x0000FF00u) | (out_rb & 0x00FF00FFu);
}

static inline uint32_t gui_blend_straight_opaque_dst(uint32_t dst, uint32_t src)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return src;

    uint32_t inv = 255u - alpha;
    
    uint32_t src_rb = src & 0x00FF00FFu;
    uint32_t src_ag = (src >> 8) & 0x00FF00FFu;
    
    uint32_t dst_rb = dst & 0x00FF00FFu;
    uint32_t dst_ag = (dst >> 8) & 0x00FF00FFu;
    
    uint32_t rb = src_rb * alpha + dst_rb * inv + 0x00800080u;
    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
    rb &= 0x00FF00FFu;
    
    uint32_t ag = src_ag * alpha + dst_ag * inv + 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00FF00FFu)) >> 8;
    ag &= 0x00FF00FFu;
    
    return 0xFF000000u | ((ag << 8) & 0x0000FF00u) | rb;
}

static inline uint32_t gui_blend_straight_opaque_dst_coverage(uint32_t dst, uint32_t src, uint8_t coverage)
{
    if (coverage == 0)
        return dst;
    uint32_t alpha = src >> 24;
    if (coverage < 255) {
        alpha = (alpha * coverage + 127u) / 255u;
    }
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return src;

    uint32_t inv = 255u - alpha;
    
    uint32_t src_rb = src & 0x00FF00FFu;
    uint32_t src_ag = (src >> 8) & 0x00FF00FFu;
    
    uint32_t dst_rb = dst & 0x00FF00FFu;
    uint32_t dst_ag = (dst >> 8) & 0x00FF00FFu;
    
    uint32_t rb = src_rb * alpha + dst_rb * inv + 0x00800080u;
    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
    rb &= 0x00FF00FFu;
    
    uint32_t ag = src_ag * alpha + dst_ag * inv + 0x00800080u;
    ag = (ag + ((ag >> 8) & 0x00FF00FFu)) >> 8;
    ag &= 0x00FF00FFu;
    
    return 0xFF000000u | ((ag << 8) & 0x0000FF00u) | rb;
}
