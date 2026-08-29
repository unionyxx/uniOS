#include "wm_core.h"

int find_top_visible_user_window()
{
    for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; i--)
        if (is_window_visible(g_windows[i]) && is_user_window(g_windows[i]))
            return i;
    return -1;
}

static int window_slot(const Registry *registry, const Window &w)
{
    return w.entry ? (int)(w.entry - &registry->windows[0]) : -1;
}

int find_window_by_entry(const WindowEntry *entry)
{
    if (!entry)
        return -1;
    for (int i = 0; i < g_window_count; i++)
        if (g_windows[i].entry == entry)
            return i;
    return -1;
}

int find_window_by_shm(int shm_id)
{
    if (!gui_shm_id_is_valid(shm_id))
        return -1;
    for (int i = 0; i < g_window_count; i++)
        if (g_windows[i].shm_id == shm_id)
            return i;
    return -1;
}

int find_registry_focused_user_window(const Registry *registry)
{
    if (!registry || registry->focused_window < WM_FIRST_USER_WINDOW || registry->focused_window >= MAX_WINDOWS)
        return -1;
    const WindowEntry *focused_entry = &registry->windows[registry->focused_window];
    for (int i = WM_FIRST_USER_WINDOW; i < g_window_count; i++) {
        if (g_windows[i].entry == focused_entry && is_window_visible(g_windows[i]) && is_user_window(g_windows[i]))
            return i;
    }
    return -1;
}

bool focus_window_owner(const Window *w)
{
    return syscall1(SYS_GUI_SET_FOCUS, w ? w->owner_pid : 0) == 0;
}

void publish_focus(Registry *registry, const Window *w)
{
    if (!registry)
        return;
    uint32_t prev_pid = registry->focused_owner_pid;
    uint32_t next_pid = 0;
    const WindowEntry *next_entry = nullptr;
    if (!w || !w->entry || !is_user_window(*w) || !is_window_visible(*w)) {
        registry->focused_window = -1;
        registry->focused_owner_pid = 0;
    } else {
        registry->focused_window = window_slot(registry, *w);
        registry->focused_owner_pid = w->owner_pid;
        next_pid = w->owner_pid;
        next_entry = w->entry;
    }
    asm volatile("sfence" ::: "memory");
    post_focus_change_events(prev_pid, next_pid);

    // Move tracking is keyed on the focused (or grabbed) window; a focus
    // transition invalidates it. Deliver the pending leave so clients drop
    // stale hover state when the pointer is no longer theirs.
    if (g_input.move_target_entry && g_input.move_target_entry != next_entry &&
        g_input.move_target_entry != g_input.client_grab_entry) {
        int target = find_window_by_entry(g_input.move_target_entry);
        if (target >= 0)
            post_plain_event_to_window(g_windows[target], EVT_MOUSE_LEAVE);
        g_input.move_target_entry = nullptr;
    }
}

void clear_window_focus(Registry *registry)
{
    int previous = find_registry_focused_user_window(registry);
    if (focus_window_owner(nullptr))
        publish_focus(registry, nullptr);
    if (previous >= WM_FIRST_USER_WINDOW && previous < g_window_count && is_window_visible(g_windows[previous]))
        mark_window_chrome_damage(g_windows[previous]);
}

void clear_hover_feedback_state()
{
    if (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_frame_index < g_window_count)
        mark_window_chrome_damage(g_windows[g_input.hover_frame_index]);
    g_input.hover_frame_index = -1;
    g_input.hover_resize_edges = RESIZE_NONE;
    g_input.hover_button = -1;
}

static void remap_interaction_indices(WindowEntry *drag_entry, WindowEntry *hover_entry)
{
    if (drag_entry)
        g_input.drag_index = find_window_by_entry(drag_entry);
    if (hover_entry)
        g_input.hover_frame_index = find_window_by_entry(hover_entry);
    // Entry pointers survive z-order reshuffles; nothing to remap for the
    // grab/move targets, but drop them if the window disappeared entirely.
    if (g_input.client_grab_entry && find_window_by_entry(g_input.client_grab_entry) < 0)
        g_input.client_grab_entry = nullptr;
    if (g_input.move_target_entry && find_window_by_entry(g_input.move_target_entry) < 0)
        g_input.move_target_entry = nullptr;
}

