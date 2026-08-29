#include "wm_core.h"

static WindowEntrySnapshot read_window_entry_snapshot(const WindowEntry &e)
{
    WindowEntrySnapshot s = {};
    s.shm_id = e.shm_id;
    s.x = e.x;
    s.y = e.y;
    s.w = e.w;
    s.h = e.h;
    s.position_serial = e.position_serial;
    s.buffer_w = e.buffer_w;
    s.buffer_h = e.buffer_h;
    s.content_w = e.content_w;
    s.content_h = e.content_h;
    s.min_w = e.min_w;
    s.min_h = e.min_h;
    s.scroll_x = e.scroll_x;
    s.scroll_y = e.scroll_y;
    s.flags = e.flags;
    s.state = e.state;
    s.owner_pid = e.owner_pid;
    s.resize_serial = e.resize_serial;
    s.buffer_resize_serial = e.buffer_resize_serial;
    s.buffer_generation = e.buffer_generation;
    s.buffer_ack_generation = e.buffer_ack_generation;
    s.active = e.active;
    s.ready = e.ready;
    s.request_close = e.request_close;
    s.request_focus = e.request_focus;
    s.request_minimize = e.request_minimize;
    s.request_maximize = e.request_maximize;
    s.request_restore = e.request_restore;

    memcpy(s.title, e.title, sizeof(s.title));
    s.title[sizeof(s.title) - 1] = '\0';
    return s;
}

static bool window_entry_snapshot_equal_for_commit(const WindowEntrySnapshot &a, const WindowEntrySnapshot &b)
{
    return a.shm_id == b.shm_id && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h &&
           a.position_serial == b.position_serial && a.buffer_w == b.buffer_w && a.buffer_h == b.buffer_h &&
           a.content_w == b.content_w && a.content_h == b.content_h && a.min_w == b.min_w && a.min_h == b.min_h &&
           a.scroll_x == b.scroll_x && a.scroll_y == b.scroll_y && a.flags == b.flags && a.state == b.state &&
           a.owner_pid == b.owner_pid && a.resize_serial == b.resize_serial &&
           a.buffer_resize_serial == b.buffer_resize_serial && a.buffer_generation == b.buffer_generation &&
           a.buffer_ack_generation == b.buffer_ack_generation && a.active == b.active && a.ready == b.ready &&
           memcmp(a.title, b.title, sizeof(a.title)) == 0;
}

static bool sample_stable_window_entry(const WindowEntry &entry, WindowEntrySnapshot *out)
{
    WindowEntrySnapshot a = read_window_entry_snapshot(entry);
    smp_rmb();
    WindowEntrySnapshot b = read_window_entry_snapshot(entry);

    if (!window_entry_snapshot_equal_for_commit(a, b))
        return false;
    if (out)
        *out = b;
    return true;
}

static void mark_titlebar_dirty(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    if (outer.w <= 0 || outer.h <= 0)
        return;

    int title_h = wm_title_bar_h() + wm_frame_border() + wm_frame_shadow_offset_y();
    if (title_h < 0)
        title_h = 0;
    if (title_h > outer.h)
        title_h = outer.h;
    if (title_h > 0)
        enqueue_damage_rect(outer.x, outer.y, outer.w, title_h);
}

