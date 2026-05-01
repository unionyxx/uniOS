#pragma once

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

constexpr bool rect_touch_or_overlap(const DirtyRect &a, const DirtyRect &b)
{
    return !((a.x + a.w) < b.x || (b.x + b.w) < a.x || (a.y + a.h) < b.y || (b.y + b.h) < a.y);
}

constexpr DirtyRect rect_union(const DirtyRect &a, const DirtyRect &b)
{
    int x1 = (a.x < b.x) ? a.x : b.x;
    int y1 = (a.y < b.y) ? a.y : b.y;
    int x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return {x1, y1, x2 - x1, y2 - y1};
}

inline bool clip_rect_to_screen(DirtyRect &rect, int screen_w, int screen_h)
{
    if (rect.x < 0) {
        rect.w += rect.x;
        rect.x = 0;
    }
    if (rect.y < 0) {
        rect.h += rect.y;
        rect.y = 0;
    }
    if (rect.x + rect.w > screen_w)
        rect.w = screen_w - rect.x;
    if (rect.y + rect.h > screen_h)
        rect.h = screen_h - rect.y;
    return rect.w > 0 && rect.h > 0;
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

    for (int i = 0; i < *count; i++) {
        for (int j = i + 1; j < *count;) {
            if (rect_touch_or_overlap(rects[i], rects[j])) {
                rects[i] = rect_union(rects[i], rects[j]);
                rects[j] = rects[*count - 1];
                (*count)--;
                continue;
            }
            j++;
        }
    }
    if (*count <= 1)
        return;

    DirtyRect bounds = rects[0];
    uint64_t total_area = 0;
    for (int i = 0; i < *count; i++) {
        bounds = rect_union(bounds, rects[i]);
        total_area += (uint64_t)rects[i].w * (uint64_t)rects[i].h;
    }
    uint64_t bound_area = (uint64_t)bounds.w * (uint64_t)bounds.h;
    int collapse_limit = interactive ? interactive_dirty_collapse_limit() : non_interactive_dirty_collapse_limit();
    if (*count > collapse_limit || bound_area * dirty_collapse_ratio_den() <= total_area * dirty_collapse_ratio_num()) {
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
    int x1 = (a.x > b.x) ? a.x : b.x;
    int y1 = (a.y > b.y) ? a.y : b.y;
    int x2 = ((a.x + a.w) < (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int y2 = ((a.y + a.h) < (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    if (x2 <= x1 || y2 <= y1)
        return false;
    out = {x1, y1, x2 - x1, y2 - y1};
    return true;
}

constexpr bool rect_contains(const DirtyRect &outer, const DirtyRect &inner)
{
    return inner.x >= outer.x && inner.y >= outer.y && (inner.x + inner.w) <= (outer.x + outer.w) &&
           (inner.y + inner.h) <= (outer.y + outer.h);
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

    int old_bottom = old_outer.y + old_outer.h;
    int overlap_bottom = overlap.y + overlap.h;
    if (overlap_bottom < old_bottom) {
        out.rects[out.count++] = {old_outer.x, overlap_bottom, old_outer.w, old_bottom - overlap_bottom};
    }

    if (old_outer.x < overlap.x) {
        out.rects[out.count++] = {old_outer.x, overlap.y, overlap.x - old_outer.x, overlap.h};
    }

    int old_right = old_outer.x + old_outer.w;
    int overlap_right = overlap.x + overlap.w;
    if (overlap_right < old_right) {
        out.rects[out.count++] = {overlap_right, overlap.y, old_right - overlap_right, overlap.h};
    }

    return out;
}

} // namespace wm
