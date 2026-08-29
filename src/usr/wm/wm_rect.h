#pragma once

#include "wm_types.h"

static inline float wm_fabsf(float x)
{
    return x < 0.0f ? -x : x;
}

static inline float wm_sqrtf(float n)
{
    float result;
    asm("sqrtss %1, %0" : "=x"(result) : "x"(n));
    return result;
}

static inline int64_t rect_right_i64(const DirtyRect &rect)
{
    return (int64_t)rect.x + (int64_t)rect.w;
}
static inline int64_t rect_bottom_i64(const DirtyRect &rect)
{
    return (int64_t)rect.y + (int64_t)rect.h;
}
static inline int clamp_i64_to_int(int64_t value)
{
    if (value < (int64_t)INT32_MIN)
        return INT32_MIN;
    if (value > (int64_t)INT32_MAX)
        return INT32_MAX;
    return (int)value;
}

static inline int clamp_dirty_rect_count(int count)
{
    if (count < 0)
        return 0;
    if (count > MAX_DIRTY_RECTS)
        return MAX_DIRTY_RECTS;
    return count;
}

static inline bool rect_contains(const DirtyRect &outer, const DirtyRect &inner)
{
    if (outer.w <= 0 || outer.h <= 0 || inner.w <= 0 || inner.h <= 0)
        return false;
    return inner.x >= outer.x && inner.y >= outer.y && rect_right_i64(inner) <= rect_right_i64(outer) &&
           rect_bottom_i64(inner) <= rect_bottom_i64(outer);
}

static inline bool point_in_rect(const DirtyRect &rect, int x, int y)
{
    if (rect.w <= 0 || rect.h <= 0)
        return false;
    return x >= rect.x && y >= rect.y && (int64_t)x < rect_right_i64(rect) && (int64_t)y < rect_bottom_i64(rect);
}

static inline bool rect_intersection(const DirtyRect &a, const DirtyRect &b, DirtyRect *out)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    int64_t x1 = (a.x > b.x) ? a.x : b.x;
    int64_t y1 = (a.y > b.y) ? a.y : b.y;
    int64_t x2 = (rect_right_i64(a) < rect_right_i64(b)) ? rect_right_i64(a) : rect_right_i64(b);
    int64_t y2 = (rect_bottom_i64(a) < rect_bottom_i64(b)) ? rect_bottom_i64(a) : rect_bottom_i64(b);
    if (x2 <= x1 || y2 <= y1)
        return false;
    if (out)
        *out = {clamp_i64_to_int(x1), clamp_i64_to_int(y1), clamp_i64_to_int(x2 - x1), clamp_i64_to_int(y2 - y1)};
    return true;
}

static inline DirtyRect rect_union(const DirtyRect &a, const DirtyRect &b)
{
    int64_t x1 = (a.x < b.x) ? a.x : b.x;
    int64_t y1 = (a.y < b.y) ? a.y : b.y;
    int64_t x2 = (rect_right_i64(a) > rect_right_i64(b)) ? rect_right_i64(a) : rect_right_i64(b);
    int64_t y2 = (rect_bottom_i64(a) > rect_bottom_i64(b)) ? rect_bottom_i64(a) : rect_bottom_i64(b);
    return {clamp_i64_to_int(x1), clamp_i64_to_int(y1), clamp_i64_to_int(x2 - x1), clamp_i64_to_int(y2 - y1)};
}

static inline bool dirty_rects_intersect(const DirtyRect &a, const DirtyRect &b)
{
    return rect_intersection(a, b, nullptr);
}

static inline wm::DirtyRect to_policy_rect(const DirtyRect &rect)
{
    return {rect.x, rect.y, rect.w, rect.h};
}
static inline DirtyRect from_policy_rect(const wm::DirtyRect &rect)
{
    return {rect.x, rect.y, rect.w, rect.h};
}

static inline DirtyRect rect_expand(const DirtyRect &rect, int pad)
{
    if (pad <= 0 || rect.w <= 0 || rect.h <= 0)
        return rect;
    int64_t x = (int64_t)rect.x - pad;
    int64_t y = (int64_t)rect.y - pad;
    int64_t w = (int64_t)rect.w + (int64_t)pad * 2;
    int64_t h = (int64_t)rect.h + (int64_t)pad * 2;
    return {clamp_i64_to_int(x), clamp_i64_to_int(y), clamp_i64_to_int(w), clamp_i64_to_int(h)};
}

static inline uint32_t div255(uint32_t x)
{
    return (uint32_t)(((uint64_t)x * 0x8081u) >> 23);
}

static inline uint8_t scale_alpha_u8(uint8_t alpha, uint8_t coverage)
{
    return (uint8_t)div255((uint32_t)alpha * (uint32_t)coverage);
}
