#pragma once

#include <limits.h>
#include <stdint.h>

namespace wm {

struct DirtyRect
{
    int x;
    int y;
    int w;
    int h;
};

struct ExposedTransitionDamage
{
    DirtyRect rects[4];
    int count;
};

enum class PresentPolicyDecision
{
    Submit,
    Wait,
    Skip,
};

struct PresentPolicyInput
{
    uint32_t pending;
    uint32_t queue_limit;
    bool strict_sync;
    bool interactive;
    bool copy_path;
    bool active_manipulation;
};

constexpr uint32_t dirty_collapse_ratio_num()
{
    return 5u;
}

constexpr uint32_t dirty_collapse_ratio_den()
{
    return 4u;
}

constexpr int interactive_dirty_collapse_limit()
{
    return 10;
}

constexpr int non_interactive_dirty_collapse_limit()
{
    return 6;
}

constexpr int clamp_i64_to_i32(int64_t value)
{
    return value < INT32_MIN ? INT32_MIN : (value > INT32_MAX ? INT32_MAX : (int)value);
}

constexpr int64_t rect_right_i64(const DirtyRect &rect)
{
    return (int64_t)rect.x + (int64_t)rect.w;
}

constexpr int64_t rect_bottom_i64(const DirtyRect &rect)
{
    return (int64_t)rect.y + (int64_t)rect.h;
}

constexpr bool rect_touch_or_overlap(const DirtyRect &a, const DirtyRect &b)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    return !(rect_right_i64(a) < (int64_t)b.x || rect_right_i64(b) < (int64_t)a.x ||
             rect_bottom_i64(a) < (int64_t)b.y || rect_bottom_i64(b) < (int64_t)a.y);
}

constexpr DirtyRect rect_union(const DirtyRect &a, const DirtyRect &b)
{
    int64_t x1 = ((int64_t)a.x < (int64_t)b.x) ? (int64_t)a.x : (int64_t)b.x;
    int64_t y1 = ((int64_t)a.y < (int64_t)b.y) ? (int64_t)a.y : (int64_t)b.y;
    int64_t x2 = (rect_right_i64(a) > rect_right_i64(b)) ? rect_right_i64(a) : rect_right_i64(b);
    int64_t y2 = (rect_bottom_i64(a) > rect_bottom_i64(b)) ? rect_bottom_i64(a) : rect_bottom_i64(b);
    return {clamp_i64_to_i32(x1), clamp_i64_to_i32(y1), clamp_i64_to_i32(x2 - x1),
            clamp_i64_to_i32(y2 - y1)};
}

inline bool clip_rect_to_screen(DirtyRect &rect, int screen_w, int screen_h)
{
    if (screen_w <= 0 || screen_h <= 0 || rect.w <= 0 || rect.h <= 0)
        return false;

    int64_t x1 = rect.x;
    int64_t y1 = rect.y;
    int64_t x2 = rect_right_i64(rect);
    int64_t y2 = rect_bottom_i64(rect);

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > screen_w)
        x2 = screen_w;
    if (y2 > screen_h)
        y2 = screen_h;
    if (x2 <= x1 || y2 <= y1)
        return false;

    rect = {clamp_i64_to_i32(x1), clamp_i64_to_i32(y1), clamp_i64_to_i32(x2 - x1),
            clamp_i64_to_i32(y2 - y1)};
    return true;
}

inline void normalize_dirty_rects(DirtyRect *rects, int *count, int screen_w, int screen_h, bool interactive)
{
    if (!rects || !count || *count <= 0)
        return;

    for (int i = 0; i < *count;) {
        if (!clip_rect_to_screen(rects[i], screen_w, screen_h)) {
            rects[i] = rects[*count - 1];
            (*count)--;
            continue;
        }
        i++;
    }
    if (*count <= 1)
        return;

    // First pass: Merge strictly overlapping or adjacent rects
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < *count; i++) {
            for (int j = i + 1; j < *count;) {
                if (rect_touch_or_overlap(rects[i], rects[j])) {
                    rects[i] = rect_union(rects[i], rects[j]);
                    rects[j] = rects[*count - 1];
                    (*count)--;
                    merged = true;
                    continue;
                }
                j++;
            }
        }
    }

    if (*count <= 1)
        return;

    // Second pass: Heuristic merge for efficiency
    // Merge if (union_area < (sum_area * 1.5))
    merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < *count; i++) {
            uint64_t area_i = (uint64_t)rects[i].w * rects[i].h;
            for (int j = i + 1; j < *count;) {
                DirtyRect u = rect_union(rects[i], rects[j]);
                uint64_t area_j = (uint64_t)rects[j].w * rects[j].h;
                uint64_t area_u = (uint64_t)u.w * u.h;

                if (area_u * 2 < (area_i + area_j) * 3) { // 1.5x threshold
                    rects[i] = u;
                    area_i = area_u;
                    rects[j] = rects[*count - 1];
                    (*count)--;
                    merged = true;
                    continue;
                }
                j++;
            }
        }
    }

    int collapse_limit = interactive ? 24 : 16;
    if (*count > collapse_limit) {
        DirtyRect bounds = rects[0];
        for (int i = 1; i < *count; i++)
            bounds = rect_union(bounds, rects[i]);
        rects[0] = bounds;
        *count = 1;
    }
}