static void sync_window_runtime_metadata(Window &w, const WindowEntrySnapshot &entry)
{
    if (!w.entry)
        return;

    const bool resize_serial_changed = w.entry_resize_serial != entry.resize_serial;
    const bool buffer_resize_serial_changed = w.buffer_resize_serial != entry.buffer_resize_serial;

    if (resize_serial_changed || buffer_resize_serial_changed) {
        w.entry_resize_serial = entry.resize_serial;
        w.buffer_resize_serial = entry.buffer_resize_serial;

        // Only the ack changes something visible (a posted configure alone
        // does not): the client finished redrawing the outstanding configure,
        // so the bounds flip to its pending geometry — the buffer remap above
        // already switched the displayed backing, so geometry and content
        // land in one frame.
        if (buffer_resize_serial_changed) {
            if (w.resize_configure_pending && w.buffer_resize_serial == w.pending_configure_serial) {
                apply_window_resize_flip(w);
            } else if (w.resize_configure_pending) {
                // Ack for a superseded configure; keep waiting for the
                // outstanding serial.
                g_frame_stats.resize_stale_acks++;
            }

            if (clamp_window_scroll(w) && w.entry) {
                w.entry->scroll_x = w.scroll_x;
                w.entry->scroll_y = w.scroll_y;
                smp_wmb();
            }

            w.needs_full_redraw = true;
            invalidate_window_decoration_cache(w);
            DirtyRect client = window_visible_client_bounds(w);
            enqueue_damage_rect(client.x, client.y, client.w, client.h);
            invalidate_window_visibility_cache();
        }
    }

    const int new_content_w = entry.content_w;
    const int new_content_h = entry.content_h;
    if (w.content_w != new_content_w || w.content_h != new_content_h) {
        w.content_w = new_content_w;
        w.content_h = new_content_h;
        if (clamp_window_scroll(w)) {
            if (w.entry) {
                w.entry->scroll_x = w.scroll_x;
                w.entry->scroll_y = w.scroll_y;
                smp_wmb();
            }
            DirtyRect client = window_visible_client_bounds(w);
            enqueue_damage_rect(client.x, client.y, client.w, client.h);
        }
    }

    const bool desired_transparent = (entry.flags & WIN_FLAG_TRANSPARENT) != 0;
    if (w.transparent != desired_transparent) {
        Window old = w;
        w.transparent = desired_transparent;
        w.needs_full_redraw = true;
        invalidate_window_decoration_cache(w);
        mark_window_transition_damage(old, w);
        invalidate_window_visibility_cache();
    }

    char entry_title[sizeof(w.title)];
    memcpy(entry_title, entry.title, sizeof(entry_title));
    entry_title[sizeof(entry_title) - 1] = '\0';
    if (strncmp(w.title, entry_title, sizeof(w.title)) != 0) {
        strncpy(w.title, entry_title, sizeof(w.title) - 1);
        w.title[sizeof(w.title) - 1] = '\0';
        invalidate_window_decoration_cache(w);
        mark_titlebar_dirty(w);
    }
}

