#include "wm_input.h"
#include "wm_window.h"
#include "wm_damage.h"
#include "wm_present.h"
#include "wm_overlays.h"
#include "wm_metrics.h"

WmInputState g_input;

void mark_cursor_transition_damage(int old_x, int old_y, GuiCursorKind old_kind, int new_x, int new_y,
                                   GuiCursorKind new_kind)
{
    // The hardware cursor plane is never baked into frames, so its movement
    // needs no damage; software-cursor frames cover the transition below.
    if (wm_cursor_backend_allowed())
        return;
    DirtyRect orct = {}, nrect = {};
    gui_get_cursor_bounds(old_kind, old_x, old_y, &orct.x, &orct.y, &orct.w, &orct.h);
    gui_get_cursor_bounds(new_kind, new_x, new_y, &nrect.x, &nrect.y, &nrect.w, &nrect.h);
    enqueue_damage_rect(orct.x - CURSOR_DAMAGE_PAD, orct.y - CURSOR_DAMAGE_PAD, orct.w + CURSOR_DAMAGE_PAD * 2,
                        orct.h + CURSOR_DAMAGE_PAD * 2);
    enqueue_damage_rect(nrect.x - CURSOR_DAMAGE_PAD, nrect.y - CURSOR_DAMAGE_PAD, nrect.w + CURSOR_DAMAGE_PAD * 2,
                        nrect.h + CURSOR_DAMAGE_PAD * 2);
}

static void clear_move_target_with_leave()
{
    if (!g_input.move_target_entry)
        return;
    int target = find_window_by_entry(g_input.move_target_entry);
    if (target >= 0)
        post_plain_event_to_window(g_windows[target], EVT_MOUSE_LEAVE);
    g_input.move_target_entry = nullptr;
}