inline void enqueue_damage_rect(DirtyRect *rects, int *count, int max_rects, int screen_w, int screen_h,
                                DirtyRect incoming)
{
    if (!rects || !count || max_rects <= 0)
        return;
    if (!clip_rect_to_screen(incoming, screen_w, screen_h))
        return;

    if (incoming.x == 0 && incoming.y == 0 && incoming.w == screen_w && incoming.h == screen_h) {
        rects[0] = incoming;
        *count = 1;
        return;
    }

    for (int i = 0; i < *count; i++) {
        if (rect_touch_or_overlap(rects[i], incoming)) {
            rects[i] = rect_union(rects[i], incoming);
            return;
        }
    }

    if (*count < max_rects) {
        rects[*count] = incoming;
        (*count)++;
        return;
    }

    DirtyRect bounds = rects[0];
    for (int i = 1; i < *count; i++)
        bounds = rect_union(bounds, rects[i]);
    rects[0] = rect_union(bounds, incoming);
    *count = 1;
}

constexpr PresentPolicyDecision choose_present_policy(const PresentPolicyInput &input)
{
    if (input.pending < input.queue_limit)
        return PresentPolicyDecision::Submit;

    if (input.strict_sync || !input.interactive)
        return PresentPolicyDecision::Wait;

    if (input.active_manipulation)
        return PresentPolicyDecision::Skip;

    if (input.copy_path)
        return PresentPolicyDecision::Wait;

    return PresentPolicyDecision::Wait;
}

constexpr uint32_t pending_presents(uint32_t last_submitted_sequence, uint32_t completed_sequence)
{
    return (last_submitted_sequence <= completed_sequence) ? 0u : (last_submitted_sequence - completed_sequence);
}

constexpr uint32_t completion_target_for_available_slot(uint32_t last_submitted_sequence, uint32_t queue_limit)
{
    if (queue_limit <= 1u)
        return last_submitted_sequence;
    if (last_submitted_sequence < queue_limit)
        return 0u;
    return last_submitted_sequence - queue_limit + 1u;
}

constexpr bool rect_intersection(const DirtyRect &a, const DirtyRect &b, DirtyRect &out)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    int64_t x1 = ((int64_t)a.x > (int64_t)b.x) ? (int64_t)a.x : (int64_t)b.x;
    int64_t y1 = ((int64_t)a.y > (int64_t)b.y) ? (int64_t)a.y : (int64_t)b.y;
    int64_t x2 = (rect_right_i64(a) < rect_right_i64(b)) ? rect_right_i64(a) : rect_right_i64(b);
    int64_t y2 = (rect_bottom_i64(a) < rect_bottom_i64(b)) ? rect_bottom_i64(a) : rect_bottom_i64(b);
    if (x2 <= x1 || y2 <= y1)
        return false;
    out = {clamp_i64_to_i32(x1), clamp_i64_to_i32(y1), clamp_i64_to_i32(x2 - x1),
           clamp_i64_to_i32(y2 - y1)};
    return true;
}

constexpr bool rect_contains(const DirtyRect &outer, const DirtyRect &inner)
{
    if (outer.w <= 0 || outer.h <= 0 || inner.w <= 0 || inner.h <= 0)
        return false;
    return inner.x >= outer.x && inner.y >= outer.y && rect_right_i64(inner) <= rect_right_i64(outer) &&
           rect_bottom_i64(inner) <= rect_bottom_i64(outer);
}

constexpr ExposedTransitionDamage compute_exposed_transition_damage(const DirtyRect &old_outer,
                                                                    const DirtyRect &new_outer)
{
    ExposedTransitionDamage out = {};
    DirtyRect overlap = {};
    if (!rect_intersection(old_outer, new_outer, overlap)) {
        out.rects[0] = old_outer;
        out.count = 1;
        return out;
    }

    if (rect_contains(overlap, old_outer))
        return out;

    if (old_outer.y < overlap.y) {
        out.rects[out.count++] = {old_outer.x, old_outer.y, old_outer.w, overlap.y - old_outer.y};
    }

    int64_t old_bottom = rect_bottom_i64(old_outer);
    int64_t overlap_bottom = rect_bottom_i64(overlap);
    if (overlap_bottom < old_bottom) {
        out.rects[out.count++] = {old_outer.x, clamp_i64_to_i32(overlap_bottom), old_outer.w,
                                  clamp_i64_to_i32(old_bottom - overlap_bottom)};
    }

    if (old_outer.x < overlap.x) {
        out.rects[out.count++] = {old_outer.x, overlap.y, overlap.x - old_outer.x, overlap.h};
    }

    int64_t old_right = rect_right_i64(old_outer);
    int64_t overlap_right = rect_right_i64(overlap);
    if (overlap_right < old_right) {
        out.rects[out.count++] = {clamp_i64_to_i32(overlap_right), overlap.y,
                                  clamp_i64_to_i32(old_right - overlap_right), overlap.h};
    }

    return out;
}

} // namespace wm
