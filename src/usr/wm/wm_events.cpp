#include "wm_input.h"
#include "wm_window.h"
#include "wm_damage.h"
#include "wm_overlays.h"
#include "wm_present.h"
#include "wm_metrics.h"

bool point_targets_window_client_for_input(const Window &w, int px, int py)
{
    return w.transparent ? point_hits_window_visible_pixel(w, px, py) : point_in_client(w, px, py);
}

static void cycle_user_window_focus(Registry *registry)
{
    if (!registry || g_window_count <= WM_FIRST_USER_WINDOW)
        return;
    int current = find_registry_focused_user_window(registry);
    int count = g_window_count - WM_FIRST_USER_WINDOW;
    for (int step = 1; step <= count; step++) {
        int index = WM_FIRST_USER_WINDOW + ((current - WM_FIRST_USER_WINDOW + step) % count);
        if (is_window_visible(g_windows[index]) && is_user_window(g_windows[index])) {
            focus_window(index, true);
            return;
        }
    }
}

void wm_handle_events(Registry *registry, Event &ev)
{
    uint32_t event_budget = 32;
    while (event_budget-- > 0 && get_event(&ev)) {
        g_frame_stats.last_input_ticks = wm_tsc_now();
        if (ev.type == EVT_MOUSE_MOVE) {
            g_input.pending_mouse_x = ev.mouse.x;
            g_input.pending_mouse_y = ev.mouse.y;
            g_input.have_pending_move = true;
            continue;
        }
        if (g_input.have_pending_move) {
            apply_mouse_move(registry, g_input.pending_mouse_x, g_input.pending_mouse_y);
            g_input.have_pending_move = false;
        }

        if (g_storage_prompt.visible) {
            if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 1) {
                g_input.pointer_down = false;
                g_input.drag_index = -1;
                g_input.drag_edges = RESIZE_NONE;
                activate_storage_prompt_button(registry, g_input.mouse_x, g_input.mouse_y);
                continue;
            }
            if (ev.type == EVT_MOUSE_DOWN || ev.type == EVT_MOUSE_UP) {
                g_input.pointer_down = false;
                g_input.drag_index = -1;
                g_input.drag_edges = RESIZE_NONE;
                continue;
            }
        }

        if (g_index.active) {
            if (ev.type == EVT_MOUSE_DOWN) {
                g_input.pointer_down = false;
                g_input.drag_index = -1;
                g_input.drag_edges = RESIZE_NONE;
                if (ev.mouse.button == 1) {
                    if (!handle_index_pointer_down(registry, g_input.mouse_x, g_input.mouse_y))
                        close_index();
                } else {
                    close_index();
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_UP) {
                g_input.pointer_down = false;
                g_input.drag_index = -1;
                g_input.drag_edges = RESIZE_NONE;
                continue;
            }
        }

        if (g_control_center.open) {
            if (ev.type == EVT_MOUSE_DOWN) {
                g_input.pointer_down = false;
                g_input.drag_index = -1;
                g_input.drag_edges = RESIZE_NONE;
                if (ev.mouse.button == 1) {
                    if (!handle_control_center_pointer_down(registry, g_input.mouse_x, g_input.mouse_y))
                        close_control_center();
                } else {
                    close_control_center();
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_UP) {
                g_input.pointer_down = false;
                g_input.drag_index = -1;
                g_input.drag_edges = RESIZE_NONE;
                handle_control_center_pointer_up();
                continue;
            }
        }

        if (ev.type == EVT_MOUSE_DOWN || ev.type == EVT_MOUSE_UP) {
            // Dismiss the system menu on any click outside it: below the
            // expanded menubar window, or inside its bounds but on
            // transparent pixels beside the panel.
            bool dismiss = g_input.mouse_y >= registry->windows[0].h;
            if (!dismiss && g_input.mouse_y >= wm_menubar_h() && system_window_hit(g_input.mouse_x, g_input.mouse_y) < 0)
                dismiss = true;
            if (dismiss) {
                registry->mb_menu_dismiss_requested = true;
                smp_wmb();
            }
        }

        if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 1) {
            if (g_context_menu.open) {
                GuiMenuItem items[8];
                int c = build_context_menu_items(registry, items, 8);
                int h = gui_popup_menu_hit_test(items, c, g_context_menu.x, g_context_menu.y, g_context_menu.w,
                                                g_input.mouse_x, g_input.mouse_y);
                if (h >= 0) {
                    activate_context_menu_item(registry, h);
                    continue;
                }
                close_context_menu();
            }

            g_input.pointer_down = true;
            g_input.drag_index = -1;
            g_input.drag_edges = RESIZE_NONE;

            int hit_idx = -1;
            bool fwd_client = false;
            int sys_hit = system_window_hit(g_input.mouse_x, g_input.mouse_y);

            if (sys_hit >= 0) {
                if (sys_hit == 0) {
                    if (g_input.mouse_x > static_cast<int>(g_screen.width) - 120) {
                        g_input.pointer_down = false;
                        g_input.drag_index = -1;
                        g_input.drag_edges = RESIZE_NONE;
                        toggle_control_center();
                        continue;
                    }
                    registry->mb_click_x = g_input.mouse_x;
                    registry->mb_click_y = g_input.mouse_y;
                    registry->mb_clicked = true;
                } else if (sys_hit == 1) {
                    registry->dk_click_x = g_input.mouse_x;
                    registry->dk_click_y = g_input.mouse_y;
                    registry->dk_clicked = true;
                }
                smp_wmb();
                hit_idx = sys_hit;
            }

            for (int i = g_window_count - 1; i >= 2 && hit_idx < 0; i--) {
                Window &w = g_windows[i];
                if (!is_window_visible(w))
                    continue;

                if (w.transparent) {
                    if (!point_targets_window_client_for_input(w, g_input.mouse_x, g_input.mouse_y))
                        continue;
                    hit_idx = i;
                    fwd_client = is_user_window(w);
                    break;
                }
                if (!point_in_outer(w, g_input.mouse_x, g_input.mouse_y))
                    continue;

                if (point_in_button(w, g_input.mouse_x, g_input.mouse_y, 0)) {
                    close_window(focus_window(i, true));
                    hit_idx = -1;
                    break;
                }
                if (point_in_button(w, g_input.mouse_x, g_input.mouse_y, 1)) {
                    minimize_window(focus_window(i, true));
                    hit_idx = -1;
                    break;
                }
                if (point_in_button(w, g_input.mouse_x, g_input.mouse_y, 2)) {
                    toggle_maximize_window(focus_window(i, true));
                    hit_idx = -1;
                    break;
                }

                int redges = hit_test_resize(w, g_input.mouse_x, g_input.mouse_y);
                if (redges != RESIZE_NONE) {
                    hit_idx = focus_window(i, true);
                    g_input.drag_index = hit_idx;
                    g_input.drag_edges = redges;
                    g_input.hover_frame_index = -1;
                    g_input.hover_resize_edges = RESIZE_NONE;
                    g_input.hover_button = -1;
                    g_input.drag_origin = g_windows[hit_idx];
                    g_input.drag_origin_mouse_x = g_input.mouse_x;
                    g_input.drag_origin_mouse_y = g_input.mouse_y;
                    break;
                }
                if (point_in_titlebar(w, g_input.mouse_x, g_input.mouse_y)) {
                    hit_idx = focus_window(i, true);
                    WindowEntry *click_entry = g_windows[hit_idx].entry;
                    const uint64_t click_ticks = get_ticks();
                    const int click_shm = g_windows[hit_idx].shm_id;
                    const uint32_t click_owner = g_windows[hit_idx].owner_pid;
                    if (click_entry && click_entry == g_input.titlebar_click_entry &&
                        click_shm == g_input.titlebar_click_shm_id &&
                        click_owner == g_input.titlebar_click_owner_pid &&
                        click_ticks - g_input.titlebar_click_ticks < 400) {
                        // Titlebar double-click toggles maximize, unless the
                        // first click of the pair already restored the window.
                        const bool first_click_restored = g_input.titlebar_click_was_maximized;
                        g_input.titlebar_click_entry = nullptr;
                        g_input.titlebar_click_shm_id = WIN_SHM_INVALID;
                        g_input.titlebar_click_owner_pid = 0;
                        g_input.pointer_down = false;
                        g_input.drag_index = -1;
                        g_input.drag_edges = RESIZE_NONE;
                        if (!first_click_restored)
                            toggle_maximize_window(hit_idx);
                        break;
                    }
                    g_input.titlebar_click_entry = click_entry;
                    g_input.titlebar_click_ticks = click_ticks;
                    g_input.titlebar_click_was_maximized = click_entry && click_entry->state == WIN_MAXIMIZED;
                    g_input.titlebar_click_shm_id = click_shm;
                    g_input.titlebar_click_owner_pid = click_owner;
                    if (g_windows[hit_idx].entry) {
                        if (g_windows[hit_idx].entry->state == WIN_MAXIMIZED) {
                            int old_x = g_windows[hit_idx].x;
                            int ow = g_windows[hit_idx].w;
                            restore_window(hit_idx, false);
                            int nw = g_windows[hit_idx].w;
                            int px = (ow > 0) ? (g_input.mouse_x - old_x) * nw / ow : nw / 2;
                            px = px < 0 ? 0 : (px >= nw ? nw - 1 : px);
                            set_window_bounds(g_windows[hit_idx], g_input.mouse_x - px,
                                              g_input.mouse_y + wm_title_bar_h() / 2, g_windows[hit_idx].w,
                                              g_windows[hit_idx].h);
                        }
                        g_input.drag_index = hit_idx;
                        g_input.drag_edges = RESIZE_NONE;
                        g_input.hover_frame_index = -1;
                        g_input.hover_resize_edges = RESIZE_NONE;
                        g_input.hover_button = -1;
                        g_input.drag_offset_x = g_input.mouse_x - g_windows[hit_idx].x;
                        g_input.drag_offset_y = g_input.mouse_y - g_windows[hit_idx].y;
                        g_input.drag_origin = g_windows[hit_idx];
                    }
                    break;
                }
                if (point_in_client(w, g_input.mouse_x, g_input.mouse_y)) {
                    hit_idx = i;
                    fwd_client = is_user_window(w);
                }
                break;
            }
            if (hit_idx >= 0) {
                if (hit_idx >= 2 && fwd_client) {
                    int focused_idx = focus_window(hit_idx, true);
                    post_mouse_event_to_window(g_windows[focused_idx], EVT_MOUSE_DOWN, g_input.mouse_x, g_input.mouse_y,
                                               ev.mouse.button);
                    g_input.client_grab_entry = g_windows[focused_idx].entry;
                    g_input.client_grab_button = ev.mouse.button;
                }
            } else {
                clear_window_focus(registry);
                g_input.drag_index = -1;
            }
        } else if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 2) {
            g_input.pointer_down = false;
            g_input.drag_index = -1;
            g_input.drag_edges = RESIZE_NONE;
            close_context_menu();
            bool opened = false;
            if (system_window_hit(g_input.mouse_x, g_input.mouse_y) < 0) {
                for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; i--) {
                    if (!is_window_visible(g_windows[i]))
                        continue;

                    if (g_windows[i].transparent) {
                        if (!point_targets_window_client_for_input(g_windows[i], g_input.mouse_x, g_input.mouse_y))
                            continue;
                        if (is_user_window(g_windows[i])) {
                            int focused_idx = focus_window(i, true);
                            post_mouse_event_to_window(g_windows[focused_idx], EVT_MOUSE_DOWN, g_input.mouse_x,
                                                       g_input.mouse_y, ev.mouse.button);
                            g_input.client_grab_entry = g_windows[focused_idx].entry;
                            g_input.client_grab_button = ev.mouse.button;
                            opened = true;
                        }
                        break;
                    }
                    if (!point_in_outer(g_windows[i], g_input.mouse_x, g_input.mouse_y))
                        continue;

                    if (point_in_client(g_windows[i], g_input.mouse_x, g_input.mouse_y)) {
                        if (is_user_window(g_windows[i])) {
                            int focused_idx = focus_window(i, true);
                            post_mouse_event_to_window(g_windows[focused_idx], EVT_MOUSE_DOWN, g_input.mouse_x,
                                                       g_input.mouse_y, ev.mouse.button);
                            g_input.client_grab_entry = g_windows[focused_idx].entry;
                            g_input.client_grab_button = ev.mouse.button;
                            opened = true;
                        }
                        break;
                    }
                    if (is_user_window(g_windows[i])) {
                        open_context_menu(registry, CONTEXT_MENU_WINDOW, focus_window(i, true), g_input.mouse_x,
                                          g_input.mouse_y);
                        opened = true;
                    }
                    break;
                }
            }
            if (!opened) {
                clear_window_focus(registry);
                open_context_menu(registry, CONTEXT_MENU_DESKTOP, -1, g_input.mouse_x, g_input.mouse_y);
            }
        } else if (ev.type == EVT_MOUSE_UP && ev.mouse.button == 1) {
            int c_idx = g_input.drag_index;
            g_input.pointer_down = false;
            g_input.drag_index = -1;
            g_input.drag_edges = RESIZE_NONE;
            // Transition from drag cursor to arrow at the release position
            mark_cursor_transition_damage(g_input.last_cursor_x, g_input.last_cursor_y, g_input.cursor_kind,
                                          g_input.mouse_x, g_input.mouse_y, GUI_CURSOR_ARROW);
            g_input.cursor_kind = GUI_CURSOR_ARROW;
            g_input.last_cursor_kind = GUI_CURSOR_ARROW;
            g_input.last_cursor_x = g_input.mouse_x;
            g_input.last_cursor_y = g_input.mouse_y;

            if (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_button >= 0) {
                DirtyRect outer = window_outer_bounds(g_windows[g_input.hover_frame_index]);
                int title_h = wm_title_bar_h() + wm_frame_border() + wm_frame_shadow_offset_y();
                if (title_h > outer.h)
                    title_h = outer.h;
                enqueue_damage_rect(outer.x, outer.y, outer.w, title_h);
            }
            if (c_idx < 2) {
                int grabbed = g_input.client_grab_entry ? find_window_by_entry(g_input.client_grab_entry) : -1;
                if (g_input.client_grab_button == 1 && grabbed >= WM_FIRST_USER_WINDOW &&
                    is_window_visible(g_windows[grabbed])) {
                    // Grabbed release: deliver even outside the client area so
                    // in-window drags terminate cleanly.
                    post_mouse_event_to_window(g_windows[grabbed], EVT_MOUSE_UP, g_input.mouse_x, g_input.mouse_y,
                                               ev.mouse.button);
                } else {
                    int focus = find_registry_focused_user_window(registry);
                    if (focus >= 2 && !pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y) &&
                        point_targets_window_client_for_input(g_windows[focus], g_input.mouse_x, g_input.mouse_y)) {
                        post_mouse_event_to_window(g_windows[focus], EVT_MOUSE_UP, g_input.mouse_x, g_input.mouse_y,
                                                   ev.mouse.button);
                    }
                }
            }
            if (g_input.client_grab_button == 1) {
                g_input.client_grab_entry = nullptr;
                g_input.client_grab_button = 0;
            }
        } else if (ev.type == EVT_KEY_DOWN) {
            if (ev.key.scancode == 56 || ev.key.scancode == 184) {
                g_input.alt_down = true;
                continue;
            }
            if (g_input.alt_down && ev.key.scancode == 15) {
                cycle_user_window_focus(registry);
                continue;
            }
            if (ev.key.c == 29) {
                if (g_index.active)
                    close_index();
                else
                    open_index();
                continue;
            }

            if (g_index.active) {
                if (ev.key.c == 27) {
                    close_index();
                } else if (ev.key.c == '\b') {
                    if (g_index.query_len > 0) {
                        g_index.query[--g_index.query_len] = '\0';
                        update_index_search();
                    }
                } else if (ev.key.c == '\n') {
                    activate_index_selection(registry);
                } else if (static_cast<uint8_t>(ev.key.c) == KEY_UP_ARROW) {
                    if (g_index.result_count > 0) {
                        if (g_index.selected_index <= 0)
                            g_index.selected_index = g_index.result_count - 1;
                        else
                            g_index.selected_index--;
                        DirtyRect r = index_overlay_bounds();
                        enqueue_damage_rect(r.x, r.y, r.w, r.h);
                    }
                } else if (static_cast<uint8_t>(ev.key.c) == KEY_DOWN_ARROW) {
                    if (g_index.result_count > 0) {
                        if (g_index.selected_index >= g_index.result_count - 1)
                            g_index.selected_index = 0;
                        else
                            g_index.selected_index++;
                        DirtyRect r = index_overlay_bounds();
                        enqueue_damage_rect(r.x, r.y, r.w, r.h);
                    }
                } else if (ev.key.c >= 32 && ev.key.c < 127) {
                    if (g_index.query_len < 63) {
                        g_index.query[g_index.query_len++] = ev.key.c;
                        g_index.query[g_index.query_len] = '\0';
                        update_index_search();
                    }
                }
                continue;
            }

            if (g_control_center.open) {
                if (ev.key.c == 27)
                    close_control_center();
                continue;
            }

            int focus = find_registry_focused_user_window(registry);
            if (focus >= WM_FIRST_USER_WINDOW) {
                post_key_event_to_window(g_windows[focus], EVT_KEY_DOWN, ev.key.c, ev.key.scancode);
            }
        } else if (ev.type == EVT_KEY_UP) {
            if (ev.key.scancode == 56 || ev.key.scancode == 184)
                g_input.alt_down = false;
        } else if (ev.type == EVT_MOUSE_SCROLL) {
            if (g_index.active) {
                if (g_index.result_count > 0) {
                    if (ev.mouse.scroll_y > 0) {
                        if (g_index.selected_index <= 0)
                            g_index.selected_index = g_index.result_count - 1;
                        else
                            g_index.selected_index--;
                    } else if (ev.mouse.scroll_y < 0) {
                        if (g_index.selected_index >= g_index.result_count - 1)
                            g_index.selected_index = 0;
                        else
                            g_index.selected_index++;
                    }
                    DirtyRect r = index_overlay_bounds();
                    enqueue_damage_rect(r.x, r.y, r.w, r.h);
                }
                continue;
            }
            if (g_control_center.open) {
                handle_control_center_scroll(registry, g_input.mouse_x, g_input.mouse_y, ev.mouse.scroll_y);
                continue;
            }
            if (g_input.pointer_down || g_storage_prompt.visible || g_context_menu.open)
                continue;

            int tgt = -1;
            for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; i--) {
                if (is_window_visible(g_windows[i]) &&
                    point_targets_window_client_for_input(g_windows[i], g_input.mouse_x, g_input.mouse_y)) {
                    tgt = i;
                    break;
                }
            }
            if (tgt >= 2) {
                int scroll_step = gui_scaled_metric(48);
                if (scroll_step < 16)
                    scroll_step = 16;
                if (scroll_window_content(g_windows[tgt], -ev.mouse.scroll_x * scroll_step,
                                          -ev.mouse.scroll_y * scroll_step))
                    continue;
                post_mouse_event_to_window(g_windows[tgt], ev.type, g_input.mouse_x, g_input.mouse_y, 0,
                                           ev.mouse.scroll_y);
                continue;
            }
            if (system_window_hit(g_input.mouse_x, g_input.mouse_y) >= 0)
                continue;

            int reorder = -1;
            if (ev.mouse.scroll_y > 0) {
                int top = find_top_visible_user_window();
                if (top >= 2)
                    reorder = send_window_to_back(top);
            } else if (ev.mouse.scroll_y < 0) {
                for (int i = WM_FIRST_USER_WINDOW; i < g_window_count; i++) {
                    if (is_window_visible(g_windows[i])) {
                        reorder = bring_window_to_front(i);
                        break;
                    }
                }
            }
            if (reorder >= 2) {
                g_input.hover_frame_index = -1;
                g_input.hover_resize_edges = RESIZE_NONE;
                g_input.hover_button = -1;
                invalidate_window_visibility_cache();
                enqueue_damage_rect(0, 0, static_cast<int>(g_screen.width), static_cast<int>(g_screen.height));
                focus_window(find_top_visible_user_window(), false);
            }
        } else if (ev.type == EVT_MOUSE_DOWN || ev.type == EVT_MOUSE_UP) {
            if (ev.type == EVT_MOUSE_UP && g_input.client_grab_entry &&
                g_input.client_grab_button == ev.mouse.button) {
                int grabbed = find_window_by_entry(g_input.client_grab_entry);
                if (grabbed >= WM_FIRST_USER_WINDOW && is_window_visible(g_windows[grabbed])) {
                    post_mouse_event_to_window(g_windows[grabbed], EVT_MOUSE_UP, g_input.mouse_x, g_input.mouse_y,
                                               ev.mouse.button);
                }
                g_input.client_grab_entry = nullptr;
                g_input.client_grab_button = 0;
                continue;
            }
            int focus = find_registry_focused_user_window(registry);
            if (focus >= 2 && !pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y) &&
                point_targets_window_client_for_input(g_windows[focus], g_input.mouse_x, g_input.mouse_y)) {
                post_mouse_event_to_window(g_windows[focus], ev.type, g_input.mouse_x, g_input.mouse_y,
                                           ev.mouse.button);
                if (ev.type == EVT_MOUSE_DOWN) {
                    g_input.client_grab_entry = g_windows[focus].entry;
                    g_input.client_grab_button = ev.mouse.button;
                }
            }
        }
    }

    if (g_input.have_pending_move) {
        apply_mouse_move(registry, g_input.pending_mouse_x, g_input.pending_mouse_y);
        g_input.have_pending_move = false;
    }
}
