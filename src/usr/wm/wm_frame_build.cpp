#include "wm_core.h"

// Identity alias mode: on synchronous copy-path backends the scene buffer is
// itself the present buffer, so the scene->present blit disappears. The
// software cursor is then baked into the scene; g_scene_cursor_rect tracks the
// baked rect so the next frame can erase it before compositing.
bool g_scene_is_presentbuffer = false;
static bool g_scene_cursor_baked = false;
static DirtyRect g_scene_cursor_rect = {};

void wm_scene_mark_presentbuffer()
{
    g_scene_is_presentbuffer = true;
}

void wm_scene_mark_cursor_baked()
{
    if (!g_scene_is_presentbuffer)
        return;
    g_scene_cursor_baked = true;
    gui_get_cursor_bounds(g_input.cursor_kind, g_input.mouse_x, g_input.mouse_y, &g_scene_cursor_rect.x,
                          &g_scene_cursor_rect.y, &g_scene_cursor_rect.w, &g_scene_cursor_rect.h);
}

// Insert a damage rect at the head of the queue so it composes before any
// window-move pixel shifts in the same frame (identity-alias cursor erase).
static void prepend_damage_rect(const DirtyRect &rect)
{
    DirtyRect clipped = rect;
    if (!clip_dirty_rect_to_screen(clipped))
        return;
    int count = clamp_dirty_rect_count(g_dirty_count);
    if (count >= MAX_DIRTY_RECTS) {
        enqueue_damage_rect(clipped.x, clipped.y, clipped.w, clipped.h);
        return;
    }
    for (int i = count; i > 0; i--)
        g_dirty_rects[i] = g_dirty_rects[i - 1];
    g_dirty_rects[0] = clipped;
    g_dirty_count = count + 1;
}