int bring_window_to_front(int index)
{
    if (index < WM_FIRST_USER_WINDOW || index >= g_window_count || index == g_window_count - 1)
        return index;
    WindowEntry *drag_entry = (g_input.drag_index >= WM_FIRST_USER_WINDOW && g_input.drag_index < g_window_count)
                                  ? g_windows[g_input.drag_index].entry
                                  : nullptr;
    WindowEntry *hover_entry =
        (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_frame_index < g_window_count)
            ? g_windows[g_input.hover_frame_index].entry
            : nullptr;
    Window temp = g_windows[index];
    for (int i = index; i < g_window_count - 1; i++)
        g_windows[i] = g_windows[i + 1];
    g_windows[g_window_count - 1] = temp;
    remap_interaction_indices(drag_entry, hover_entry);
    return g_window_count - 1;
}

int send_window_to_back(int index)
{
    if (index < WM_FIRST_USER_WINDOW || index >= g_window_count || index == WM_FIRST_USER_WINDOW)
        return index;
    WindowEntry *drag_entry = (g_input.drag_index >= WM_FIRST_USER_WINDOW && g_input.drag_index < g_window_count)
                                  ? g_windows[g_input.drag_index].entry
                                  : nullptr;
    WindowEntry *hover_entry =
        (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_frame_index < g_window_count)
            ? g_windows[g_input.hover_frame_index].entry
            : nullptr;
    Window temp = g_windows[index];
    for (int i = index; i > WM_FIRST_USER_WINDOW; i--)
        g_windows[i] = g_windows[i - 1];
    g_windows[2] = temp;
    remap_interaction_indices(drag_entry, hover_entry);
    return 2;
}

int focus_window(int index, bool raise)
{
    if (index < 0 || index >= g_window_count)
        return index;
    WindowEntry *target_entry = g_windows[index].entry;
    WindowEntry *prev_entry = nullptr;
    WindowEntry *hover_entry = nullptr;
    int prev_idx = find_registry_focused_user_window(gui_registry());
    if (prev_idx >= WM_FIRST_USER_WINDOW && prev_idx < g_window_count)
        prev_entry = g_windows[prev_idx].entry;
    if (g_input.hover_frame_index >= WM_FIRST_USER_WINDOW && g_input.hover_frame_index < g_window_count)
        hover_entry = g_windows[g_input.hover_frame_index].entry;

    bool target_was_focused = (prev_idx == index);
    if (!target_was_focused && prev_idx >= WM_FIRST_USER_WINDOW && prev_idx < g_window_count && target_entry &&
        g_windows[prev_idx].entry == target_entry) {
        target_was_focused = true;
    }

    bool z_order_changed = false;
    if (raise) {
        int raised_index = bring_window_to_front(index);
        z_order_changed = raised_index != index;
        index = raised_index;
        if (z_order_changed)
            invalidate_window_visibility_cache();
        g_input.hover_frame_index = -1;
        g_input.hover_resize_edges = RESIZE_NONE;
        g_input.hover_button = -1;
    }
    Window &w = g_windows[index];
    if (w.entry && w.entry->owner_pid)
        w.owner_pid = w.entry->owner_pid;
    if (index >= WM_FIRST_USER_WINDOW && is_window_visible(w)) {
        if (focus_window_owner(&w))
            publish_focus(gui_registry(), &w);
    } else {
        if (focus_window_owner(nullptr))
            publish_focus(gui_registry(), nullptr);
    }

    bool hover_cleared_on_target = raise && target_entry && hover_entry == target_entry;
    bool focus_changed = !target_was_focused;
    if (z_order_changed)
        mark_window_frame_damage(w);
    else if (focus_changed || hover_cleared_on_target)
        mark_window_chrome_damage(w);
    int updated_prev = find_window_by_entry(prev_entry);
    if (updated_prev >= 2 && updated_prev < g_window_count && updated_prev != index &&
        is_window_visible(g_windows[updated_prev]))
        mark_window_chrome_damage(g_windows[updated_prev]);
    int updated_hover = find_window_by_entry(hover_entry);
    if (updated_hover >= 2 && updated_hover < g_window_count && updated_hover != index &&
        updated_hover != updated_prev && is_window_visible(g_windows[updated_hover]))
        mark_window_chrome_damage(g_windows[updated_hover]);
    return index;
}

