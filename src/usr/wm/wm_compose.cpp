#include "wm_core.h"

// Compose dirty rect without cached visible regions. Fallback for overflows.
void compose_rect_unclipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                            const Registry *registry)
{
    int start_index = 0;
    bool covered_by_opaque = false;
    for (int i = g_window_count - 1; i >= 0; i--) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || w.transparent || !w.buffer)
            continue;
        DirtyRect outer = window_occlusion_bounds(w);
        if (rect_contains(outer, r)) {
            start_index = i;
            covered_by_opaque = true;
            break;
        }
    }

    if (!covered_by_opaque)
        gui_blit_rect(&g_backbuffer, &g_wallpaper, r.x, r.y, r.x, r.y, r.w, r.h);

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || w.transparent)
            continue;
        if (!dirty_rects_intersect(g_window_outer_cache[i], r))
            continue;
        draw_window_decoration_clipped(&g_backbuffer, w, r, (i == focused_index), (i == hover_frame_index),
                                       (i == hover_frame_index) ? hover_button : -1);
        draw_window_client_clipped(&g_backbuffer, w, r);
    }

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || !w.transparent)
            continue;
        if (!dirty_rects_intersect(g_window_outer_cache[i], r))
            continue;
        draw_window_client_clipped(&g_backbuffer, w, r);
    }

    if (g_context_menu.open && rect_intersection(r, context_menu_bounds(), nullptr))
        draw_context_menu_overlay_clipped(r, registry);
    if (g_storage_prompt.visible)
        draw_storage_prompt_overlay_clipped(r);
    if (g_index.active)
        draw_index_overlay_clipped(r, registry);
    if (g_control_center.open)
        draw_control_center_overlay_clipped(r);
    draw_toast_overlay_clipped(r);
}

bool compose_rect_clipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                          const Registry *registry)
{
    if (!g_backbuffer.buffer)
        return false;

    int start_index = find_top_opaque_covering_window(r);
    if (start_index < 0) {
        gui_blit_rect(&g_backbuffer, &g_wallpaper, r.x, r.y, r.x, r.y, r.w, r.h);
        start_index = 0;
    }

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || w.transparent)
            continue;

        if (!dirty_rects_intersect(r, g_window_outer_cache[i]))
            continue;

        // Cache overflow: fallback to unclipped path.
        if (g_window_visible_region_overflow[i])
            return false;

        int region_count = g_window_visible_region_count[i];
        if (region_count > MAX_VISIBLE_REGIONS)
            region_count = MAX_VISIBLE_REGIONS;
        for (int region = 0; region < region_count; region++) {
            DirtyRect visible = {};
            if (!rect_intersection(g_window_visible_regions[i][region], r, &visible))
                continue;
            bool focused = (i == focused_index);
            bool hovered_frame = (i == hover_frame_index);
            draw_window_decoration_clipped(&g_backbuffer, w, visible, focused, hovered_frame,
                                           hovered_frame ? hover_button : -1);
            if (rect_intersection(visible, g_window_client_cache[i], nullptr)) {
                draw_window_client_clipped(&g_backbuffer, w, visible);
            }
        }
    }

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || !w.transparent)
            continue;

        DirtyRect visible = {};
        if (!rect_intersection(r, g_window_outer_cache[i], &visible))
            continue;

        draw_window_client_clipped(&g_backbuffer, w, visible);
    }

    if (g_context_menu.open && rect_intersection(r, context_menu_bounds(), nullptr))
        draw_context_menu_overlay_clipped(r, registry);
    if (g_storage_prompt.visible)
        draw_storage_prompt_overlay_clipped(r);
    if (g_index.active)
        draw_index_overlay_clipped(r, registry);
    if (g_control_center.open)
        draw_control_center_overlay_clipped(r);
    draw_toast_overlay_clipped(r);
    return true;
}

bool move_backbuffer_rect(const DirtyRect &old_rect, const DirtyRect &new_rect)
{
    if (!g_backbuffer.buffer || old_rect.w != new_rect.w || old_rect.h != new_rect.h)
        return false;
    DirtyRect src = old_rect;
    DirtyRect dst = new_rect;
    if (!clip_dirty_rect_to_screen(src) || !clip_dirty_rect_to_screen(dst))
        return false;
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
        return false;
    if (src.w != dst.w || src.h != dst.h)
        return false;

    uint32_t stride = g_backbuffer.pitch / 4;
    int start = 0, end = src.h, step = 1;
    if (dst.y > src.y) {
        start = src.h - 1;
        end = -1;
        step = -1;
    }

    for (int row = start; row != end; row += step) {
        memmove(&g_backbuffer.buffer[(dst.y + row) * stride + dst.x],
                &g_backbuffer.buffer[(src.y + row) * stride + src.x], (size_t)src.w * sizeof(uint32_t));
    }
    return true;
}