bool wm_build_frame(Registry *registry, bool manip, bool inter, bool resizing, uint32_t limit)
{
    if (g_dirty_frame_ready || g_dirty_count <= 0)
        return true;

    if (g_window_visibility_cache_dirty) {
        refresh_window_cache();
        refresh_window_visible_regions();
        g_window_visibility_cache_dirty = false;
    }
    normalize_dirty_rects(inter);
    // Collapse only when the rect count actually threatens the budget:
    // collapsing two small distant rects during a resize turns them
    // into a screen-spanning recompose every frame.
    if (resizing && g_dirty_count > MAX_DIRTY_RECTS / 2) {
        collapse_dirty_rects_to_bounds();
    }

    uint32_t build_pending = wm::pending_presents(wm_present_last_sequence(), g_display_queue.completed_sequence);
    if (build_pending) {
        refresh_display_queue_from_status();
        build_pending = wm::pending_presents(wm_present_last_sequence(), g_display_queue.completed_sequence);
    }

    wm::PresentPolicyDecision build_action = wm::choose_present_policy(
        {build_pending, limit, (g_display_caps.flags & DISPLAY_FLAG_STRICT_SYNC_ONLY) != 0, inter,
         g_display_copy_path, manip});

    if (build_action == wm::PresentPolicyDecision::Skip) {
        g_frame_stats.frames_skipped++;
        yield();
        return false;
    }

    wm_cursor_begin_frame();

    if (g_scene_is_presentbuffer || select_presentbuffer_slot_for_frame()) {
        uint64_t now = get_ticks();
        bool toast_expired = false;

        int toast_idx = (g_notifications.head - 1 + MAX_NOTIFICATIONS) % MAX_NOTIFICATIONS;
        for (int i = 0; i < g_notifications.count; i++) {
            Notification &notif = g_notifications.history[toast_idx];
            if (notif.active_toast && (now - notif.timestamp_ticks > TOAST_DURATION_TICKS)) {
                notif.active_toast = false;
                toast_expired = true;
            }
            toast_idx = (toast_idx - 1 + MAX_NOTIFICATIONS) % MAX_NOTIFICATIONS;
        }

        if (toast_expired) {
            int toast_w = gui_scaled_metric(320);
            int toast_h = gui_scaled_metric(76);
            int margin = gui_space_2();
            DirtyRect toast_box = {static_cast<int>(g_backbuffer.width) - toast_w - margin, wm_menubar_h() + margin,
                                   toast_w, toast_h};
            enqueue_damage_rect(toast_box.x - 16, toast_box.y - 16, toast_box.w + 32, toast_h + 32);
        }

        int focus = find_registry_focused_user_window(registry);

        DirtyRect cc_damage = {};
        bool has_cc_damage = false;
        if (g_control_center.open) {
            DirtyRect cc_box = control_center_bounds();
            DirtyRect notif_box = {cc_box.x, cc_box.y + cc_box.h + gui_space_2(), cc_box.w, gui_scaled_metric(240)};
            cc_damage = rect_expand(rect_union(cc_box, notif_box), gui_scaled_metric(14));
            has_cc_damage = true;
        }

        DirtyRect toast_damage = {};
        bool has_toast_damage = false;
        if (g_notifications.count > 0) {
            int toast_w = gui_scaled_metric(320);
            int toast_h = gui_scaled_metric(76);
            int margin = gui_space_2();
            DirtyRect toast_box = {static_cast<int>(g_backbuffer.width) - toast_w - margin, wm_menubar_h() + margin,
                                   toast_w, toast_h};
            toast_damage = rect_expand(toast_box, gui_scaled_metric(14));
            has_toast_damage = true;
        }

        for (int d = 0; d < g_dirty_count; d++) {
            DirtyRect &r = g_dirty_rects[d];
            if (has_cc_damage && rect_intersection(r, cc_damage, nullptr)) {
                r = rect_union(r, cc_damage);
                clip_dirty_rect_to_screen(r);
            }
            if (has_toast_damage && rect_intersection(r, toast_damage, nullptr)) {
                r = rect_union(r, toast_damage);
                clip_dirty_rect_to_screen(r);
            }
        }

        wm::DirtyRect optimized_rects[MAX_DIRTY_RECTS] = {};
        int optimized_count = 0;
        for (int d = 0; d < g_dirty_count; d++) {
            DirtyRect &r = g_dirty_rects[d];
            if (r.w > 0 && r.h > 0) {
                wm::enqueue_damage_rect(optimized_rects, &optimized_count, MAX_DIRTY_RECTS,
                                        static_cast<int>(g_screen.width), static_cast<int>(g_screen.height),
                                        to_policy_rect(r));
            }
        }

        for (int d = 0; d < optimized_count; d++) {
            g_dirty_rects[d] = from_policy_rect(optimized_rects[d]);
        }
        g_dirty_count = optimized_count;

        bool hw_cursor_allowed = wm_cursor_backend_allowed();
        wm_cursor_erase_previous_software(hw_cursor_allowed, inter);

        DirtyRect cursor_rect = {};
        bool draw_cursor = prepare_cursor_overlay_damage(inter, &cursor_rect, !hw_cursor_allowed);
        bool draw_software_cursor = false;
        if (draw_cursor)
            draw_software_cursor = wm_cursor_select_plane(hw_cursor_allowed, cursor_rect, inter);

        // Identity alias: the software cursor is baked into the scene.
        // Erase last frame's baked cursor (recompose from layers) before
        // any window-move pixel shifts run in this frame's compose pass.
        if (g_scene_is_presentbuffer && g_scene_cursor_baked) {
            g_scene_cursor_baked = false;
            if (!dirty_set_contains_rect(g_scene_cursor_rect))
                prepend_damage_rect(g_scene_cursor_rect);
        }

        DirtyRect compose_union = {0, 0, 0, 0};
        bool has_compose_union = false;
        int dirty_count = clamp_dirty_rect_count(g_dirty_count);

        uint64_t compose_tsc_start = wm_tsc_now();
        for (int d = 0; d < dirty_count; d++) {
            DirtyRect &r = g_dirty_rects[d];
            if (r.w <= 0 || r.h <= 0)
                continue;

            if (!has_compose_union) {
                compose_union = r;
                has_compose_union = true;
            } else {
                compose_union = rect_union(compose_union, r);
            }

            if (!compose_rect_clipped(r, focus, g_input.hover_frame_index, g_input.hover_button, registry)) {
                compose_rect_unclipped(r, focus, g_input.hover_frame_index, g_input.hover_button, registry);
            }
            if (!g_scene_is_presentbuffer)
                gui_blit_rect(&g_presentbuffer, &g_backbuffer, r.x, r.y, r.x, r.y, r.w, r.h);
        }
        g_frame_stats.last_compose_ticks = wm_tsc_now() - compose_tsc_start;
        g_frame_stats.total_compose_ticks += g_frame_stats.last_compose_ticks;

        if (has_compose_union) {
            capture_shell_backdrop_for_rect(compose_union, const_cast<Registry *>(registry));
        }
        if (!g_display_copy_path)
            flush_shell_blur_updates(registry);

        if (draw_cursor && draw_software_cursor) {
            // Clear any stale pixels then bake the software cursor; the
            // hardware plane needs neither (it never touches frames).
            // In identity-alias mode the scene is the present buffer, so
            // the erase happened during compose and the cursor bakes
            // directly into the scene.
            if (!g_scene_is_presentbuffer)
                gui_blit_rect(&g_presentbuffer, &g_backbuffer, cursor_rect.x, cursor_rect.y, cursor_rect.x,
                              cursor_rect.y, cursor_rect.w, cursor_rect.h);
            // gui_draw_cursor_kind expects the HOTSPOT position (mouse coordinates),
            // not the bounds top-left. It subtracts the hotspot internally.
            gui_draw_cursor_kind(&g_presentbuffer, g_input.mouse_x, g_input.mouse_y, g_input.cursor_kind);
            g_frame_stats.cursor_software_frames++;
            if (g_scene_is_presentbuffer) {
                g_scene_cursor_baked = true;
                g_scene_cursor_rect = cursor_rect;
            }
        } else if (draw_cursor) {
            g_frame_stats.cursor_backend_frames++;
        }
        wm_cursor_finish_frame(draw_cursor, draw_software_cursor, cursor_rect);
        wm_stats_note_dirty_set(g_dirty_rects, g_dirty_count);
        g_frame_stats.frames_built++;
        g_dirty_frame_ready = true;
    }
    return true;
}
