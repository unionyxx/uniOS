#include "wm_core.h"

Surface g_screen;
Surface g_backbuffer;
Surface g_presentbuffer;
Surface g_wallpaper;
Surface g_menubar_blur;
Surface g_dock_blur;
Surface g_menubar_blur_source;
Surface g_dock_blur_source;
DisplayCaps g_display_caps;
bool g_display_copy_path = false;

DisplayBufferHandle g_presentbuffer_handle = 0;
PresentBufferSlot g_presentbuffer_slots[MAX_PRESENT_BUFFER_SLOTS] = {};
uint32_t g_presentbuffer_slot_count = 0;
uint32_t g_presentbuffer_active_slot = 0;
DisplayQueueState g_display_queue = {};
WmFrameStats g_frame_stats = {};
WmBenchState g_bench = {};

Window g_windows[MAX_WINDOWS];
int g_window_count = 0;
int g_add_fail_logs = 0;
uint32_t g_system_flags = SYSTEM_FLAG_SHOW_DESKTOP_GRID;

DirtyRect g_dirty_rects[MAX_DIRTY_RECTS];
DirtyRect g_window_outer_cache[MAX_WINDOWS];
DirtyRect g_window_client_cache[MAX_WINDOWS];
bool g_window_visible_cache[MAX_WINDOWS];
DirtyRect g_window_visible_regions[MAX_WINDOWS][MAX_VISIBLE_REGIONS];
int g_window_visible_region_count[MAX_WINDOWS];
bool g_window_visible_region_overflow[MAX_WINDOWS] = {};
int g_dirty_count = 0;
bool g_window_visibility_cache_dirty = true;
bool g_dirty_frame_ready = false;

ContextMenuState g_context_menu = {};
StoragePromptState g_storage_prompt = {};
WmInputState g_input;

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