void wm_commit_windows(Registry *registry)
{
    (void)registry;
    for (int i = 0; i < g_window_count; i++) {
        Window &w = g_windows[i];
        if (w.entry) {
            if (w.entry->request_close) {
                w.entry->request_close = false;
                close_window(i, false); // the app asked for this close; it terminates itself
                i--;
                continue;
            }
            if (w.entry->request_restore) {
                w.entry->request_restore = false;
                restore_window(i, true);
            } else if (w.entry->request_maximize) {
                w.entry->request_maximize = false;
                maximize_window(i);
            } else if (w.entry->request_minimize) {
                w.entry->request_minimize = false;
                minimize_window(i);
            }

            WindowEntrySnapshot entry_snapshot = {};
            if (!sample_stable_window_entry(*w.entry, &entry_snapshot)) {
                if (w.unstable_sample_count < 3) {
                    w.unstable_sample_count++;
                    w.needs_full_redraw = true;
                    continue;
                }
                // The client updates its entry faster than we can sample
                // it; accept the freshest read best-effort instead of
                // re-damaging the whole window every frame forever.
                entry_snapshot = read_window_entry_snapshot(*w.entry);
                w.unstable_sample_count = 0;
            } else {
                w.unstable_sample_count = 0;
            }
            int bw = entry_snapshot.buffer_w > 0 ? entry_snapshot.buffer_w : entry_snapshot.w;
            int bh = entry_snapshot.buffer_h > 0 ? entry_snapshot.buffer_h : entry_snapshot.h;
            if (bw > 8192)
                bw = 8192;
            if (bh > 8192)
                bh = 8192;

            if (gui_shm_id_is_valid(entry_snapshot.shm_id)) {
                if (entry_snapshot.shm_id != w.shm_id) {
                    bool is_memfd = (entry_snapshot.shm_id & 0x40000000) != 0;
                    bool map_ok = false;
                    uint64_t mapped = 0;
                    uint64_t req_size = (uint64_t)bw * bh * 4;

                    if (is_memfd) {
                        int fd = entry_snapshot.shm_id & ~0x40000000;
                        uint64_t file_size = syscall1(SYS_FSIZE, (uint64_t)fd);
                        if (file_size != (uint64_t)-1 && req_size <= file_size) {
                            void *mapped_ptr =
                                mmap(NULL, (size_t)req_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                            if (mapped_ptr != MAP_FAILED) {
                                mapped = reinterpret_cast<uint64_t>(mapped_ptr);
                                map_ok = true;
                            }
                        }
                    } else {
                        uint32_t shm_owner = (uint32_t)syscall1(SYS_SHM_GET_OWNER, entry_snapshot.shm_id);
                        if (shm_owner == entry_snapshot.owner_pid || shm_owner == 0) {
                            uint64_t shm_size = syscall1(SYS_SHM_INFO, (uint64_t)entry_snapshot.shm_id);
                            if (shm_size != (uint64_t)-1 && req_size <= shm_size) {
                                uint64_t legacy_ptr = syscall1(SYS_SHM_MAP, entry_snapshot.shm_id);
                                if (legacy_ptr != 0 && legacy_ptr != static_cast<uint64_t>(-1)) {
                                    mapped = legacy_ptr;
                                    map_ok = true;
                                }
                            }
                        }
                    }

                    if (map_ok) {
                        int old_shm_id = w.shm_id;
                        void *old_buffer = w.buffer;
                        int old_buffer_w = w.buffer_w;
                        int old_buffer_h = w.buffer_h;

                        w.shm_id = entry_snapshot.shm_id;
                        w.buffer = reinterpret_cast<uint32_t *>(static_cast<uintptr_t>(mapped));
                        if (gui_shm_id_is_valid(old_shm_id) && old_shm_id != entry_snapshot.shm_id) {
                            bool old_is_memfd = (old_shm_id & 0x40000000) != 0;
                            if (old_is_memfd) {
                                int old_fd = old_shm_id & ~0x40000000;
                                size_t old_size = (size_t)old_buffer_w * old_buffer_h * 4;
                                munmap(old_buffer, old_size);
                                close(old_fd);
                            } else {
                                syscall1(SYS_SHM_UNMAP, old_shm_id);
                            }
                        }
                        w.entry->buffer_ack_generation = entry_snapshot.buffer_generation;
                        w.buffer_generation_acked = entry_snapshot.buffer_generation;
                        w.buffer_generation_seen = entry_snapshot.buffer_generation;
                        smp_wmb();
                        w.needs_full_redraw = true;
                        invalidate_window_decoration_cache(w);
                        mark_window_frame_damage(w);
                        invalidate_window_visibility_cache();
                    }
                }

                if (w.buffer_w != bw || w.buffer_h != bh) {
                    bool is_memfd = (w.shm_id & 0x40000000) != 0;
                    bool size_change_valid = false;

                    if (!is_memfd) {
                        // Legacy System V Shared Memory Path
                        uint64_t actual_shm_size = syscall1(SYS_SHM_INFO, static_cast<uint64_t>(w.shm_id));
                        uint64_t required_bytes =
                            static_cast<uint64_t>(bw) * static_cast<uint64_t>(bh) * sizeof(uint32_t);
                        if (actual_shm_size != static_cast<uint64_t>(-1) && required_bytes <= actual_shm_size) {
                            size_change_valid = true;
                        } else {
                            LOG_WARN("wm", "Sanitizer blocked out-of-bounds SystemV size adjustment.");
                        }
                    } else {
                        // Modern memfd Backed Graphics Pipeline
                        uint64_t req_size = (uint64_t)bw * bh * 4;
                        int fd = w.shm_id & ~0x40000000;

                        // Safely establish an expanded virtual memory map for the existing file handle
                        uint64_t file_size = syscall1(SYS_FSIZE, (uint64_t)fd);
                        void *mapped_ptr = nullptr;
                        if (file_size != (uint64_t)-1 && req_size <= file_size) {
                            mapped_ptr = mmap(NULL, (size_t)req_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                        }
                        if (mapped_ptr != MAP_FAILED && mapped_ptr != nullptr) {
                            if (w.buffer) {
                                size_t old_size = (size_t)w.buffer_w * w.buffer_h * 4;
                                munmap(w.buffer, old_size);
                            }
                            w.buffer = reinterpret_cast<uint32_t *>(mapped_ptr);
                            size_change_valid = true;
                        } else {
                            LOG_WARN("wm", "Sanitizer blocked invalid memfd mmap dimension growth.");
                        }
                    }

                    if (size_change_valid) {
                        Window old = w;
                        w.buffer_w = bw;
                        w.buffer_h = bh;
                        mark_window_transition_damage(old, w);
                        w.needs_full_redraw = true;
                        invalidate_window_decoration_cache(w);
                        invalidate_window_visibility_cache();
                    } else {
                        // Force-clamp geometry parameters back to the current safe buffer metrics
                        bw = w.buffer_w;
                        bh = w.buffer_h;
                    }
                }
            }

            sync_window_runtime_metadata(w, entry_snapshot);

            w.min_w = entry_snapshot.min_w > 0 ? entry_snapshot.min_w : 0;
            w.min_h = entry_snapshot.min_h > 0 ? entry_snapshot.min_h : 0;

            if (i < WM_FIRST_USER_WINDOW) {
                int nx = entry_snapshot.x, ny = entry_snapshot.y;
                int nw = w.resize_configure_pending ? w.w : entry_snapshot.w;
                int nh = w.resize_configure_pending ? w.h : entry_snapshot.h;

                if (nw > static_cast<int>(g_screen.width) * 2)
                    nw = static_cast<int>(g_screen.width) * 2;
                if (nh > static_cast<int>(g_screen.height) * 2)
                    nh = static_cast<int>(g_screen.height) * 2;

                if (!w.resize_configure_pending && nw > 0 && nh > 0 &&
                    (w.x != nx || w.y != ny || w.w != nw || w.h != nh)) {
                    Window old = w;
                    w.x = nx;
                    w.y = ny;
                    w.w = nw;
                    w.h = nh;
                    w.needs_full_redraw = (old.w != nw) || (old.h != nh);
                    if (clamp_window_scroll(w) && w.entry) {
                        w.entry->scroll_x = w.scroll_x;
                        w.entry->scroll_y = w.scroll_y;
                        smp_wmb();
                    }
                    mark_window_transition_damage(old, w);
                    invalidate_window_visibility_cache();
                }
            } else if (!w.resize_configure_pending && w.entry &&
                       (entry_snapshot.x != w.x || entry_snapshot.y != w.y || entry_snapshot.w != w.w ||
                        entry_snapshot.h != w.h)) {
                // While a resize configure is outstanding the entry
                // deliberately carries the target the client redraws
                // toward; writing the visible bounds back would clobber
                // it. Outside a resize the WM is the geometry authority.
                w.entry->x = w.x;
                w.entry->y = w.y;
                w.entry->w = w.w;
                w.entry->h = w.h;
                smp_wmb();
            }

            if (entry_snapshot.owner_pid)
                w.owner_pid = entry_snapshot.owner_pid;
            w.buffer_generation_seen = entry_snapshot.buffer_generation;
            w.buffer_generation_acked = entry_snapshot.buffer_ack_generation;

            // A matching ack already cleared resize_configure_pending in
            // sync_window_runtime_metadata above (same scan, same entry
            // snapshot); what remains here is retransmitting a configure
            // the client has not answered yet.
            if (w.resize_configure_pending && w.owner_pid) {
                uint64_t now = get_ticks();
                uint64_t retry_interval = WM_RESIZE_CONFIGURE_RETRY_TICKS << 4;
                if (w.last_configure_ticks == 0 || now - w.last_configure_ticks >= retry_interval) {
                    resend_window_resize_configure(w);
                }
            }
        }
        if (w.damage_ptr) {
            if (damage_take_dropped_updates(w.damage_ptr) != 0)
                w.needs_full_redraw = true;

            Rect d = {};
            while (damage_pop_rect(w.damage_ptr, &d)) {
                if (!w.first_damage_received) {
                    w.first_damage_received = true;
                    w.needs_full_redraw = true;
                }
                if (is_window_visible(w)) {
                    int64_t dx64 = (int64_t)w.x + d.x - w.scroll_x;
                    int64_t dy64 = (int64_t)w.y + d.y - w.scroll_y;
                    DirtyRect damaged = {clamp_i64_to_int(dx64), clamp_i64_to_int(dy64), d.w, d.h};
                    DirtyRect client = window_visible_client_bounds(w);
                    DirtyRect visible = {};
                    if (rect_intersection(damaged, client, &visible)) {
                        enqueue_damage_rect(visible.x, visible.y, visible.w, visible.h);
                    }
                }
                w.active = true;
                if (w.entry)
                    w.entry->active = true;
            }

            if (!w.first_damage_received && (get_ticks() - w.last_commit_ticks > 90)) {
                w.first_damage_received = true;
                w.needs_full_redraw = true;
            }
        }
    }
}

void wm_apply_focus_requests()
{
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i].entry && g_windows[i].entry->request_focus) {
            g_windows[i].entry->request_focus = false;
            if (g_windows[i].entry->state == WIN_MINIMIZED)
                restore_window(i, false);
            focus_window(i, true);
            continue;
        }
        if (is_window_visible(g_windows[i]) && g_windows[i].needs_full_redraw) {
            mark_window_frame_damage(g_windows[i]);
            g_windows[i].needs_full_redraw = false;
        }
    }
}