void restore_window(int index, bool raise)
{
    if (index < WM_FIRST_USER_WINDOW || index >= g_window_count)
        return;
    Window &w = g_windows[index];
    if (!w.entry)
        return;
    int rw = w.entry->restore_w > 0 ? w.entry->restore_w : w.w;
    int rh = w.entry->restore_h > 0 ? w.entry->restore_h : w.h;
    w.entry->state = WIN_NORMAL;
    w.active = true;
    set_window_bounds(w, w.entry->restore_x, w.entry->restore_y, rw, rh);
    close_context_menu();
    invalidate_window_visibility_cache();
    if (raise)
        focus_window(index, true);
}

void maximize_window(int index)
{
    if (index < WM_FIRST_USER_WINDOW || index >= g_window_count || !g_windows[index].entry)
        return;
    Window &w = g_windows[index];
    if (w.entry->state != WIN_MAXIMIZED) {
        w.entry->restore_x = w.x;
        w.entry->restore_y = w.y;
        w.entry->restore_w = w.w;
        w.entry->restore_h = w.h;
    }
    w.entry->state = WIN_MAXIMIZED;
    w.active = true;
    set_window_bounds(w, wm_desktop_margin(), wm_menubar_h() + wm_title_bar_h() + wm_desktop_margin(),
                      (int)g_screen.width - wm_desktop_margin() * 2,
                      (int)g_screen.height - wm_dock_reserved_h() -
                          (wm_menubar_h() + wm_title_bar_h() + wm_desktop_margin()));
    close_context_menu();
    invalidate_window_visibility_cache();
    focus_window(index, true);
}

void toggle_maximize_window(int index)
{
    if (index >= WM_FIRST_USER_WINDOW && index < g_window_count && g_windows[index].entry) {
        if (g_windows[index].entry->state == WIN_MAXIMIZED)
            restore_window(index, true);
        else
            maximize_window(index);
    }
}

void minimize_window(int index)
{
    if (index < WM_FIRST_USER_WINDOW || index >= g_window_count || !g_windows[index].entry)
        return;
    const Window &w = g_windows[index];
    if (w.entry->state == WIN_NORMAL) {
        w.entry->restore_x = w.x;
        w.entry->restore_y = w.y;
        w.entry->restore_w = w.w;
        w.entry->restore_h = w.h;
    }
    w.entry->state = WIN_MINIMIZED;
    asm volatile("sfence" ::: "memory");
    mark_window_frame_damage(w);

    DirtyRect covered = window_opaque_bounds(w);
    capture_shell_backdrop_for_rect(covered, gui_registry());

    if (g_input.drag_index == index) {
        g_input.drag_index = -1;
        g_input.drag_edges = RESIZE_NONE;
        g_input.pointer_down = false;
    }
    if (g_input.hover_frame_index == index) {
        g_input.hover_frame_index = -1;
        g_input.hover_resize_edges = RESIZE_NONE;
        g_input.hover_button = -1;
    }
    if (g_input.client_grab_entry == w.entry)
        g_input.client_grab_entry = nullptr;
    if (g_input.move_target_entry == w.entry) {
        post_plain_event_to_window(w, EVT_MOUSE_LEAVE);
        g_input.move_target_entry = nullptr;
    }

    close_context_menu();
    invalidate_window_visibility_cache();

    int focus_idx = find_top_visible_user_window();
    Window *focus_target = focus_idx >= 0 ? &g_windows[focus_idx] : nullptr;
    if (focus_window_owner(focus_target))
        publish_focus(gui_registry(), focus_target);
    if (focus_idx >= WM_FIRST_USER_WINDOW)
        mark_window_chrome_damage(g_windows[focus_idx]);
}

