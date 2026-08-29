#include "wm_overlays.h"
#include "wm_window.h"

void publish_settings_changed(Registry *registry)
{
    if (!registry)
        return;
    uint32_t next = (registry->settings_generation == 0xFFFFFFFFu) ? 1u : registry->settings_generation + 1u;
    registry->settings_generation = next;
    asm volatile("sfence" ::: "memory");
}

void show_desktop_windows()
{
    for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; i--)
        if (is_window_visible(g_windows[i]) && is_user_window(g_windows[i]))
            minimize_window(i);
}


void launch_or_focus_app(Registry *registry, const char *title, const char *path)
{
    const uint32_t win_limit = registry->window_count > MAX_WINDOWS ? MAX_WINDOWS : registry->window_count;
    for (uint32_t i = WM_FIRST_USER_WINDOW; i < win_limit; i++) {
        WindowEntry &e = registry->windows[i];
        if (!e.ready || !gui_shm_id_is_valid(e.shm_id) || !e.owner_pid || !gui_window_title_matches(e.title, title))
            continue;
        if (e.state == WIN_MINIMIZED || e.state == WIN_HIDDEN)
            e.request_restore = true;
        e.request_focus = true;
        asm volatile("sfence" ::: "memory");
        return;
    }
    if (fork() == 0) {
        exec(path);
        exit(1);
    }
}

