#include "wm_damage.h"
#include "wm_input.h"
#include "wm_metrics.h"
#include "wm_overlays.h"
#include "wm_present.h"
#include "wm_render.h"
#include "wm_window.h"

static bool g_applying_pending_bounds = false;

// Branchless integer absolute difference
static inline int wm_abs(int x)
{
    int mask = x >> 31;
    return (x ^ mask) - mask;
}
static int wm_snap_threshold()
{
    int threshold = gui_scaled_metric(WM_SNAP_THRESHOLD_BASE);
    return threshold < 6 ? 6 : threshold;
}

static int wm_snap_escape()
{
    int escape = gui_scaled_metric(WM_SNAP_ESCAPE_BASE);
    return escape < wm_snap_threshold() ? wm_snap_threshold() : escape;
}

void apply_window_move_snap(const Window &w, int *x, int *y, int width, int height)
{
    if (!x || !y || !w.entry || w.entry->state == WIN_MAXIMIZED || width <= 0 || height <= 0)
        return;

    int threshold = wm_snap_threshold();
    int min_y = wm_menubar_h() + wm_title_bar_h() + wm_desktop_margin();
    int left = wm_desktop_margin();
    int right = (int)g_screen.width - wm_desktop_margin();
    int bottom = (int)g_screen.height - wm_dock_reserved_h();

    int nx = *x;
    int ny = *y;
    int edges = RESIZE_NONE;
    if (wm_abs(nx - left) <= threshold) {
        nx = left;
        edges |= RESIZE_LEFT;
    }
    if (wm_abs((nx + width) - right) <= threshold) {
        nx = right - width;
        edges |= RESIZE_RIGHT;
    }
    if (wm_abs(ny - min_y) <= threshold) {
        ny = min_y;
        edges |= RESIZE_TOP;
    }
    if (wm_abs((ny + height) - bottom) <= threshold) {
        ny = bottom - height;
        edges |= RESIZE_BOTTOM;
    }

    if (edges != RESIZE_NONE) {
        g_input.snap_edges = edges;
        g_input.snap_preview = {nx, ny, width, height};
        enqueue_damage_rect(nx, ny, width, height);
    } else if (g_input.snap_edges != RESIZE_NONE) {
        int escape = wm_snap_escape();
        if (wm_abs(*x - g_input.snap_preview.x) > escape || wm_abs(*y - g_input.snap_preview.y) > escape) {
            DirtyRect old_preview = g_input.snap_preview;
            g_input.snap_edges = RESIZE_NONE;
            g_input.snap_preview = {};
            enqueue_damage_rect(old_preview.x, old_preview.y, old_preview.w, old_preview.h);
        }
    }

    *x = nx;
    *y = ny;
}

void reset_window_snap_state()
{
    if (g_input.snap_edges == RESIZE_NONE)
        return;
    DirtyRect preview = g_input.snap_preview;
    g_input.snap_edges = RESIZE_NONE;
    g_input.snap_preview = {};
    if (preview.w > 0 && preview.h > 0)
        enqueue_damage_rect(preview.x, preview.y, preview.w, preview.h);
}

static bool compositor_state_allows_fast_move()
{
    if (g_dirty_count != 0)
        return false;
    if (g_window_visibility_cache_dirty)
        return false;
    if (g_context_menu.open || g_storage_prompt.visible)
        return false;
    if (g_input.hover_frame_index >= 0 || g_input.hover_resize_edges != RESIZE_NONE || g_input.hover_button >= 0)
        return false;
    return true;
}

static bool can_fast_move_window(int moving_index, const DirtyRect &old_outer, const DirtyRect &new_outer)
{
    if (moving_index < WM_FIRST_USER_WINDOW || moving_index != g_window_count - 1)
        return false;
    if (!compositor_state_allows_fast_move())
        return false;
    DirtyRect move_union = rect_union(old_outer, new_outer);
    for (int i = 0; i < g_window_count; i++) {
        if (i == moving_index || !is_window_visible(g_windows[i]) || !g_windows[i].buffer)
            continue;
        if (rect_intersection(move_union,
                              g_windows[i].transparent ? window_client_bounds(g_windows[i])
                                                       : window_outer_bounds(g_windows[i]),
                              nullptr))
            return false;
    }
    return true;
}

void set_window_bounds(Window &w, int x, int y, int width, int height)
{
    if (!w.entry)
        return;
    int min_width = (w.min_w > 0) ? w.min_w : wm_default_min_w();
    int min_height = (w.min_h > 0) ? w.min_h : wm_default_min_h();
    if (width < min_width)
        width = min_width;
    if (height < min_height)
        height = min_height;
    int min_y = wm_menubar_h() + wm_title_bar_h() + wm_desktop_margin();
    int max_width = (int)g_screen.width - wm_desktop_margin() * 2;
    int max_height = (int)g_screen.height - wm_dock_reserved_h() - min_y;
    if (max_width < min_width)
        max_width = min_width;
    if (max_height < min_height)
        max_height = min_height;
    if (width > max_width)
        width = max_width;
    if (height > max_height)
        height = max_height;
    int max_x = (int)g_screen.width - width - wm_desktop_margin();
    int max_y = (int)g_screen.height - wm_dock_reserved_h() - height;
    if (max_x < wm_desktop_margin())
        max_x = wm_desktop_margin();
    if (max_y < min_y)
        max_y = min_y;
    x = x < wm_desktop_margin() ? wm_desktop_margin() : (x > max_x ? max_x : x);
    y = y < min_y ? min_y : (y > max_y ? max_y : y);
    w.target_x = x;
    w.target_y = y;
    w.target_w = width;
    w.target_h = height;

    bool size_changed = (w.w != width) || (w.h != height);
    bool moved = (w.x != x) || (w.y != y);
    if (!size_changed && !moved) {
        w.entry->active = true;
        close_context_menu();
        asm volatile("sfence" ::: "memory");
        return;
    }

    if (size_changed) {
        // Synchronous resize: the configure carries the new geometry and the
        // visible bounds flip to it only when the client acks. While a
        // configure is outstanding only the target moves; apply_pending
        // publishes the newest target right after the ack lands, so the
        // client is never asked for two sizes at once.
        if (!w.resize_configure_pending && !post_window_resize_configure(w))
            apply_window_bounds_now(w, x, y, width, height, true);
        w.entry->active = true;
        close_context_menu();
        asm volatile("sfence" ::: "memory");
        return;
    }

    apply_window_bounds_now(w, x, y, width, height, true);

    w.entry->active = true;
    close_context_menu();
    asm volatile("sfence" ::: "memory");
}

