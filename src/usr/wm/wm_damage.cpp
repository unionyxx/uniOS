#include "wm_damage.h"

#include "wm_input.h"
#include "wm_metrics.h"
#include "wm_present.h"

DirtyRect g_dirty_rects[MAX_DIRTY_RECTS];
int g_dirty_count = 0;
bool g_window_visibility_cache_dirty = true;
bool g_dirty_frame_ready = false;

void collapse_dirty_rects_to_bounds()
{
    int count = clamp_dirty_rect_count(g_dirty_count);
    if (count <= 0) {
        invalidate_dirty_frame();
        return;
    }
    DirtyRect bounds = g_dirty_rects[0];
    for (int i = 1; i < count; i++) {
        bounds = rect_union(bounds, g_dirty_rects[i]);
    }
    g_dirty_rects[0] = bounds;
    g_dirty_count = 1;
    invalidate_dirty_frame();
}

bool dirty_set_is_single_fullscreen_rect()
{
    if (clamp_dirty_rect_count(g_dirty_count) != 1)
        return false;
    const DirtyRect &rect = g_dirty_rects[0];
    return rect.x == 0 && rect.y == 0 && rect.w == static_cast<int>(g_screen.width) &&
           rect.h == static_cast<int>(g_screen.height);
}

bool dirty_set_intersects_rect(const DirtyRect &target)
{
    int count = clamp_dirty_rect_count(g_dirty_count);
    for (int i = 0; i < count; i++) {
        if (rect_intersection(g_dirty_rects[i], target, nullptr))
            return true;
    }
    return false;
}

bool dirty_set_contains_rect(const DirtyRect &target)
{
    int count = clamp_dirty_rect_count(g_dirty_count);
    for (int i = 0; i < count; i++) {
        if (rect_contains(g_dirty_rects[i], target))
            return true;
    }
    return false;
}

static bool dirty_rect_less(const DirtyRect &a, const DirtyRect &b);

// Optimized non-recursive Shell Sort for DirtyRect arrays (Ciura-style sequence)
void shell_sort_dirty_rects(DirtyRect *arr, int n)
{
    int gap = 1;
    while (gap < n) {
        gap = gap * 3 + 1;
    }
    while (gap > 0) {
        for (int i = gap; i < n; i++) {
            DirtyRect temp = arr[i];
            int j = i;
            while (j >= gap && dirty_rect_less(temp, arr[j - gap])) {
                arr[j] = arr[j - gap];
                j -= gap;
            }
            arr[j] = temp;
        }
        gap /= 3;
    }
}

static void copy_dirty_rects_to_policy(wm::DirtyRect *dst, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = {g_dirty_rects[i].x, g_dirty_rects[i].y, g_dirty_rects[i].w, g_dirty_rects[i].h};
}

static void copy_dirty_rects_from_policy(const wm::DirtyRect *src, int count)
{
    for (int i = 0; i < count; i++)
        g_dirty_rects[i] = {src[i].x, src[i].y, src[i].w, src[i].h};
}

void enqueue_damage_rect(int x, int y, int w, int h)
{
    DirtyRect incoming = {x, y, w, h};
    if (!clip_dirty_rect_to_screen(incoming))
        return;

    if (incoming.x == 0 && incoming.y == 0 && incoming.w == (int)g_screen.width && incoming.h == (int)g_screen.height) {
        g_dirty_rects[0] = incoming;
        g_dirty_count = 1;
        invalidate_dirty_frame();
        return;
    }

    if (g_dirty_count < MAX_DIRTY_RECTS) {
        bool overlaps = false;
        for (int i = 0; i < g_dirty_count; i++) {
            if (dirty_rects_intersect(g_dirty_rects[i], incoming)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            g_dirty_rects[g_dirty_count++] = incoming;
            invalidate_dirty_frame();
            return;
        }
    }

    bool resizing = g_input.pointer_down && g_input.drag_edges != RESIZE_NONE;
    if (resizing && g_dirty_count > 1) {
        collapse_dirty_rects_to_bounds();
    }

    wm::DirtyRect policy_rects[MAX_DIRTY_RECTS];
    int policy_count = clamp_dirty_rect_count(g_dirty_count);
    copy_dirty_rects_to_policy(policy_rects, policy_count);

    wm::DirtyRect incoming_policy = {incoming.x, incoming.y, incoming.w, incoming.h};
    wm::enqueue_damage_rect(policy_rects, &policy_count, MAX_DIRTY_RECTS, (int)g_screen.width, (int)g_screen.height,
                            incoming_policy);

    g_dirty_count = clamp_dirty_rect_count(policy_count);
    copy_dirty_rects_from_policy(policy_rects, g_dirty_count);
    invalidate_dirty_frame();
}

void invalidate_dirty_frame()
{
    g_dirty_frame_ready = false;
}

void invalidate_window_visibility_cache()
{
    g_window_visibility_cache_dirty = true;
    g_dirty_frame_ready = false;
}

static bool dirty_rect_less(const DirtyRect &a, const DirtyRect &b)
{
    if (a.y != b.y)
        return a.y < b.y;
    return a.x < b.x;
}

static void sort_dirty_rects()
{
    if (g_dirty_count > 1)
        shell_sort_dirty_rects(g_dirty_rects, g_dirty_count);
}

void normalize_dirty_rects(bool interactive)
{
    if (g_dirty_count <= 0) {
        invalidate_dirty_frame();
        return;
    }

    wm::DirtyRect policy_rects[MAX_DIRTY_RECTS] = {};
    int policy_count = clamp_dirty_rect_count(g_dirty_count);
    copy_dirty_rects_to_policy(policy_rects, policy_count);

    wm::normalize_dirty_rects(policy_rects, &policy_count, (int)g_screen.width, (int)g_screen.height, interactive);

    g_dirty_count = clamp_dirty_rect_count(policy_count);
    copy_dirty_rects_from_policy(policy_rects, g_dirty_count);
    if (g_dirty_count > 1)
        sort_dirty_rects();
    invalidate_dirty_frame();
}

bool clip_dirty_rect_to_screen(DirtyRect &rect)
{
    int x = rect.x, y = rect.y, w = rect.w, h = rect.h;
    if (w <= 0 || h <= 0)
        return false;

    int screen_w = (int)g_screen.width;
    int screen_h = (int)g_screen.height;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }

    if (x >= screen_w || y >= screen_h)
        return false;

    if (x + w > screen_w)
        w = screen_w - x;
    if (y + h > screen_h)
        h = screen_h - y;

    if (w <= 0 || h <= 0)
        return false;

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    return true;
}

void enqueue_damage_rect_expanded(const DirtyRect &rect, int pad)
{
    DirtyRect expanded = rect_expand(rect, pad);
    enqueue_damage_rect(expanded.x, expanded.y, expanded.w, expanded.h);
}
