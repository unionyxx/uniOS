#include "wm_damage.h"
#include "wm_metrics.h"
#include "wm_present.h"
#include "wm_window.h"

DirtyRect g_window_outer_cache[MAX_WINDOWS];
DirtyRect g_window_client_cache[MAX_WINDOWS];
bool g_window_visible_cache[MAX_WINDOWS];
DirtyRect g_window_visible_regions[MAX_WINDOWS][MAX_VISIBLE_REGIONS];
int g_window_visible_region_count[MAX_WINDOWS];
bool g_window_visible_region_overflow[MAX_WINDOWS] = {};

void refresh_window_cache()
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        g_window_outer_cache[i] = {0, 0, 0, 0};
        g_window_client_cache[i] = {0, 0, 0, 0};
        g_window_visible_cache[i] = false;
        g_window_visible_region_count[i] = 0;
    }
    for (int i = 0; i < g_window_count; i++) {
        g_window_visible_cache[i] = is_window_visible(g_windows[i]);
        g_window_outer_cache[i] = window_outer_bounds(g_windows[i]);
        g_window_client_cache[i] = window_visible_client_bounds(g_windows[i]);
    }
}

static void merge_adjacent_regions(DirtyRect *regions, int *count);

static bool subtract_region_list(DirtyRect *regions, int *region_count, const DirtyRect &cut)
{
    int initial_count = *region_count;
    for (int i = 0; i < initial_count;) {
        DirtyRect current = regions[i], overlap = {};
        if (!rect_intersection(current, cut, &overlap)) {
            i++;
            continue;
        }

        if (*region_count > MAX_VISIBLE_REGIONS - 4) {
            merge_adjacent_regions(regions, region_count);
            if (*region_count > MAX_VISIBLE_REGIONS - 4)
                return false;
        }

        regions[i] = regions[*region_count - 1];
        (*region_count)--;

        // Fast path: region is completely consumed by overlap
        if (overlap.x == current.x && overlap.y == current.y && overlap.w == current.w && overlap.h == current.h) {
            continue;
        }

        int o_b = overlap.y + overlap.h;
        int c_b = current.y + current.h;
        int o_r = overlap.x + overlap.w;
        int c_r = current.x + current.w;

        if (current.y < overlap.y) {
            int h = overlap.y - current.y;
            if (h > 0) {
                if (*region_count >= MAX_VISIBLE_REGIONS)
                    return false;
                regions[(*region_count)++] = {current.x, current.y, current.w, h};
            }
        }
        if (o_b < c_b) {
            int h = c_b - o_b;
            if (h > 0) {
                if (*region_count >= MAX_VISIBLE_REGIONS)
                    return false;
                regions[(*region_count)++] = {current.x, o_b, current.w, h};
            }
        }
        if (current.x < overlap.x) {
            int w = overlap.x - current.x;
            if (w > 0) {
                if (*region_count >= MAX_VISIBLE_REGIONS)
                    return false;
                regions[(*region_count)++] = {current.x, overlap.y, w, overlap.h};
            }
        }
        if (o_r < c_r) {
            int w = c_r - o_r;
            if (w > 0) {
                if (*region_count >= MAX_VISIBLE_REGIONS)
                    return false;
                regions[(*region_count)++] = {o_r, overlap.y, w, overlap.h};
            }
        }
    }
    return true;
}

static void merge_adjacent_regions(DirtyRect *regions, int *count)
{
    if (!regions || !count || *count <= 1)
        return;

    shell_sort_dirty_rects(regions, *count);

    int write = 0;
    for (int read = 1; read < *count; read++) {
        DirtyRect &a = regions[write];
        DirtyRect &b = regions[read];
        bool merged = false;

        if (a.x == b.x && a.w == b.w && a.y + a.h == b.y) {
            a.h += b.h;
            merged = true;
        } else if (a.y == b.y && a.h == b.h && a.x + a.w == b.x) {
            a.w += b.w;
            merged = true;
        }

        if (!merged) {
            write++;
            regions[write] = b;
        }
    }
    *count = write + 1;
}

void refresh_window_visible_regions()
{
    const int limit = g_window_count > MAX_WINDOWS ? MAX_WINDOWS : g_window_count;
    for (int i = 0; i < limit; i++) {
        g_window_visible_region_count[i] = 0;
        g_window_visible_region_overflow[i] = false;
        if (!g_window_visible_cache[i] || !g_windows[i].buffer || i < WM_FIRST_USER_WINDOW)
            continue;

        DirtyRect regions[MAX_VISIBLE_REGIONS];
        int region_count = 1;
        regions[0] = g_window_outer_cache[i];

        for (int above = i + 1; above < g_window_count; above++) {
            const Window &cover = g_windows[above];
            if (!g_window_visible_cache[above] || !cover.buffer || cover.transparent)
                continue;
            DirtyRect cover_rects[3];
            int cover_rect_count = 0;
            get_window_opaque_cover_rects(cover, cover_rects, &cover_rect_count);
            for (int c = 0; c < cover_rect_count; c++) {
                if (!subtract_region_list(regions, &region_count, cover_rects[c])) {
                    g_window_visible_region_overflow[i] = true;
                    region_count = 0;
                    break;
                }
                if (region_count == 0)
                    break;
            }
            if (g_window_visible_region_overflow[i] || region_count == 0)
                break;
        }

        if (region_count > 1)
            merge_adjacent_regions(regions, &region_count);

        g_window_visible_region_count[i] = region_count;
        for (int r = 0; r < region_count; r++)
            g_window_visible_regions[i][r] = regions[r];
    }
}

int find_top_opaque_covering_window(const DirtyRect &r)
{
    for (int i = g_window_count - 1; i >= 0; i--) {
        const Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || w.transparent || !w.buffer)
            continue;
        DirtyRect cover_rects[3];
        int cover_rect_count = 0;
        get_window_opaque_cover_rects(w, cover_rects, &cover_rect_count);
        for (int c = 0; c < cover_rect_count; c++) {
            if (rect_contains(cover_rects[c], r))
                return i;
        }
    }
    return -1;
}

bool rect_intersects_window_chrome(const Window &w, const DirtyRect &r)
{
    if (w.transparent)
        return false;
    DirtyRect outer = window_outer_bounds(w);
    if (!rect_intersection(outer, r, nullptr))
        return false;

    DirtyRect client = window_visible_client_bounds(w);
    DirtyRect overlap = {};
    if (!rect_intersection(outer, client, &overlap))
        return true;

    if (outer.y < overlap.y && rect_intersection(r, {outer.x, outer.y, outer.w, overlap.y - outer.y}, nullptr))
        return true;
    int overlap_bottom = overlap.y + overlap.h;
    int outer_bottom = outer.y + outer.h;
    if (overlap_bottom < outer_bottom &&
        rect_intersection(r, {outer.x, overlap_bottom, outer.w, outer_bottom - overlap_bottom}, nullptr))
        return true;
    if (outer.x < overlap.x && rect_intersection(r, {outer.x, overlap.y, overlap.x - outer.x, overlap.h}, nullptr))
        return true;
    int overlap_right = overlap.x + overlap.w;
    int outer_right = outer.x + outer.w;
    if (overlap_right < outer_right &&
        rect_intersection(r, {overlap_right, overlap.y, outer_right - overlap_right, overlap.h}, nullptr))
        return true;

    return false;
}