void apply_mouse_move(Registry *registry, int new_x, int new_y)
{
    if (g_screen.width > 0) {
        if (new_x < 0)
            new_x = 0;
        else if (new_x >= (int)g_screen.width)
            new_x = (int)g_screen.width - 1;
    } else {
        new_x = 0;
    }
    if (g_screen.height > 0) {
        if (new_y < 0)
            new_y = 0;
        else if (new_y >= (int)g_screen.height)
            new_y = (int)g_screen.height - 1;
    } else {
        new_y = 0;
    }
    if (new_x == g_input.mouse_x && new_y == g_input.mouse_y)
        return;
    g_input.old_mouse_x = g_input.mouse_x;
    g_input.old_mouse_y = g_input.mouse_y;
    g_input.mouse_x = new_x;
    g_input.mouse_y = new_y;
    // Update last cursor render position
    g_input.last_cursor_x = g_input.mouse_x;
    g_input.last_cursor_y = g_input.mouse_y;

    if (update_control_center_drag(g_input.mouse_x, g_input.mouse_y)) {
        clear_move_target_with_leave();
        mark_cursor_transition_damage(g_input.old_mouse_x, g_input.old_mouse_y, g_input.cursor_kind, g_input.mouse_x,
                                      g_input.mouse_y, g_input.cursor_kind);
        return;
    }

    if (g_input.pointer_down && g_input.drag_index >= WM_FIRST_USER_WINDOW && g_input.drag_index < g_window_count) {
        // The WM owns the pointer during window moves/resizes; clients must
        // not keep hover state from before the drag started.
        clear_move_target_with_leave();
        Window &w = g_windows[g_input.drag_index];
        if (g_input.drag_edges == RESIZE_NONE) {
            int nx = g_input.mouse_x - g_input.drag_offset_x;
            int ny = g_input.mouse_y - g_input.drag_offset_y;
            apply_window_move_snap(w, &nx, &ny, g_input.drag_origin.w, g_input.drag_origin.h);
            set_window_bounds(w, nx, ny, g_input.drag_origin.w, g_input.drag_origin.h);
        } else {
            int dx = g_input.mouse_x - g_input.drag_origin_mouse_x, dy = g_input.mouse_y - g_input.drag_origin_mouse_y;
            int nx = g_input.drag_origin.x, ny = g_input.drag_origin.y, nw = g_input.drag_origin.w,
                nh = g_input.drag_origin.h;
            int min_width = (w.min_w > 0) ? w.min_w : wm_default_min_w();
            int min_height = (w.min_h > 0) ? w.min_h : wm_default_min_h();
            int origin_right = g_input.drag_origin.x + g_input.drag_origin.w;
            int origin_bottom = g_input.drag_origin.y + g_input.drag_origin.h;
            if (g_input.drag_edges & RESIZE_LEFT) {
                nx += dx;
                nw = origin_right - nx;
                if (nw < min_width) {
                    nw = min_width;
                    nx = origin_right - min_width;
                }
            }
            if (g_input.drag_edges & RESIZE_RIGHT) {
                nw = origin_right + dx - nx;
                if (nw < min_width)
                    nw = min_width;
            }
            if (g_input.drag_edges & RESIZE_TOP) {
                ny += dy;
                nh = origin_bottom - ny;
                if (nh < min_height) {
                    nh = min_height;
                    ny = origin_bottom - min_height;
                }
            }
            if (g_input.drag_edges & RESIZE_BOTTOM) {
                nh = origin_bottom + dy - ny;
                if (nh < min_height)
                    nh = min_height;
            }
            if (w.entry)
                w.entry->state = WIN_NORMAL;
            set_window_bounds(w, nx, ny, nw, nh);
        }
    }

    mark_cursor_transition_damage(g_input.old_mouse_x, g_input.old_mouse_y, g_input.cursor_kind, g_input.mouse_x,
                                  g_input.mouse_y, g_input.cursor_kind);
    update_storage_prompt_hover(g_input.mouse_x, g_input.mouse_y);
    if (g_storage_prompt.visible) {
        clear_move_target_with_leave();
        return;
    }
    update_index_hover(g_input.mouse_x, g_input.mouse_y);
    if (g_index.active) {
        clear_move_target_with_leave();
        return;
    }
    update_control_center_hover(g_input.mouse_x, g_input.mouse_y);
    if (g_control_center.open) {
        clear_move_target_with_leave();
        return;
    }
    update_context_menu_hover(registry, g_input.mouse_x, g_input.mouse_y);
    if (pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y)) {
        clear_move_target_with_leave();
        return;
    }

    if (g_input.drag_index >= WM_FIRST_USER_WINDOW)
        return;

    // Client pointer grab: a window holding a button press receives every move
    // even outside its client area so in-window drags (sliders, scrollbars,
    // selections) continue seamlessly past the frame.
    if (g_input.client_grab_entry) {
        int grabbed = find_window_by_entry(g_input.client_grab_entry);
        if (grabbed >= WM_FIRST_USER_WINDOW && is_window_visible(g_windows[grabbed])) {
            post_mouse_event_to_window(g_windows[grabbed], EVT_MOUSE_MOVE, g_input.mouse_x, g_input.mouse_y, 0);
            return;
        }
        g_input.client_grab_entry = nullptr;
    }

    int focus = find_registry_focused_user_window(registry);
    if (focus >= WM_FIRST_USER_WINDOW) {
        const Window &fw = g_windows[focus];
        bool hit = fw.transparent ? point_hits_window_visible_pixel(fw, g_input.mouse_x, g_input.mouse_y)
                                  : point_in_client(fw, g_input.mouse_x, g_input.mouse_y);
        if (hit) {
            if (g_input.move_target_entry != fw.entry) {
                if (g_input.move_target_entry) {
                    int prev = find_window_by_entry(g_input.move_target_entry);
                    if (prev >= 0)
                        post_plain_event_to_window(g_windows[prev], EVT_MOUSE_LEAVE);
                }
                g_input.move_target_entry = fw.entry;
            }
            post_mouse_event_to_window(fw, EVT_MOUSE_MOVE, g_input.mouse_x, g_input.mouse_y, 0);
            return;
        }
    }
    clear_move_target_with_leave();
}

