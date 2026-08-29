#pragma once

#include "wm_metrics.h"

extern Window g_windows[MAX_WINDOWS];
extern int g_window_count;
extern int g_add_fail_logs;

extern DirtyRect g_window_outer_cache[MAX_WINDOWS];
extern DirtyRect g_window_client_cache[MAX_WINDOWS];
extern bool g_window_visible_cache[MAX_WINDOWS];
extern DirtyRect g_window_visible_regions[MAX_WINDOWS][MAX_VISIBLE_REGIONS];
extern int g_window_visible_region_count[MAX_WINDOWS];
extern bool g_window_visible_region_overflow[MAX_WINDOWS];

// WM-side snapshot of a shared WindowEntry, sampled across load fences so a
// concurrently updating client can never tear the values the WM acts on.
struct WindowEntrySnapshot
{
    int shm_id;
    int x, y, w, h;
    uint32_t position_serial;
    int buffer_w, buffer_h;
    int content_w, content_h;
    int min_w, min_h;
    int scroll_x, scroll_y;
    uint32_t flags;
    uint32_t state;
    uint32_t owner_pid;
    uint32_t resize_serial;
    uint32_t buffer_resize_serial;
    uint32_t buffer_generation;
    uint32_t buffer_ack_generation;
    bool active;
    bool ready;
    bool request_close;
    bool request_focus;
    bool request_minimize;
    bool request_maximize;
    bool request_restore;
    char title[64];
};

// Adoption and commit pipeline.
void wm_adopt_windows(Registry *registry);
void wm_reap_dead_owners();
void wm_commit_windows(Registry *registry);
void wm_apply_focus_requests();

// Window cache and visibility.
void invalidate_window_visibility_cache();
void refresh_window_cache();
void refresh_window_visible_regions();

// Window geometry.
DirtyRect window_client_bounds(const Window &w);
DirtyRect window_outer_bounds(const Window &w);
DirtyRect window_occlusion_bounds(const Window &w);
DirtyRect window_opaque_bounds(const Window &w);
void get_window_opaque_cover_rects(const Window &w, DirtyRect *out_rects, int *out_count);
bool is_window_visible(const Window &w);
bool is_user_window(const Window &w);
int find_top_visible_user_window();
int find_registry_focused_user_window(const Registry *registry);
int find_window_by_shm(int shm_id);
int find_window_by_entry(const WindowEntry *entry);
int find_top_opaque_covering_window(const DirtyRect &r);
bool rect_intersects_window_chrome(const Window &w, const DirtyRect &r);

// Focus and z-order.
bool focus_window_owner(const Window *w);
void publish_focus(Registry *registry, const Window *w);
void clear_window_focus(Registry *registry);
int focus_window(int index, bool raise);
int bring_window_to_front(int index);
int send_window_to_back(int index);
void clear_hover_feedback_state();

// Lifecycle and state transitions.
void close_window(int index, bool kill_owner = true);
void minimize_window(int index);
void maximize_window(int index);
void toggle_maximize_window(int index);
void restore_window(int index, bool raise);
void set_window_bounds(Window &w, int x, int y, int width, int height);
void apply_window_bounds_now(Window &w, int x, int y, int width, int height, bool publish);
void apply_pending_window_bounds();
void apply_window_move_snap(const Window &w, int *x, int *y, int width, int height);
void reset_window_snap_state();
bool add_win_internal(int shm_id, int x, int y, int w, int h, const char *title, Damage *d_ptr, WindowEntry *entry,
                      bool transparent);

// Synchronous resize protocol.
bool post_window_resize_configure(Window &w);
void resend_window_resize_configure(Window &w);
void apply_window_resize_flip(Window &w);
void wm_resize_snapshot_capture(Window &w);
void wm_resize_snapshot_release(Window &w);
void wm_commit_snapshot_capture(Window &w);
void wm_commit_snapshot_release(Window &w);

// Content scrolling.
bool clamp_window_scroll(Window &w);
bool scroll_window_content(Window &w, int delta_x, int delta_y);
void publish_window_scroll(const Window &w);

// Damage marking.
void mark_window_frame_damage(const Window &w);
void mark_window_chrome_damage(const Window &w);
void mark_window_decoration_damage(const Window &w);
void mark_exposed_transition_damage(const DirtyRect &old_outer, const DirtyRect &new_outer);
void mark_window_transition_damage(const Window &old_w, const Window &new_w);
void post_focus_change_events(uint32_t prev_pid, uint32_t next_pid);

static inline int window_effective_w(const Window &w)
{
    return w.w;
}

static inline int window_effective_h(const Window &w)
{
    return w.h;
}

static inline DirtyRect window_visible_client_bounds(const Window &w)
{
    int eff_w = window_effective_w(w);
    int eff_h = window_effective_h(w);
    if (w.transparent)
        return {w.x, w.y, eff_w, eff_h};

    int border = wm_frame_border();
    int left = w.x + border;
    int top = w.y;
    int right = w.x + eff_w - border;
    int bottom = w.y + eff_h - border;
    int width = right - left;
    int height = bottom - top;
    if (width <= 0 || height <= 0)
        return {w.x, w.y, eff_w, eff_h};
    return {left, top, width, height};
}

static inline bool point_hits_window_visible_pixel(const Window &w, int px, int py, uint8_t min_alpha = 8)
{
    if (!point_in_rect({w.x, w.y, w.w, w.h}, px, py))
        return false;
    if (!w.transparent)
        return true;
    if (!w.buffer || w.buffer_w <= 0 || w.buffer_h <= 0)
        return false;

    int local_x = px - w.x + w.scroll_x;
    int local_y = py - w.y + w.scroll_y;
    if (local_x < 0 || local_y < 0 || local_x >= w.buffer_w || local_y >= w.buffer_h)
        return false;

    uint32_t pixel = w.buffer[(size_t)local_y * (size_t)w.buffer_w + (size_t)local_x];
    return ((pixel >> 24) & 0xFFu) >= min_alpha;
}
