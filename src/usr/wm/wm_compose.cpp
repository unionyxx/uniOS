#include "wm_core.h"

void compose_rect_unclipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                            const Registry *registry)
{
    int start_index = find_top_opaque_covering_window(r);
    if (start_index < 0) {
        gui_blit_rect(&g_backbuffer, &g_wallpaper, r.x, r.y, r.x, r.y, r.w, r.h);
        start_index = 0;
    }

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

static bool subtract_rect_local(DirtyRect *regions, int *count, const DirtyRect &cut)
{
    int initial_count = *count;
    for (int i = 0; i < initial_count; i++) {
        DirtyRect current = regions[i];
        DirtyRect overlap = {};
        if (!rect_intersection(current, cut, &overlap)) {
            continue;
        }

        int fragments = 0;
        if (overlap.y > current.y) fragments++;
        if (overlap.y + overlap.h < current.y + current.h) fragments++;
        if (overlap.x > current.x) fragments++;
        if (overlap.x + overlap.w < current.x + current.w) fragments++;

        if (*count - 1 + fragments > MAX_VISIBLE_REGIONS) {
            return false;
        }

        regions[i] = regions[*count - 1];
        (*count)--;
        i--;
        initial_count--;

        if (overlap.y > current.y) {
            regions[*count] = {current.x, current.y, current.w, overlap.y - current.y};
            (*count)++;
        }
        if (overlap.y + overlap.h < current.y + current.h) {
            regions[*count] = {current.x, overlap.y + overlap.h, current.w, (current.y + current.h) - (overlap.y + overlap.h)};
            (*count)++;
        }
        if (overlap.x > current.x) {
            regions[*count] = {current.x, overlap.y, overlap.x - current.x, overlap.h};
            (*count)++;
        }
        if (overlap.x + overlap.w < current.x + current.w) {
            regions[*count] = {overlap.x + overlap.w, overlap.y, (current.x + current.w) - (overlap.x + overlap.w), overlap.h};
            (*count)++;
        }
    }
    return true;
}

bool compose_rect_clipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                          const Registry *registry)
{
    if (!g_backbuffer.buffer)
        return false;

    int start_index = find_top_opaque_covering_window(r);
    if (start_index < 0) {
        DirtyRect wallpaper_regions[MAX_VISIBLE_REGIONS];
        int count = 0;
        wallpaper_regions[count++] = r;

        for (int i = 0; i < g_window_count; i++) {
            Window &w = g_windows[i];
            if (!g_window_visible_cache[i] || !w.buffer || w.transparent)
                continue;
            DirtyRect cover_rects[3];
            int cover_rect_count = 0;
            get_window_opaque_cover_rects(w, cover_rects, &cover_rect_count);
            for (int cr = 0; cr < cover_rect_count; cr++) {
                if (!subtract_rect_local(wallpaper_regions, &count, cover_rects[cr]))
                    return false;
            }
            if (count >= MAX_VISIBLE_REGIONS - 4)
                break;
        }

        for (int ri = 0; ri < count; ri++) {
            DirtyRect sub = wallpaper_regions[ri];
            gui_blit_rect(&g_backbuffer, &g_wallpaper, sub.x, sub.y, sub.x, sub.y, sub.w, sub.h);
        }
        start_index = 0;
    }

    for (int i = start_index; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || !w.buffer || w.transparent)
            continue;

        if (!dirty_rects_intersect(r, g_window_outer_cache[i]))
            continue;

        if (g_window_visible_region_overflow[i])
            return false;

        int region_count = g_window_visible_region_count[i];
        if (region_count > MAX_VISIBLE_REGIONS)
            region_count = MAX_VISIBLE_REGIONS;
        
        bool focused = (i == focused_index);
        bool hovered_frame = (i == hover_frame_index);
        int hover_btn = hovered_frame ? hover_button : -1;

        for (int region = 0; region < region_count; region++) {
            DirtyRect visible = {};
            if (!rect_intersection(g_window_visible_regions[i][region], r, &visible))
                continue;
            
            draw_window_decoration_clipped(&g_backbuffer, w, visible, focused, hovered_frame, hover_btn);
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

    size_t stride = g_backbuffer.pitch / sizeof(uint32_t);
    int start = 0, end = src.h, step = 1;
    if (dst.y > src.y) {
        start = src.h - 1;
        end = -1;
        step = -1;
    }

    size_t row_size = (size_t)src.w * sizeof(uint32_t);
    bool x_overlap = !(dst.x >= src.x + src.w || dst.x + src.w <= src.x);

    if (!x_overlap) {
        for (int row = start; row != end; row += step) {
            size_t dst_offset = (size_t)(dst.y + row) * stride + dst.x;
            size_t src_offset = (size_t)(src.y + row) * stride + src.x;
            memcpy(&g_backbuffer.buffer[dst_offset], &g_backbuffer.buffer[src_offset], row_size);
        }
    } else {
        for (int row = start; row != end; row += step) {
            size_t dst_offset = (size_t)(dst.y + row) * stride + dst.x;
            size_t src_offset = (size_t)(src.y + row) * stride + src.x;
            memmove(&g_backbuffer.buffer[dst_offset], &g_backbuffer.buffer[src_offset], row_size);
        }
    }
    return true;
}