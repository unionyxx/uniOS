#include "wm_core.h"

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
