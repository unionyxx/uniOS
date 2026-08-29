#include "wm_main.h"
#include "wm_present.h"
#include "wm_window.h"
#include "wm_input.h"
#include "wm_damage.h"
#include "wm_settings.h"
#include "wm_overlays.h"
#include "wm_metrics.h"

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    Registry *registry = wm_bootstrap();
    if (!registry)
        return 1;

    Event ev;

    while (true) {
        uint64_t t0 = get_ticks();
        uint64_t frame_tsc_start = wm_tsc_now();
        uint64_t t_events_start = get_ticks();
        (void)t0;
        (void)t_events_start;

        wm_handle_events(registry, ev);
        apply_pending_window_bounds();
        wm_drain_display_events();
        smp_rmb();

        wm_sync_registry(registry);

        registry->mouse_x = g_input.mouse_x;
        registry->mouse_y = g_input.mouse_y;
        update_hover_feedback();
        update_cursor_kind();
        if (g_context_menu.open) {
            update_context_menu_hover(registry, g_input.mouse_x, g_input.mouse_y);
        }

        wm_adopt_windows(registry);
        wm_commit_windows(registry);
        wm_apply_focus_requests();

        bool manip = g_input.pointer_down && g_input.drag_index >= 2;
        bool inter = manip || g_input.hover_resize_edges != RESIZE_NONE || g_input.hover_button >= 0 ||
                     g_context_menu.open || g_storage_prompt.visible;
        bool resizing = manip && g_input.drag_edges != RESIZE_NONE;
        uint32_t limit = g_display_copy_path ? 1u : MAX_PENDING_PRESENTS;

        wm_reap_dead_owners();

        // Scripted compositor benchmark: drives synthetic drag/resize geometry
        // changes so frame cost is measurable without manual input.
        wm_bench_tick(registry);

        // Keep the stats overlay repainting while it is visible.
        if ((g_system_flags & SYSTEM_FLAG_SHOW_DEBUG_STATS) != 0) {
            DirtyRect stats_damage = {};
            wm_stats_overlay_bounds(nullptr, &stats_damage);
            enqueue_damage_rect(stats_damage.x, stats_damage.y, stats_damage.w, stats_damage.h);
        }

        if (!wm_build_frame(registry, manip, inter, resizing, limit))
            continue;

        if (wm_present_end_frame(registry, manip, inter, limit, frame_tsc_start))
            continue;
    }
    return 0;
}