void close_window(int index, bool kill_owner)
{
    if (index < WM_FIRST_USER_WINDOW || index >= g_window_count)
        return;
    Window doomed = g_windows[index];
    uint32_t owner = doomed.owner_pid;

    // A pending titlebar double-click must not survive this window: the
    // registry slot can be reused by a different window within the
    // double-click window (ABA).
    if (doomed.entry && doomed.entry == g_input.titlebar_click_entry) {
        g_input.titlebar_click_entry = nullptr;
        g_input.titlebar_click_shm_id = WIN_SHM_INVALID;
        g_input.titlebar_click_owner_pid = 0;
    }

    mark_window_frame_damage(doomed);

    DirtyRect covered = window_opaque_bounds(doomed);
    capture_shell_backdrop_for_rect(covered, gui_registry());

    close_context_menu();
    if (gui_shm_id_is_valid(doomed.shm_id)) {
        bool is_memfd = (doomed.shm_id & 0x40000000) != 0;
        if (is_memfd) {
            int fd = doomed.shm_id & ~0x40000000;
            size_t size = (size_t)doomed.buffer_w * doomed.buffer_h * 4;
            munmap(doomed.buffer, size);
            close(fd);
        } else {
            syscall1(SYS_SHM_UNMAP, (uint64_t)doomed.shm_id);
        }
    }
    gui_destroy_surface(&doomed.decoration_cache);
    gui_destroy_surface(&doomed.button_cache);
    wm_resize_snapshot_release(doomed);
    if (doomed.entry) {
        memset(doomed.entry, 0, sizeof(*doomed.entry));
        doomed.entry->shm_id = WIN_SHM_INVALID;
        doomed.entry->state = WIN_HIDDEN;
        asm volatile("sfence" ::: "memory");
    }

    for (int i = index; i < g_window_count - 1; i++)
        g_windows[i] = g_windows[i + 1];
    g_window_count--;
    if (g_window_count >= 0 && g_window_count < MAX_WINDOWS)
        memset(&g_windows[g_window_count], 0, sizeof(g_windows[g_window_count]));

    if (g_input.drag_index == index) {
        g_input.drag_index = -1;
        g_input.drag_edges = RESIZE_NONE;
        g_input.pointer_down = false;
    } else if (g_input.drag_index > index)
        g_input.drag_index--;

    if (g_input.hover_frame_index == index) {
        g_input.hover_frame_index = -1;
        g_input.hover_resize_edges = RESIZE_NONE;
        g_input.hover_button = -1;
    } else if (g_input.hover_frame_index > index)
        g_input.hover_frame_index--;

    if (g_input.client_grab_entry == doomed.entry)
        g_input.client_grab_entry = nullptr;
    if (g_input.move_target_entry == doomed.entry)
        g_input.move_target_entry = nullptr;

    invalidate_window_visibility_cache();
    int focus_idx = find_top_visible_user_window();
    Window *focus_target = focus_idx >= 0 ? &g_windows[focus_idx] : nullptr;
    if (focus_window_owner(focus_target))
        publish_focus(gui_registry(), focus_target);
    if (focus_idx >= WM_FIRST_USER_WINDOW)
        mark_window_chrome_damage(g_windows[focus_idx]);
    // Kill only on user-initiated close. Reaping an already-dead owner (or
    // honoring the app's own request_close) and then killing by PID can hit a
    // recycled PID and terminate an unrelated process.
    if (kill_owner && owner)
        syscall2(SYS_KILL, owner, SIGTERM);
}