// Apply geometry to a window. publish=true additionally writes the registry
// entry (and issues a resize configure on a size change) — i.e. it claims the
// new size toward the client; plain moves use it directly. publish=false only
// moves the compositor-side geometry; the synchronous resize flip uses it to
// land the bounds of an acknowledged configure without touching the entry
// (which keeps carrying the drag target the client is redrawing toward).
void apply_window_bounds_now(Window &w, int x, int y, int width, int height, bool publish)
{
    bool size_changed = (w.w != width) || (w.h != height);
    bool moved = (w.x != x) || (w.y != y);
    if (moved || size_changed) {
        Window old = w;
        w.x = x;
        w.y = y;
        w.w = width;
        w.h = height;
        if (publish) {
            w.entry->x = x;
            w.entry->y = y;
            w.entry->w = width;
            w.entry->h = height;
            w.entry->position_serial++;
            asm volatile("sfence" ::: "memory");
        }

        // Update last_rendered immediately so damage calculation uses current state
        // (not stale values from last submitted frame, which may be many frames ago during resize)
        w.last_rendered_x = x;
        w.last_rendered_y = y;
        w.last_rendered_w = width;
        w.last_rendered_h = height;

        DirtyRect old_covered = window_opaque_bounds(old);
        DirtyRect new_covered = window_opaque_bounds(w);
        capture_shell_backdrop_for_rect(old_covered, gui_registry());
        capture_shell_backdrop_for_rect(new_covered, gui_registry());

        if (size_changed && clamp_window_scroll(w))
            publish_window_scroll(w);

        bool moved_fast = false;
        if (moved && !size_changed && !w.transparent && g_input.pointer_down && g_input.drag_edges == RESIZE_NONE &&
            g_input.drag_index >= WM_FIRST_USER_WINDOW && g_input.drag_index < g_window_count &&
            g_windows[g_input.drag_index].entry == w.entry) {
            DirtyRect old_outer = window_outer_bounds(old);
            DirtyRect new_outer = window_outer_bounds(w);
            if (can_fast_move_window(g_input.drag_index, old_outer, new_outer))
                moved_fast = move_backbuffer_rect(old_outer, new_outer);
        }

        if (moved_fast) {
            mark_presentbuffer_slots_stale(rect_union(window_outer_bounds(old), window_outer_bounds(w)));
            mark_exposed_transition_damage(window_outer_bounds(old), window_outer_bounds(w));
            mark_window_decoration_damage(w);

            int shadow_pad = wm_frame_shadow_offset_x() > wm_frame_shadow_offset_y() ? wm_frame_shadow_offset_x()
                                                                                     : wm_frame_shadow_offset_y();
            enqueue_damage_rect_expanded(window_outer_bounds(old), shadow_pad);
        } else if (size_changed) {
            int pad = (g_input.pointer_down && g_input.drag_edges != RESIZE_NONE) ? wm_window_damage_pad_interactive()
                                                                                  : wm_window_damage_pad();
            DirtyRect last_rendered_outer;
            if (old.transparent) {
                last_rendered_outer = {old.last_rendered_x, old.last_rendered_y, old.last_rendered_w,
                                       old.last_rendered_h};
            } else {
                int t_h = wm_title_bar_h();
                last_rendered_outer = {old.last_rendered_x, old.last_rendered_y - t_h,
                                       old.last_rendered_w + wm_frame_shadow_offset_x(),
                                       old.last_rendered_h + t_h + wm_frame_shadow_offset_y()};
            }
            DirtyRect full_bounds = rect_expand(rect_union(last_rendered_outer, window_outer_bounds(w)), pad);
            enqueue_damage_rect(full_bounds.x, full_bounds.y, full_bounds.w, full_bounds.h);
        } else {
            mark_window_transition_damage(old, w);
        }
        invalidate_window_visibility_cache();
    }
}

void apply_pending_window_bounds()
{
    g_applying_pending_bounds = true;
    for (int i = WM_FIRST_USER_WINDOW; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (w.target_x == w.x && w.target_y == w.y && w.target_w == w.w && w.target_h == w.h)
            continue;

        if (w.target_w != w.w || w.target_h != w.h) {
            // Synchronous resize: publish the newest target only when the
            // previous configure has been acked. Between acks the window
            // holds its last committed frame — content is never scaled,
            // stale, or partially drawn, and the window follows the pointer
            // at the client's render rate like a toolkit live resize.
            if (!w.resize_configure_pending)
                post_window_resize_configure(w);
            continue;
        }

        set_window_bounds(w, w.target_x, w.target_y, w.target_w, w.target_h);
    }
    g_applying_pending_bounds = false;
}