void update_hover_feedback()
{
    int nhf = -1, nre = RESIZE_NONE, nhb = -1;
    if (!g_input.pointer_down && !pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y)) {
        for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; i--) {
            const Window &w = g_windows[i];
            if (!is_window_visible(w))
                continue;
            if (w.transparent) {
                if (point_hits_window_visible_pixel(w, g_input.mouse_x, g_input.mouse_y))
                    break;
                continue;
            }
            if (!point_in_outer(w, g_input.mouse_x, g_input.mouse_y))
                continue;
            for (int b = 0; b < 3; b++)
                if (point_in_button(w, g_input.mouse_x, g_input.mouse_y, b)) {
                    nhf = i;
                    nhb = b;
                    break;
                }
            if (nhf >= 0)
                break;
            nre = hit_test_resize(w, g_input.mouse_x, g_input.mouse_y);
            if (nre != RESIZE_NONE || point_in_titlebar(w, g_input.mouse_x, g_input.mouse_y)) {
                nhf = i;
                break;
            }
            break;
        }
    }
    if (nhf >= WM_FIRST_USER_WINDOW && nhf != g_window_count - 1) {
        nhf = -1;
        nre = RESIZE_NONE;
        nhb = -1;
    }
    if (nhf == g_input.hover_frame_index && nre == g_input.hover_resize_edges && nhb == g_input.hover_button)
        return;
    int old_hover_frame = g_input.hover_frame_index;
    if (old_hover_frame >= WM_FIRST_USER_WINDOW && old_hover_frame < g_window_count)
        mark_window_chrome_damage(g_windows[old_hover_frame]);
    g_input.hover_frame_index = nhf;
    g_input.hover_resize_edges = nre;
    g_input.hover_button = nhb;
    if (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_frame_index < g_window_count &&
        g_input.hover_frame_index != old_hover_frame)
        mark_window_chrome_damage(g_windows[g_input.hover_frame_index]);
}

void update_cursor_kind()
{
    auto ck_edges = [](int edges) -> GuiCursorKind {
        bool h = (edges & RESIZE_LEFT) || (edges & RESIZE_RIGHT), v = (edges & RESIZE_TOP) || (edges & RESIZE_BOTTOM);
        if (h && v)
            return (((edges & RESIZE_LEFT) && (edges & RESIZE_TOP)) ||
                    ((edges & RESIZE_RIGHT) && (edges & RESIZE_BOTTOM)))
                       ? GUI_CURSOR_RESIZE_D1
                       : GUI_CURSOR_RESIZE_D2;
        return h ? GUI_CURSOR_RESIZE_H : (v ? GUI_CURSOR_RESIZE_V : GUI_CURSOR_ARROW);
    };
    GuiCursorKind n_k = GUI_CURSOR_ARROW;
    if (g_input.pointer_down && g_input.drag_index >= WM_FIRST_USER_WINDOW)
        n_k = g_input.drag_edges == RESIZE_NONE ? GUI_CURSOR_MOVE : ck_edges(g_input.drag_edges);
    else if (g_input.hover_resize_edges != RESIZE_NONE)
        n_k = ck_edges(g_input.hover_resize_edges);
    else if (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_button < 0)
        n_k = GUI_CURSOR_MOVE;
    if (!g_input.pointer_down)
        reset_window_snap_state();
    if (n_k != g_input.cursor_kind) {
        // Old cursor was rendered at last_cursor_x/last_cursor_y with last_cursor_kind
        // New cursor will be rendered at mouse_x/mouse_y with n_k
        mark_cursor_transition_damage(g_input.last_cursor_x, g_input.last_cursor_y, g_input.last_cursor_kind,
                                      g_input.mouse_x, g_input.mouse_y, n_k);
        g_input.last_cursor_kind = n_k;
        g_input.last_cursor_x = g_input.mouse_x;
        g_input.last_cursor_y = g_input.mouse_y;
        g_input.cursor_kind = n_k;
    }
}