bool add_win_internal(int shm_id, int x, int y, int w, int h, const char *title, Damage *d_ptr, WindowEntry *entry,
                      bool transparent)
{
    int bw = (entry && entry->buffer_w > 0) ? entry->buffer_w : w;
    int bh = (entry && entry->buffer_h > 0) ? entry->buffer_h : h;
    uint64_t req_size = (uint64_t)bw * bh * 4;

    bool is_memfd = (shm_id & 0x40000000) != 0;
    bool map_ok = false;
    uint64_t ptr = 0;

    if (is_memfd) {
        int fd = shm_id & ~0x40000000;
        // Reject buffers larger than the backing file: mapping past EOF
        // succeeds but faults (taking the compositor down) on first read.
        uint64_t file_size = syscall1(SYS_FSIZE, (uint64_t)fd);
        if (file_size == (uint64_t)-1 || req_size > file_size) {
            if (g_add_fail_logs < 8) {
                LOG_WARN("wm", "add rejected: memfd=%d claims %ux%u but file is %llu bytes", fd, bw, bh,
                         (unsigned long long)file_size);
                g_add_fail_logs++;
            }
            return false;
        }
        void *mapped_ptr = mmap(NULL, req_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapped_ptr != MAP_FAILED) {
            ptr = reinterpret_cast<uint64_t>(mapped_ptr);
            map_ok = true;
        }
    } else {
        uint64_t shm_size = syscall1(SYS_SHM_INFO, (uint64_t)shm_id);
        if (w > 0 && h > 0 && gui_shm_id_is_valid(shm_id) && shm_size != (uint64_t)-1 && req_size <= shm_size) {
            uint64_t legacy_ptr = syscall1(SYS_SHM_MAP, (uint64_t)shm_id);
            if (legacy_ptr != 0 && legacy_ptr != (uint64_t)-1) {
                ptr = legacy_ptr;
                map_ok = true;
            }
        }
    }

    if (!map_ok) {
        if (g_add_fail_logs < 8) {
            LOG_WARN("wm", "add skipped/failed: shm=%d (invalid bounds/size/mapping)", shm_id);
            g_add_fail_logs++;
        }
        return false;
    }

    if (g_window_count >= MAX_WINDOWS) {
        if (g_add_fail_logs < 8) {
            LOG_WARN("wm", "table full");
            g_add_fail_logs++;
        }
        if (is_memfd) {
            munmap(reinterpret_cast<void *>(ptr), req_size);
            close(shm_id & ~0x40000000);
        } else {
            syscall1(SYS_SHM_UNMAP, (uint64_t)shm_id);
        }
        return false;
    }

    w = w > bw ? bw : w;
    h = h > bh ? bh : h;

    Window &win = g_windows[g_window_count++];
    memset(&win, 0, sizeof(win));
    win.shm_id = shm_id;
    win.buffer = reinterpret_cast<uint32_t *>(ptr);
    win.owner_pid = entry ? entry->owner_pid : 0;
    win.x = x;
    win.y = y;
    win.w = w;
    win.h = h;
    win.target_x = x;
    win.target_y = y;
    win.target_w = w;
    win.target_h = h;
    win.buffer_w = bw;
    win.buffer_h = bh;
    win.last_rendered_x = x;
    win.last_rendered_y = y;
    win.last_rendered_w = w;
    win.last_rendered_h = h;
    win.scroll_x = 0;
    win.scroll_y = 0;
    win.min_w = entry ? entry->min_w : 0;
    win.min_h = entry ? entry->min_h : 0;
    win.active = true;
    win.transparent = transparent;
    win.needs_full_redraw = false;
    win.last_commit_ticks = get_ticks();
    win.damage_ptr = d_ptr;
    win.first_damage_received = false;
    win.entry = entry;
    strncpy(win.title, title ? title : "", 63);
    win.title[63] = '\0';

    if (entry) {
        entry->active = true;
        if (entry->state == WIN_HIDDEN)
            entry->state = WIN_NORMAL;
        asm volatile("sfence" ::: "memory");
    }
    if (transparent)
        enqueue_damage_rect(x, y, w, h);
    else {
        DirtyRect o = window_outer_bounds(win);
        enqueue_damage_rect(o.x, o.y, o.w, o.h);
    }
    invalidate_window_visibility_cache();
    return true;
}

