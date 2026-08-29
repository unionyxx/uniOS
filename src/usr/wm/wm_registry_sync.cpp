#include "wm_main.h"
#include "wm_settings.h"
#include "wm_render.h"
#include "wm_overlays.h"
#include "wm_damage.h"
#include "wm_present.h"
#include "wm_window.h"
#include "wm_metrics.h"

// Generation trackers: consume registry-side requests exactly once per
// generation bump so a restarting client cannot replay a stale request.
static uint32_t g_last_settings_gen = 0;
static uint32_t g_last_storage_gen = 0;
static uint32_t g_last_notify_gen = 0;
static GuiThemeMode g_applied_theme_mode = GUI_THEME_LIGHT;

void wm_registry_sync_init(Registry *registry, GuiThemeMode initial_theme)
{
    g_last_settings_gen = registry->settings_generation;
    g_last_storage_gen = registry->storage_request_generation;
    g_last_notify_gen = registry->notify_generation;
    g_applied_theme_mode = initial_theme;
}

void wm_sync_registry(Registry *registry)
{
    if (registry->settings_generation != g_last_settings_gen) {
        g_last_settings_gen = registry->settings_generation;
        GuiThemeMode next_theme = registry->theme_mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK;
        bool theme_changed = next_theme != g_applied_theme_mode;
        bool flags_changed = registry->system_flags != g_system_flags;
        bool transparency_changed = registry->transparency_level != g_control_center.transparency_level;

        g_system_flags = registry->system_flags;
        sync_control_center_state_from_registry(registry);

        if (theme_changed) {
            g_applied_theme_mode = next_theme;
            gui_apply_theme(next_theme);
            refresh_wm_metrics();
            reload_wallpaper(registry, true);
            recapture_shell_blur_sources(registry);
            enqueue_damage_rect(0, 0, static_cast<int>(g_screen.width), static_cast<int>(g_screen.height));
        } else if (flags_changed) {
            enqueue_damage_rect(0, 0, g_screen.width, wm_menubar_h());
            if (registry->window_count > 1) {
                enqueue_damage_rect(registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                                    registry->windows[1].h);
            }
        }
        if (transparency_changed) {
            capture_shell_backdrop_for_rect({0, 0, static_cast<int>(g_screen.width),
                                             static_cast<int>(g_screen.height)},
                                            registry);
            recapture_shell_blur_sources(registry);
            enqueue_damage_rect(0, 0, static_cast<int>(g_screen.width), static_cast<int>(g_screen.height));
        }
    }

    if (registry->storage_request_generation != g_last_storage_gen) {
        g_last_storage_gen = registry->storage_request_generation;
        if (!apply_storage_mode_request(registry, registry->storage_request_mode)) {
            registry->storage_mode = get_storage_mode();
            smp_wmb();
        }
    }

    if (registry->storage_mode != static_cast<uint32_t>(get_storage_mode())) {
        registry->storage_mode = get_storage_mode();
        smp_wmb();
    }
    if (registry->wallpaper_reload_requested) {
        registry->wallpaper_reload_requested = false;
        reload_wallpaper(registry, true);
        enqueue_damage_rect(0, 0, static_cast<int>(g_screen.width), static_cast<int>(g_screen.height));
    }

    if (registry->cp_toggle_requested) {
        registry->cp_toggle_requested = false;
        smp_wmb();
        toggle_control_center();
    }

    if (registry->notify_generation != g_last_notify_gen) {
        g_last_notify_gen = registry->notify_generation;
        char notify_title[sizeof(registry->notify_title)];
        char notify_message[sizeof(registry->notify_message)];
        memcpy(notify_title, (const void *)registry->notify_title, sizeof(notify_title));
        memcpy(notify_message, (const void *)registry->notify_message, sizeof(notify_message));
        notify_title[sizeof(notify_title) - 1] = '\0';
        notify_message[sizeof(notify_message) - 1] = '\0';
        if (notify_title[0] || notify_message[0])
            wm_push_notification(notify_title, notify_message);
    }
}
