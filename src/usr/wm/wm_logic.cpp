#include "../libc/config_utils.h"
#include "../libc/log.h"
#include "wm_core.h"

IndexState g_index = {};
ControlCenterState g_control_center = {false, CONTROL_ITEM_NONE, 75, true, true, true, false, true, 180, false};
NotificationCenterState g_notifications = {};

void wm_push_notification(const char* title, const char* message) {
    int index = g_notifications.head;
    Notification& notif = g_notifications.history[index];

    strncpy(notif.title, title, sizeof(notif.title) - 1);
    notif.title[sizeof(notif.title) - 1] = '\0';

    strncpy(notif.message, message, sizeof(notif.message) - 1);
    notif.message[sizeof(notif.message) - 1] = '\0';

    notif.timestamp_ticks = get_ticks();
    notif.read = false;
    notif.active_toast = true;

    g_notifications.head = (g_notifications.head + 1) % MAX_NOTIFICATIONS;
    if (g_notifications.count < MAX_NOTIFICATIONS) {
        g_notifications.count++;
    }

    int toast_w = gui_scaled_metric(320);
    int toast_h = gui_scaled_metric(76);
    int margin = gui_space_2();
    int toast_x = g_screen.width - toast_w - margin;
    int toast_y = wm_menubar_h() + margin;

    enqueue_damage_rect(toast_x - 16, toast_y - 16, toast_w + 32, toast_h + 32);
}

static int resolve_context_menu_target_index();
static bool context_menu_targets_window_entry(const WindowEntry *entry);
static bool ensure_context_menu_target_valid();
static void launch_or_focus_app(Registry *registry, const char *title, const char *path);

static void copy_dirty_rects_to_policy(wm::DirtyRect *dst, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = {g_dirty_rects[i].x, g_dirty_rects[i].y, g_dirty_rects[i].w, g_dirty_rects[i].h};
}

static void copy_dirty_rects_from_policy(const wm::DirtyRect *src, int count)
{
    for (int i = 0; i < count; i++)
        g_dirty_rects[i] = {src[i].x, src[i].y, src[i].w, src[i].h};
}

void enqueue_damage_rect(int x, int y, int w, int h)
{
    DirtyRect incoming = {x, y, w, h};
    if (!clip_dirty_rect_to_screen(incoming))
        return;

    // Full-screen dirty collapses everything to a single rect
    if (incoming.x == 0 && incoming.y == 0 && incoming.w == (int)g_screen.width &&
        incoming.h == (int)g_screen.height) {
        g_dirty_rects[0] = incoming;
        g_dirty_count = 1;
        invalidate_dirty_frame();
        return;
    }

    // Fast path: append if there is room and the incoming rect does not overlap any existing rect
    if (g_dirty_count < MAX_DIRTY_RECTS) {
        bool overlaps = false;
        for (int i = 0; i < g_dirty_count; i++) {
            if (dirty_rects_intersect(g_dirty_rects[i], incoming)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            g_dirty_rects[g_dirty_count++] = incoming;
            invalidate_dirty_frame();
            return;
        }
    }

    // Full policy merge path (deduplication, coalescing, and collapse heuristics)
    wm::DirtyRect policy_rects[MAX_DIRTY_RECTS];
    int policy_count = clamp_dirty_rect_count(g_dirty_count);
    copy_dirty_rects_to_policy(policy_rects, policy_count);

    wm::DirtyRect incoming_policy = {incoming.x, incoming.y, incoming.w, incoming.h};
    wm::enqueue_damage_rect(policy_rects, &policy_count, MAX_DIRTY_RECTS, (int)g_screen.width, (int)g_screen.height,
                            incoming_policy);

    g_dirty_count = clamp_dirty_rect_count(policy_count);
    copy_dirty_rects_from_policy(policy_rects, g_dirty_count);
    invalidate_dirty_frame();
}

void invalidate_dirty_frame()
{
    g_dirty_frame_ready = false;
}
void invalidate_window_visibility_cache()
{
    g_window_visibility_cache_dirty = true;
    g_dirty_frame_ready = false;
}

static bool dirty_rect_less(const DirtyRect &a, const DirtyRect &b)
{
    if (a.y != b.y)
        return a.y < b.y;
    return a.x < b.x;
}

static void swap_dirty_rect(DirtyRect *a, DirtyRect *b)
{
    DirtyRect t = *a;
    *a = *b;
    *b = t;
}

static int partition_dirty_rects(DirtyRect *arr, int low, int high)
{
    DirtyRect pivot = arr[high];
    int i = low - 1;
    for (int j = low; j < high; j++) {
        if (dirty_rect_less(arr[j], pivot)) {
            i++;
            swap_dirty_rect(&arr[i], &arr[j]);
        }
    }
    swap_dirty_rect(&arr[i + 1], &arr[high]);
    return i + 1;
}

static void quicksort_dirty_rects(DirtyRect *arr, int low, int high)
{
    while (low < high) {
        int pi = partition_dirty_rects(arr, low, high);
        if (pi - low < high - pi) {
            quicksort_dirty_rects(arr, low, pi - 1);
            low = pi + 1;
        } else {
            quicksort_dirty_rects(arr, pi + 1, high);
            high = pi - 1;
        }
    }
}

static void sort_dirty_rects()
{
    if (g_dirty_count > 1)
        quicksort_dirty_rects(g_dirty_rects, 0, g_dirty_count - 1);
}

void normalize_dirty_rects(bool interactive)
{
    if (g_dirty_count <= 0) {
        invalidate_dirty_frame();
        return;
    }

    wm::DirtyRect policy_rects[MAX_DIRTY_RECTS] = {};
    int policy_count = clamp_dirty_rect_count(g_dirty_count);
    copy_dirty_rects_to_policy(policy_rects, policy_count);

    wm::normalize_dirty_rects(policy_rects, &policy_count, (int)g_screen.width, (int)g_screen.height, interactive);

    g_dirty_count = clamp_dirty_rect_count(policy_count);
    copy_dirty_rects_from_policy(policy_rects, g_dirty_count);
    if (g_dirty_count > 1)
        sort_dirty_rects();
    invalidate_dirty_frame();
}

bool clip_dirty_rect_to_screen(DirtyRect &rect)
{
    int64_t x = rect.x, y = rect.y, w = rect.w, h = rect.h;
    if (w <= 0 || h <= 0)
        return false;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    int64_t max_w = (int64_t)g_screen.width - x, max_h = (int64_t)g_screen.height - y;
    if (max_w <= 0 || max_h <= 0)
        return false;
    if (w > max_w)
        w = max_w;
    if (h > max_h)
        h = max_h;
    if (w <= 0 || h <= 0)
        return false;
    rect.x = clamp_i64_to_int(x);
    rect.y = clamp_i64_to_int(y);
    rect.w = clamp_i64_to_int(w);
    rect.h = clamp_i64_to_int(h);
    return true;
}

static void enqueue_damage_rect_expanded(const DirtyRect &rect, int pad)
{
    DirtyRect expanded = rect_expand(rect, pad);
    enqueue_damage_rect(expanded.x, expanded.y, expanded.w, expanded.h);
}

static void publish_window_scroll(const Window &w)
{
    if (!w.entry)
        return;
    if (w.entry->scroll_x == w.scroll_x && w.entry->scroll_y == w.scroll_y)
        return;
    w.entry->scroll_x = w.scroll_x;
    w.entry->scroll_y = w.scroll_y;
    asm volatile("sfence" ::: "memory");
}

static uint32_t next_configure_serial(Window &w)
{
    uint32_t serial = (w.configure_serial == 0xFFFFFFFFu) ? 1u : w.configure_serial + 1u;
    w.configure_serial = serial;
    return serial;
}

bool post_window_resize_configure(Window &w)
{
    if (!w.entry || !(w.entry->flags & WIN_FLAG_RESIZABLE) || !w.owner_pid || w.target_w <= 0 || w.target_h <= 0)
        return false;

    w.pending_configure_serial = next_configure_serial(w);
    w.entry_resize_serial = w.pending_configure_serial;
    w.resize_configure_pending = true;
    w.last_configure_ticks = get_ticks();

    // Publish the requested client size and serial to the client-visible entry.
    // The visible compositor frame is allowed to move/resize immediately; this
    // serial only proves when the client has committed matching content.
    w.entry->resize_serial = w.pending_configure_serial;
    asm volatile("sfence" ::: "memory");

    Event resize_ev = {};
    resize_ev.type = EVT_WINDOW_RESIZE;
    resize_ev.resize.width = w.target_w;
    resize_ev.resize.height = w.target_h;
    resize_ev.resize.serial = w.pending_configure_serial;
    syscall2(SYS_POST_EVENT, w.owner_pid, (uint64_t)&resize_ev);
    return true;
}

static int wm_snap_threshold()
{
    int threshold = gui_scaled_metric(WM_SNAP_THRESHOLD_BASE);
    return threshold < 6 ? 6 : threshold;
}

static int wm_snap_escape()
{
    int escape = gui_scaled_metric(WM_SNAP_ESCAPE_BASE);
    return escape < wm_snap_threshold() ? wm_snap_threshold() : escape;
}

static void apply_window_move_snap(const Window &w, int *x, int *y, int width, int height)
{
    if (!x || !y || !w.entry || w.entry->state == WIN_MAXIMIZED || width <= 0 || height <= 0)
        return;

    int threshold = wm_snap_threshold();
    int min_y = wm_menubar_h() + wm_title_bar_h() + wm_desktop_margin();
    int left = wm_desktop_margin();
    int right = (int)g_screen.width - wm_desktop_margin();
    int bottom = (int)g_screen.height - wm_dock_reserved_h();

    int nx = *x;
    int ny = *y;
    int edges = RESIZE_NONE;
    if (wm_fabsf((float)(nx - left)) <= (float)threshold) {
        nx = left;
        edges |= RESIZE_LEFT;
    }
    if (wm_fabsf((float)((nx + width) - right)) <= (float)threshold) {
        nx = right - width;
        edges |= RESIZE_RIGHT;
    }
    if (wm_fabsf((float)(ny - min_y)) <= (float)threshold) {
        ny = min_y;
        edges |= RESIZE_TOP;
    }
    if (wm_fabsf((float)((ny + height) - bottom)) <= (float)threshold) {
        ny = bottom - height;
        edges |= RESIZE_BOTTOM;
    }

    if (edges != RESIZE_NONE) {
        g_input.snap_edges = edges;
        g_input.snap_preview = {nx, ny, width, height};
    } else if (g_input.snap_edges != RESIZE_NONE) {
        int escape = wm_snap_escape();
        if (wm_fabsf((float)(*x - g_input.snap_preview.x)) > (float)escape ||
            wm_fabsf((float)(*y - g_input.snap_preview.y)) > (float)escape) {
            g_input.snap_edges = RESIZE_NONE;
            g_input.snap_preview = {};
        }
    }

    *x = nx;
    *y = ny;
}

static void reset_window_snap_state()
{
    if (g_input.snap_edges == RESIZE_NONE)
        return;
    DirtyRect preview = g_input.snap_preview;
    g_input.snap_edges = RESIZE_NONE;
    g_input.snap_preview = {};
    if (preview.w > 0 && preview.h > 0)
        enqueue_damage_rect(preview.x, preview.y, preview.w, preview.h);
}

DirtyRect window_client_bounds(const Window &w)
{
    return {w.x, w.y, w.w, w.h};
}
DirtyRect window_outer_bounds(const Window &w)
{
    if (w.transparent)
        return {w.x, w.y, w.w, w.h};
    int t_h = wm_title_bar_h();
    return {w.x, w.y - t_h, w.w + wm_frame_shadow_offset_x(), w.h + t_h + wm_frame_shadow_offset_y()};
}
static inline int window_safe_side_inset()
{
    int inset = gui_scaled_metric(FRAME_OCCLUSION_INSET) + wm_frame_border() + wm_frame_shadow_offset_x();
    return inset < 2 ? 2 : inset;
}

static void get_window_opaque_cover_rects(const Window &w, DirtyRect *out_rects, int *out_count)
{
    if (!out_rects || !out_count)
        return;
    *out_count = 0;
    if (w.transparent) {
        out_rects[0] = window_client_bounds(w);
        *out_count = 1;
        return;
    }

    int side_inset = window_safe_side_inset();
    int title_h = wm_title_bar_h();
    int radius = gui_scaled_metric(12) - wm_frame_border();
    if (radius < side_inset)
        radius = side_inset;

    DirtyRect rects[3];
    int count = 0;

    int shadow_pad = wm_frame_shadow_offset_y();
    DirtyRect main = {w.x + side_inset, w.y - title_h + radius, w.w - side_inset * 2,
                      w.h + title_h - radius - radius - shadow_pad};
    if (main.w > 0 && main.h > 0)
        rects[count++] = main;

    DirtyRect top_band = {w.x + radius, w.y - title_h + side_inset, w.w - radius * 2, radius - side_inset};
    if (top_band.w > 0 && top_band.h > 0)
        rects[count++] = top_band;

    DirtyRect bottom_band = {w.x + radius, w.y + w.h - radius, w.w - radius * 2, radius - side_inset};
    if (bottom_band.w > 0 && bottom_band.h > 0)
        rects[count++] = bottom_band;

    if (count == 0) {
        DirtyRect fallback = {w.x + side_inset, w.y, w.w - side_inset * 2, w.h};
        rects[count++] = (fallback.w > 0 && fallback.h > 0) ? fallback : window_client_bounds(w);
    }

    *out_count = count;
    for (int i = 0; i < count; i++)
        out_rects[i] = rects[i];
}

DirtyRect window_opaque_bounds(const Window &w)
{
    if (w.transparent)
        return window_client_bounds(w);
    int side_inset = window_safe_side_inset();
    int title_h = wm_title_bar_h();
    int radius = gui_scaled_metric(12) - wm_frame_border();
    if (radius < side_inset)
        radius = side_inset;

    DirtyRect main = {w.x + side_inset, w.y - title_h + radius, w.w - side_inset * 2, w.h + title_h - radius - radius};
    if (main.w > 0 && main.h > 0)
        return main;

    DirtyRect fallback = {w.x + side_inset, w.y, w.w - side_inset * 2, w.h};
    return (fallback.w > 0 && fallback.h > 0) ? fallback : window_client_bounds(w);
}

DirtyRect window_occlusion_bounds(const Window &w)
{
    return window_opaque_bounds(w);
}

bool is_visible_state(uint32_t state)
{
    return state != WIN_MINIMIZED && state != WIN_HIDDEN;
}
bool is_window_visible(const Window &w)
{
    return w.active && (!w.entry || is_visible_state(w.entry->state));
}
bool is_user_window(const Window &w)
{
    return w.entry && (w.entry->flags & WIN_FLAG_SYSTEM) == 0;
}

void refresh_window_cache()
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        g_window_outer_cache[i] = {0, 0, 0, 0};
        g_window_client_cache[i] = {0, 0, 0, 0};
        g_window_visible_cache[i] = false;
        g_window_visible_region_count[i] = 0;
    }
    for (int i = 0; i < g_window_count; i++) {
        g_window_visible_cache[i] = is_window_visible(g_windows[i]);
        g_window_outer_cache[i] = window_outer_bounds(g_windows[i]);
        g_window_client_cache[i] = window_visible_client_bounds(g_windows[i]);
    }
}

static bool subtract_region_list(DirtyRect *regions, int *region_count, const DirtyRect &cut)
{
    for (int i = 0; i < *region_count;) {
        DirtyRect current = regions[i], overlap = {};
        if (!rect_intersection(current, cut, &overlap)) {
            i++;
            continue;
        }
        regions[i] = regions[*region_count - 1];
        (*region_count)--;

        DirtyRect pieces[4];
        int piece_count = 0;
        if (current.y < overlap.y)
            pieces[piece_count++] = {current.x, current.y, current.w, overlap.y - current.y};
        int o_b = overlap.y + overlap.h, c_b = current.y + current.h;
        if (o_b < c_b)
            pieces[piece_count++] = {current.x, o_b, current.w, c_b - o_b};
        if (current.x < overlap.x)
            pieces[piece_count++] = {current.x, overlap.y, overlap.x - current.x, overlap.h};
        int o_r = overlap.x + overlap.w, c_r = current.x + current.w;
        if (o_r < c_r)
            pieces[piece_count++] = {o_r, overlap.y, c_r - o_r, overlap.h};

        for (int p = 0; p < piece_count; p++) {
            if (pieces[p].w <= 0 || pieces[p].h <= 0)
                continue;
            if (*region_count >= MAX_VISIBLE_REGIONS)
                return false;
            regions[(*region_count)++] = pieces[p];
        }
    }
    return true;
}

static void merge_adjacent_regions(DirtyRect *regions, int *count)
{
    if (!regions || !count || *count <= 1)
        return;

    quicksort_dirty_rects(regions, 0, *count - 1);

    int write = 0;
    for (int read = 1; read < *count; read++) {
        DirtyRect &a = regions[write];
        DirtyRect &b = regions[read];
        bool merged = false;

        if (a.x == b.x && a.w == b.w && a.y + a.h == b.y) {
            a.h += b.h;
            merged = true;
        } else if (a.y == b.y && a.h == b.h && a.x + a.w == b.x) {
            a.w += b.w;
            merged = true;
        }

        if (!merged) {
            write++;
            regions[write] = b;
        }
    }
    *count = write + 1;
}

void refresh_window_visible_regions()
{
    for (int i = 0; i < (g_window_count > MAX_WINDOWS ? MAX_WINDOWS : g_window_count); i++) {
        g_window_visible_region_count[i] = 0;
        g_window_visible_region_overflow[i] = false;
        if (!g_window_visible_cache[i] || !g_windows[i].buffer || g_windows[i].transparent)
            continue;

        DirtyRect regions[MAX_VISIBLE_REGIONS];
        int region_count = 1;
        regions[0] = g_window_outer_cache[i];

        for (int above = i + 1; above < g_window_count; above++) {
            const Window &cover = g_windows[above];
            if (!g_window_visible_cache[above] || !cover.buffer || cover.transparent)
                continue;
            DirtyRect cover_rects[3];
            int cover_rect_count = 0;
            get_window_opaque_cover_rects(cover, cover_rects, &cover_rect_count);
            for (int c = 0; c < cover_rect_count; c++) {
                if (!subtract_region_list(regions, &region_count, cover_rects[c])) {
                    g_window_visible_region_overflow[i] = true;
                    region_count = 0;
                    break;
                }
                if (region_count == 0)
                    break;
            }
            if (g_window_visible_region_overflow[i] || region_count == 0)
                break;
        }

        if (region_count > 1)
            merge_adjacent_regions(regions, &region_count);

        g_window_visible_region_count[i] = region_count;
        for (int r = 0; r < region_count; r++)
            g_window_visible_regions[i][r] = regions[r];
    }
}

int find_top_opaque_covering_window(const DirtyRect &r)
{
    for (int i = g_window_count - 1; i >= 0; i--) {
        const Window &w = g_windows[i];
        if (!g_window_visible_cache[i] || w.transparent || !w.buffer)
            continue;
        DirtyRect cover_rects[3];
        int cover_rect_count = 0;
        get_window_opaque_cover_rects(w, cover_rects, &cover_rect_count);
        for (int c = 0; c < cover_rect_count; c++) {
            if (rect_contains(cover_rects[c], r))
                return i;
        }
    }
    return -1;
}

bool rect_intersects_window_chrome(const Window &w, const DirtyRect &r)
{
    if (w.transparent)
        return false;
    DirtyRect outer = window_outer_bounds(w);
    if (!rect_intersection(outer, r, nullptr))
        return false;

    DirtyRect client = window_visible_client_bounds(w);
    DirtyRect overlap = {};
    if (!rect_intersection(outer, client, &overlap))
        return true;

    if (outer.y < overlap.y && rect_intersection(r, {outer.x, outer.y, outer.w, overlap.y - outer.y}, nullptr))
        return true;
    int overlap_bottom = overlap.y + overlap.h;
    int outer_bottom = outer.y + outer.h;
    if (overlap_bottom < outer_bottom &&
        rect_intersection(r, {outer.x, overlap_bottom, outer.w, outer_bottom - overlap_bottom}, nullptr))
        return true;
    if (outer.x < overlap.x && rect_intersection(r, {outer.x, overlap.y, overlap.x - outer.x, overlap.h}, nullptr))
        return true;
    int overlap_right = overlap.x + overlap.w;
    int outer_right = outer.x + outer.w;
    if (overlap_right < outer_right &&
        rect_intersection(r, {overlap_right, overlap.y, outer_right - overlap_right, overlap.h}, nullptr))
        return true;

    return false;
}

int find_top_visible_user_window()
{
    for (int i = g_window_count - 1; i >= 2; i--)
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
    if (!registry || registry->focused_window < 2 || registry->focused_window >= MAX_WINDOWS)
        return -1;
    const WindowEntry *focused_entry = &registry->windows[registry->focused_window];
    for (int i = 2; i < g_window_count; i++) {
        if (g_windows[i].entry == focused_entry && is_window_visible(g_windows[i]) && is_user_window(g_windows[i]))
            return i;
    }
    return -1;
}

void focus_window_owner(const Window *w)
{
    syscall1(SYS_GUI_SET_FOCUS, w ? (w->entry && w->entry->owner_pid ? w->entry->owner_pid : w->owner_pid) : 0);
}

void publish_focus(Registry *registry, const Window *w)
{
    if (!registry)
        return;
    if (!w || !w->entry || !is_user_window(*w) || !is_window_visible(*w)) {
        registry->focused_window = -1;
        registry->focused_owner_pid = 0;
    } else {
        registry->focused_window = window_slot(registry, *w);
        registry->focused_owner_pid = w->owner_pid;
    }
    asm volatile("sfence" ::: "memory");
}

void clear_window_focus(Registry *registry)
{
    int previous = find_registry_focused_user_window(registry);
    focus_window_owner(nullptr);
    publish_focus(registry, nullptr);
    if (previous >= 2 && previous < g_window_count && is_window_visible(g_windows[previous]))
        mark_window_chrome_damage(g_windows[previous]);
}

static void clear_hover_feedback_state()
{
    if (g_input.hover_frame_index >= 2 && g_input.hover_frame_index < g_window_count)
        mark_window_chrome_damage(g_windows[g_input.hover_frame_index]);
    g_input.hover_frame_index = -1;
    g_input.hover_resize_edges = RESIZE_NONE;
    g_input.hover_button = -1;
}

int bring_window_to_front(int index)
{
    if (index < 2 || index >= g_window_count || index == g_window_count - 1)
        return index;
    Window temp = g_windows[index];
    for (int i = index; i < g_window_count - 1; i++)
        g_windows[i] = g_windows[i + 1];
    g_windows[g_window_count - 1] = temp;
    return g_window_count - 1;
}

int send_window_to_back(int index)
{
    if (index < 2 || index >= g_window_count || index == 2)
        return index;
    Window temp = g_windows[index];
    for (int i = index; i > 2; i--)
        g_windows[i] = g_windows[i - 1];
    g_windows[2] = temp;
    return 2;
}

static void mark_window_titlebar_damage(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    int title_h = w.y - outer.y;
    if (title_h < 0)
        title_h = 0;
    if (title_h > outer.h)
        title_h = outer.h;
    if (title_h > 0)
        enqueue_damage_rect(outer.x, outer.y, outer.w, title_h);
}

void mark_window_frame_damage(const Window &w)
{
    DirtyRect outer = window_outer_bounds(w);
    enqueue_damage_rect_expanded(outer, wm_window_damage_pad());
}

void mark_window_chrome_damage(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    DirtyRect client = window_visible_client_bounds(w);
    DirtyRect overlap = {};
    if (!rect_intersection(outer, client, &overlap)) {
        enqueue_damage_rect(outer.x, outer.y, outer.w, outer.h);
        return;
    }

    if (outer.y < overlap.y)
        enqueue_damage_rect(outer.x, outer.y, outer.w, overlap.y - outer.y);

    int overlap_bottom = overlap.y + overlap.h;
    int outer_bottom = outer.y + outer.h;
    if (overlap_bottom < outer_bottom)
        enqueue_damage_rect(outer.x, overlap_bottom, outer.w, outer_bottom - overlap_bottom);

    if (outer.x < overlap.x)
        enqueue_damage_rect(outer.x, overlap.y, overlap.x - outer.x, overlap.h);

    int overlap_right = overlap.x + overlap.w;
    int outer_right = outer.x + outer.w;
    if (overlap_right < outer_right)
        enqueue_damage_rect(overlap_right, overlap.y, outer_right - overlap_right, overlap.h);
}

static void mark_window_decoration_damage(const Window &w)
{
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    mark_window_titlebar_damage(w);
    mark_window_chrome_damage(w);

    int shadow_extent = gui_scaled_metric(8) + gui_scaled_metric(3) + gui_scaled_metric(2) + gui_scaled_metric(1);
    if (shadow_extent < CURSOR_MAX_SIZE)
        shadow_extent = CURSOR_MAX_SIZE;

    int side_strip = shadow_extent;
    if (side_strip > outer.w)
        side_strip = outer.w;
    if (side_strip > 0) {
        enqueue_damage_rect(outer.x, outer.y, side_strip, outer.h);
        int right_x = outer.x + outer.w - side_strip;
        enqueue_damage_rect(right_x, outer.y, side_strip, outer.h);
    }

    int bottom_strip = shadow_extent;
    if (bottom_strip > outer.h)
        bottom_strip = outer.h;
    if (bottom_strip > 0)
        enqueue_damage_rect(outer.x, outer.y + outer.h - bottom_strip, outer.w, bottom_strip);
}

static void mark_exposed_transition_damage(const DirtyRect &old_outer, const DirtyRect &new_outer)
{
    int pad = wm_window_damage_pad();
    DirtyRect old_padded = rect_expand(old_outer, pad);
    DirtyRect new_padded = rect_expand(new_outer, pad);
    wm::ExposedTransitionDamage dmg =
        wm::compute_exposed_transition_damage(to_policy_rect(old_padded), to_policy_rect(new_padded));
    for (int i = 0; i < dmg.count; i++)
        enqueue_damage_rect(dmg.rects[i].x, dmg.rects[i].y, dmg.rects[i].w, dmg.rects[i].h);
}

void mark_window_transition_damage(const Window &old_w, const Window &new_w)
{
    int pad = wm_window_damage_pad();
    DirtyRect o = rect_expand(window_outer_bounds(old_w), pad);
    DirtyRect n = rect_expand(window_outer_bounds(new_w), pad);
    enqueue_damage_rect(n.x, n.y, n.w, n.h);
    DirtyRect overlap = {};
    if (!rect_intersection(o, n, &overlap)) {
        enqueue_damage_rect(o.x, o.y, o.w, o.h);
        return;
    }
    if (rect_contains(overlap, o))
        return;
    if (o.y < overlap.y)
        enqueue_damage_rect(o.x, o.y, o.w, overlap.y - o.y);
    if (overlap.y + overlap.h < o.y + o.h)
        enqueue_damage_rect(o.x, overlap.y + overlap.h, o.w, o.y + o.h - (overlap.y + overlap.h));
    if (o.x < overlap.x)
        enqueue_damage_rect(o.x, overlap.y, overlap.x - o.x, overlap.h);
    if (overlap.x + overlap.w < o.x + o.w)
        enqueue_damage_rect(overlap.x + overlap.w, overlap.y, o.x + o.w - (overlap.x + overlap.w), overlap.h);
}

void mark_cursor_transition_damage(int old_x, int old_y, GuiCursorKind old_kind, int new_x, int new_y,
                                   GuiCursorKind new_kind)
{
    DirtyRect orct = {}, nrect = {};
    gui_get_cursor_bounds(old_kind, old_x, old_y, &orct.x, &orct.y, &orct.w, &orct.h);
    gui_get_cursor_bounds(new_kind, new_x, new_y, &nrect.x, &nrect.y, &nrect.w, &nrect.h);
    enqueue_damage_rect(orct.x - CURSOR_DAMAGE_PAD, orct.y - CURSOR_DAMAGE_PAD, orct.w + CURSOR_DAMAGE_PAD * 2,
                        orct.h + CURSOR_DAMAGE_PAD * 2);
    enqueue_damage_rect(nrect.x - CURSOR_DAMAGE_PAD, nrect.y - CURSOR_DAMAGE_PAD, nrect.w + CURSOR_DAMAGE_PAD * 2,
                        nrect.h + CURSOR_DAMAGE_PAD * 2);
}

static int window_max_scroll_x(const Window &w)
{
    int visible_w = w.w > 0 ? w.w : 0;
    int content_w = w.content_w > 0 ? w.content_w : visible_w;
    return content_w > visible_w ? content_w - visible_w : 0;
}

static int window_max_scroll_y(const Window &w)
{
    int visible_h = w.h > 0 ? w.h : 0;
    int content_h = w.content_h > 0 ? w.content_h : visible_h;
    return content_h > visible_h ? content_h - visible_h : 0;
}

bool clamp_window_scroll(Window &w)
{
    int old_x = w.scroll_x;
    int old_y = w.scroll_y;
    int max_x = window_max_scroll_x(w);
    int max_y = window_max_scroll_y(w);

    if (max_x <= 0)
        w.scroll_x = 0;
    else if (w.scroll_x < 0)
        w.scroll_x = 0;
    else if (w.scroll_x > max_x)
        w.scroll_x = max_x;

    if (max_y <= 0)
        w.scroll_y = 0;
    else if (w.scroll_y < 0)
        w.scroll_y = 0;
    else if (w.scroll_y > max_y)
        w.scroll_y = max_y;

    return old_x != w.scroll_x || old_y != w.scroll_y;
}

bool scroll_window_content(Window &w, int delta_x, int delta_y)
{
    if (!is_user_window(w) || !is_window_visible(w) || !w.buffer)
        return false;
    if (delta_x == 0 && delta_y == 0)
        return false;

    if (window_max_scroll_x(w) <= 0 && window_max_scroll_y(w) <= 0)
        return false;

    int old_x = w.scroll_x;
    int old_y = w.scroll_y;
    int64_t next_x = (int64_t)w.scroll_x + (int64_t)delta_x;
    int64_t next_y = (int64_t)w.scroll_y + (int64_t)delta_y;
    w.scroll_x = clamp_i64_to_int(next_x);
    w.scroll_y = clamp_i64_to_int(next_y);
    clamp_window_scroll(w);
    if (old_x == w.scroll_x && old_y == w.scroll_y)
        return false;

    publish_window_scroll(w);

    DirtyRect client = window_visible_client_bounds(w);
    enqueue_damage_rect(client.x, client.y, client.w, client.h);
    return true;
}

static bool compositor_state_allows_fast_move()
{
    if (g_dirty_count != 0)
        return false;
    if (g_window_visibility_cache_dirty)
        return false;
    if (g_context_menu.open || g_storage_prompt.visible)
        return false;
    if (g_input.hover_frame_index >= 0 || g_input.hover_resize_edges != RESIZE_NONE || g_input.hover_button >= 0)
        return false;
    return true;
}

static bool can_fast_move_window(int moving_index, const DirtyRect &old_outer, const DirtyRect &new_outer)
{
    if (moving_index < 2 || moving_index != g_window_count - 1)
        return false;
    if (!compositor_state_allows_fast_move())
        return false;
    DirtyRect move_union = rect_union(old_outer, new_outer);
    for (int i = 0; i < g_window_count; i++) {
        if (i == moving_index || !is_window_visible(g_windows[i]) || !g_windows[i].buffer)
            continue;
        if (rect_intersection(move_union,
                              g_windows[i].transparent ? window_client_bounds(g_windows[i])
                                                       : window_outer_bounds(g_windows[i]),
                              nullptr))
            return false;
    }
    return true;
}

void set_window_bounds(Window &w, int x, int y, int width, int height)
{
    if (!w.entry)
        return;
    int min_width = (w.min_w > 0) ? w.min_w : wm_default_min_w();
    int min_height = (w.min_h > 0) ? w.min_h : wm_default_min_h();
    if (width < min_width)
        width = min_width;
    if (height < min_height)
        height = min_height;
    int min_y = wm_menubar_h() + wm_title_bar_h() + wm_desktop_margin();
    int max_width = (int)g_screen.width - wm_desktop_margin() * 2;
    int max_height = (int)g_screen.height - wm_dock_reserved_h() - min_y;
    if (max_width < min_width)
        max_width = min_width;
    if (max_height < min_height)
        max_height = min_height;
    if (width > max_width)
        width = max_width;
    if (height > max_height)
        height = max_height;
    int max_x = (int)g_screen.width - width - wm_desktop_margin();
    int max_y = (int)g_screen.height - wm_dock_reserved_h() - height;
    if (max_x < wm_desktop_margin())
        max_x = wm_desktop_margin();
    if (max_y < min_y)
        max_y = min_y;
    x = x < wm_desktop_margin() ? wm_desktop_margin() : (x > max_x ? max_x : x);
    y = y < min_y ? min_y : (y > max_y ? max_y : y);
    w.target_x = x;
    w.target_y = y;
    w.target_w = width;
    w.target_h = height;

    bool size_changed = (w.w != width) || (w.h != height);
    bool moved = (w.x != x) || (w.y != y);

    if (moved || size_changed) {
        Window old = w;
        w.x = x;
        w.y = y;
        w.w = width;
        w.h = height;
        w.entry->x = x;
        w.entry->y = y;
        w.entry->w = width;
        w.entry->h = height;

        if (size_changed) {
            invalidate_window_decoration_cache(w);
            if (clamp_window_scroll(w))
                publish_window_scroll(w);
            post_window_resize_configure(w);
        }

        bool moved_fast = false;
        if (moved && !size_changed && !w.transparent && g_input.pointer_down && g_input.drag_edges == RESIZE_NONE &&
            g_input.drag_index >= 2 && g_input.drag_index < g_window_count &&
            g_windows[g_input.drag_index].entry == w.entry) {
            DirtyRect old_outer = window_outer_bounds(old);
            DirtyRect new_outer = window_outer_bounds(w);
            if (can_fast_move_window(g_input.drag_index, old_outer, new_outer))
                moved_fast = move_backbuffer_rect(old_outer, new_outer);
        }

        if (moved_fast) {
            mark_presentbuffer_slots_stale(rect_union(window_outer_bounds(old), window_outer_bounds(w)));
            mark_exposed_transition_damage(window_outer_bounds(old), window_outer_bounds(w));
            mark_window_decoration_damage(w);
        } else {
            mark_window_transition_damage(old, w);
        }
        invalidate_window_visibility_cache();
    }

    w.entry->active = true;
    if (context_menu_targets_window_entry(w.entry))
        close_context_menu();
    asm volatile("sfence" ::: "memory");
}

int focus_window(int index, bool raise)
{
    if (index < 0 || index >= g_window_count)
        return index;
    WindowEntry *target_entry = g_windows[index].entry;
    WindowEntry *prev_entry = nullptr;
    WindowEntry *hover_entry = nullptr;
    int prev_idx = find_registry_focused_user_window(gui_registry());
    if (prev_idx >= 2 && prev_idx < g_window_count)
        prev_entry = g_windows[prev_idx].entry;
    if (g_input.hover_frame_index >= 2 && g_input.hover_frame_index < g_window_count)
        hover_entry = g_windows[g_input.hover_frame_index].entry;

    bool target_was_focused = (prev_idx == index);
    if (!target_was_focused && prev_idx >= 2 && prev_idx < g_window_count && target_entry &&
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
    if (index >= 2 && is_window_visible(w)) {
        focus_window_owner(&w);
        publish_focus(gui_registry(), &w);
    } else {
        focus_window_owner(nullptr);
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
    if (index < 2 || index >= g_window_count)
        return;
    Window &w = g_windows[index];
    if (!w.entry)
        return;
    int rw = w.entry->restore_w > 0 ? w.entry->restore_w : w.w;
    int rh = w.entry->restore_h > 0 ? w.entry->restore_h : w.h;
    w.entry->state = WIN_NORMAL;
    w.active = true;
    set_window_bounds(w, w.entry->restore_x, w.entry->restore_y, rw, rh);
    if (context_menu_targets_window_entry(w.entry))
        close_context_menu();
    invalidate_window_visibility_cache();
    if (raise)
        focus_window(index, true);
}

void maximize_window(int index)
{
    if (index < 2 || index >= g_window_count || !g_windows[index].entry)
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
    if (context_menu_targets_window_entry(w.entry))
        close_context_menu();
    invalidate_window_visibility_cache();
    focus_window(index, true);
}

void toggle_maximize_window(int index)
{
    if (index >= 2 && index < g_window_count && g_windows[index].entry) {
        if (g_windows[index].entry->state == WIN_MAXIMIZED)
            restore_window(index, true);
        else
            maximize_window(index);
    }
}

void minimize_window(int index)
{
    if (index < 2 || index >= g_window_count || !g_windows[index].entry)
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
    if (context_menu_targets_window_entry(w.entry))
        close_context_menu();
    invalidate_window_visibility_cache();

    int focus_idx = find_top_visible_user_window();
    focus_window_owner(focus_idx >= 0 ? &g_windows[focus_idx] : nullptr);
    publish_focus(gui_registry(), focus_idx >= 0 ? &g_windows[focus_idx] : nullptr);
    if (focus_idx >= 2)
        mark_window_chrome_damage(g_windows[focus_idx]);
}

void close_window(int index)
{
    if (index < 2 || index >= g_window_count)
        return;
    Window doomed = g_windows[index];
    uint32_t owner = (doomed.entry && doomed.entry->owner_pid) ? doomed.entry->owner_pid : doomed.owner_pid;

    mark_window_frame_damage(doomed);
    if (context_menu_targets_window_entry(doomed.entry))
        close_context_menu();
    if (gui_shm_id_is_valid(doomed.shm_id))
        syscall1(SYS_SHM_UNMAP, (uint64_t)doomed.shm_id);
    gui_destroy_surface(&doomed.decoration_cache);
    gui_destroy_surface(&doomed.button_cache);
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

    invalidate_window_visibility_cache();
    int focus_idx = find_top_visible_user_window();
    focus_window_owner(focus_idx >= 0 ? &g_windows[focus_idx] : nullptr);
    publish_focus(gui_registry(), focus_idx >= 0 ? &g_windows[focus_idx] : nullptr);
    if (focus_idx >= 2)
        mark_window_chrome_damage(g_windows[focus_idx]);
    if (owner)
        syscall2(SYS_KILL, owner, SIGTERM);
}

int hit_test_resize(const Window &w, int px, int py)
{
    if (!is_user_window(w) || !is_window_visible(w) || w.transparent || !w.entry ||
        !(w.entry->flags & WIN_FLAG_RESIZABLE) || w.entry->state == WIN_MAXIMIZED)
        return RESIZE_NONE;
    int edges = RESIZE_NONE, grip = wm_resize_grip();
    int border = wm_frame_border();
    int64_t l = w.x - border, r = w.x + w.w + border, t = w.y - wm_title_bar_h() - border, b = w.y + w.h + border;
    if (px < l - grip || px >= r + grip || py < t - grip || py >= b + grip)
        return RESIZE_NONE;
    if (px < l + grip)
        edges |= RESIZE_LEFT;
    if (px >= r - grip)
        edges |= RESIZE_RIGHT;
    if (py < t + grip)
        edges |= RESIZE_TOP;
    if (py >= b - grip)
        edges |= RESIZE_BOTTOM;
    return edges;
}

static bool point_in_rounded_window_outer(const Window &w, int px, int py)
{
    DirtyRect outer = window_outer_bounds(w);
    if (!point_in_rect(outer, px, py))
        return false;
    if (w.transparent)
        return true;

    int radius = gui_scaled_metric(12);
    if (radius < 0)
        radius = 0;
    return gui_rounded_rect_coverage_local(px - outer.x, py - outer.y, outer.w, outer.h, radius,
                                           GUI_ROUNDED_EDGE_ALL) != 0;
}

static bool point_in_rounded_window_titlebar(const Window &w, int px, int py)
{
    if (w.transparent)
        return false;
    DirtyRect title = {w.x, w.y - wm_title_bar_h(), w.w, wm_title_bar_h()};
    if (!point_in_rect(title, px, py))
        return false;

    int radius = gui_scaled_metric(12) - wm_frame_border();
    if (radius < 0)
        radius = 0;
    if (radius > title.w / 2)
        radius = title.w / 2;
    if (radius > title.h)
        radius = title.h;
    return gui_rounded_rect_coverage_local(px - title.x, py - title.y, title.w, title.h, radius,
                                           GUI_ROUNDED_EDGE_TOP) != 0;
}

static bool point_in_rounded_window_client(const Window &w, int px, int py)
{
    DirtyRect client = window_visible_client_bounds(w);
    if (!point_in_rect(client, px, py))
        return false;
    if (w.transparent)
        return true;

    int inner_left = client.x;
    int inner_top = client.y;
    int inner_w = client.w;
    int inner_h = client.h;

    int inner_r = gui_scaled_metric(12) - wm_frame_border();
    if (inner_r < 0)
        inner_r = 0;
    if (inner_r > inner_w / 2)
        inner_r = inner_w / 2;
    if (inner_r > inner_h / 2)
        inner_r = inner_h / 2;
    return gui_rounded_rect_coverage_local(px - inner_left, py - inner_top, inner_w, inner_h, inner_r,
                                           GUI_ROUNDED_EDGE_BOTTOM) != 0;
}

bool point_in_titlebar(const Window &w, int px, int py)
{
    return point_in_rounded_window_titlebar(w, px, py);
}
bool point_in_client(const Window &w, int px, int py)
{
    return point_in_rounded_window_client(w, px, py);
}
bool point_in_outer(const Window &w, int px, int py)
{
    return point_in_rounded_window_outer(w, px, py);
}
bool point_in_button(const Window &w, int px, int py, int idx)
{
    return point_in_rect(window_button_bounds(w, idx), px, py);
}

int system_window_hit(int px, int py)
{
    for (int i = 1; i >= 0; i--) {
        if (i >= g_window_count)
            continue;
        const Window &w = g_windows[i];
        if (!is_window_visible(w) || !w.buffer)
            continue;
        if (w.transparent ? point_hits_window_visible_pixel(w, px, py) : point_in_client(w, px, py))
            return i;
    }
    return -1;
}

bool pointer_blocked_by_shell_overlay(int px, int py)
{
    return g_storage_prompt.visible || g_context_menu.open || g_index.active || g_control_center.open ||
           system_window_hit(px, py) >= 0;
}

void post_mouse_event_to_window(const Window &w, EventType type, int px, int py, uint8_t button, int8_t scroll_y)
{
    if (!is_user_window(w) || !is_window_visible(w))
        return;
    uint32_t owner = (w.entry && w.entry->owner_pid) ? w.entry->owner_pid : w.owner_pid;
    if (!owner)
        return;
    Event ev = {};
    ev.type = type;
    ev.mouse.x = px - w.x + w.scroll_x;
    ev.mouse.y = py - w.y + w.scroll_y;
    ev.mouse.button = button;
    ev.mouse.scroll_y = scroll_y;
    syscall2(SYS_POST_EVENT, owner, (uint64_t)&ev);
}

void post_key_event_to_window(const Window &w, EventType type, char c, uint8_t scancode)
{
    if (!is_user_window(w) || !is_window_visible(w))
        return;
    uint32_t owner = (w.entry && w.entry->owner_pid) ? w.entry->owner_pid : w.owner_pid;
    if (!owner)
        return;
    Event ev = {};
    ev.type = type;
    ev.key.c = c;
    ev.key.scancode = scancode;
    syscall2(SYS_POST_EVENT, owner, (uintptr_t)&ev);
}

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
        src = "";
    size_t i = 0;
    for (; i + 1 < dst_size && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static bool ascii_starts_with_ci(const char *text, const char *query)
{
    if (!text || !query)
        return false;
    while (*query) {
        if (!*text || ascii_lower(*text) != ascii_lower(*query))
            return false;
        text++;
        query++;
    }
    return true;
}

static bool ascii_contains_ci(const char *text, const char *query)
{
    if (!text || !query)
        return false;
    if (!*query)
        return true;
    for (const char *p = text; *p; p++) {
        const char *h = p;
        const char *n = query;
        while (*h && *n && ascii_lower(*h) == ascii_lower(*n)) {
            h++;
            n++;
        }
        if (!*n)
            return true;
    }
    return false;
}

struct IndexCatalogEntry
{
    const char *title;
    const char *detail;
    const char *path;
    bool is_app;
    IndexActionKind action;
};

static const IndexCatalogEntry k_index_catalog[] = {
    {"Terminal", "Command-line shell", "/bin/terminal.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Files", "Browse files and volumes", "/bin/files.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Settings", "System settings", "/bin/preferences.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Latitude", "Project editor", "/bin/latitude.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"About uniOS", "System information", "/bin/about.elf", true, INDEX_ACTION_LAUNCH_APP},
    {"Control Panel", "Network, appearance, volume", "control", false, INDEX_ACTION_OPEN_CONTROL_PANEL},
    {"Storage Mode", "Choose storage access", "storage", false, INDEX_ACTION_OPEN_STORAGE_PROMPT},
    {"Show Desktop", "Hide open windows", "desktop", false, INDEX_ACTION_SHOW_DESKTOP},
    {"Toggle Dark Mode", "Switch appearance", "appearance", false, INDEX_ACTION_TOGGLE_THEME},
    {"Toggle Desktop Grid", "Show or hide grid lines", "desktop grid", false, INDEX_ACTION_TOGGLE_DESKTOP_GRID},
    {"Toggle Clock Seconds", "Show or hide clock seconds", "clock seconds", false, INDEX_ACTION_TOGGLE_CLOCK_SECONDS},
    {"Toggle Motion", "Turn window motion on or off", "animations", false, INDEX_ACTION_TOGGLE_ANIMATIONS},
    {"Toggle Transparency", "Use transparent or solid surfaces", "transparency", false,
     INDEX_ACTION_TOGGLE_TRANSPARENCY},
};

static constexpr int INDEX_CATALOG_COUNT = (int)(sizeof(k_index_catalog) / sizeof(k_index_catalog[0]));

static int index_catalog_count()
{
    return INDEX_CATALOG_COUNT;
}

static int score_index_entry(const IndexCatalogEntry &entry, const char *query, int query_len, int ordinal)
{
    if (query_len <= 0)
        return 1000 - ordinal;

    int score = 0;
    if (ascii_starts_with_ci(entry.title, query))
        score = 1200;
    else if (ascii_contains_ci(entry.title, query))
        score = 950;
    else if (ascii_contains_ci(entry.detail, query))
        score = 650;
    else if (ascii_contains_ci(entry.path, query))
        score = 500;
    if (score == 0)
        return 0;
    if (entry.is_app)
        score += 24;
    return score - ordinal;
}

static void set_index_result(IndexResult &dst, const IndexCatalogEntry &src, int score)
{
    copy_cstr(dst.title, sizeof(dst.title), src.title);
    copy_cstr(dst.detail, sizeof(dst.detail), src.detail);
    copy_cstr(dst.path, sizeof(dst.path), src.path);
    dst.is_app = src.is_app;
    dst.action = src.action;
    dst.score = score;
}

DirtyRect index_overlay_bounds()
{
    int margin = gui_space_2();
    int max_w = (int)g_screen.width - margin * 2;
    int max_h = (int)g_screen.height - wm_menubar_h() - margin * 2;
    int min_w = gui_scaled_metric(280);
    int min_h = gui_scaled_metric(220);
    int bw = gui_scaled_metric(640);
    int bh = gui_scaled_metric(432);
    if (max_w > 0 && bw > max_w)
        bw = max_w;
    if (max_h > 0 && bh > max_h)
        bh = max_h;
    if (bw < min_w && max_w >= min_w)
        bw = min_w;
    if (bh < min_h && max_h >= min_h)
        bh = min_h;
    if (bw <= 0)
        bw = (int)g_screen.width;
    if (bh <= 0)
        bh = (int)g_screen.height;
    int x = ((int)g_screen.width - bw) / 2;
    int y = wm_menubar_h() + gui_scaled_metric(36);
    int max_y = (int)g_screen.height - bh - margin;
    if (y > max_y)
        y = max_y;
    if (x < margin)
        x = margin;
    if (y < wm_menubar_h() + margin)
        y = wm_menubar_h() + margin;
    return {x, y, bw, bh};
}

static DirtyRect index_damage_bounds()
{
    return rect_expand(index_overlay_bounds(), gui_scaled_metric(14));
}

static int index_result_item_h()
{
    int h = gui_scaled_metric(52);
    return h < gui_scaled_metric(40) ? gui_scaled_metric(40) : h;
}

static DirtyRect index_search_bounds()
{
    DirtyRect box = index_overlay_bounds();
    int pad = gui_space_2();
    int h = gui_scaled_metric(44);
    return {box.x + pad, box.y + pad, box.w - pad * 2, h};
}

static int index_results_start_y()
{
    DirtyRect search = index_search_bounds();
    return search.y + search.h + gui_space_1();
}

static int index_result_at(int mouse_x, int mouse_y)
{
    if (!g_index.active || !point_in_rect(index_overlay_bounds(), mouse_x, mouse_y))
        return -1;
    int y = index_results_start_y();
    int h = index_result_item_h();
    int pad = gui_space_2();
    DirtyRect box = index_overlay_bounds();
    int bottom = box.y + box.h - pad;
    for (int i = 0; i < g_index.result_count; i++) {
        DirtyRect row = {box.x + pad, y, box.w - pad * 2, h};
        if (row.y + row.h > bottom)
            break;
        if (point_in_rect(row, mouse_x, mouse_y))
            return i;
        y += h + gui_scaled_metric(2);
    }
    return -1;
}

void update_index_search()
{
    IndexResult sorted[INDEX_CATALOG_COUNT];
    int sorted_count = 0;

    for (int i = 0; i < index_catalog_count(); i++) {
        int score = score_index_entry(k_index_catalog[i], g_index.query, g_index.query_len, i);
        if (score <= 0)
            continue;
        IndexResult candidate = {};
        set_index_result(candidate, k_index_catalog[i], score);

        int insert_at = sorted_count;
        while (insert_at > 0 && sorted[insert_at - 1].score < candidate.score) {
            if (insert_at < INDEX_MAX_RESULTS)
                sorted[insert_at] = sorted[insert_at - 1];
            insert_at--;
        }
        if (insert_at < INDEX_MAX_RESULTS)
            sorted[insert_at] = candidate;
        if (sorted_count < INDEX_MAX_RESULTS)
            sorted_count++;
    }

    g_index.result_count = sorted_count;
    for (int i = 0; i < g_index.result_count; i++)
        g_index.results[i] = sorted[i];
    if (g_index.result_count <= 0) {
        g_index.selected_index = -1;
        g_index.hovered_index = -1;
    } else {
        if (g_index.selected_index < 0 || g_index.selected_index >= g_index.result_count)
            g_index.selected_index = 0;
        if (g_index.hovered_index >= g_index.result_count)
            g_index.hovered_index = -1;
    }

    DirtyRect damage = index_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

void open_index()
{
    if (g_index.active)
        return;
    close_control_center();
    close_context_menu();
    g_index = {};
    g_index.active = true;
    g_index.selected_index = 0;
    g_index.hovered_index = -1;
    g_index.open_ticks = get_ticks();
    update_index_search();
}

void close_index()
{
    if (!g_index.active)
        return;
    DirtyRect damage = index_damage_bounds();
    g_index.active = false;
    g_index.hovered_index = -1;
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

static void publish_settings_changed(Registry *registry)
{
    if (!registry)
        return;
    uint32_t next = (registry->settings_generation == 0xFFFFFFFFu) ? 1u : registry->settings_generation + 1u;
    registry->settings_generation = next;
    asm volatile("sfence" ::: "memory");
}

static void show_desktop_windows()
{
    for (int i = g_window_count - 1; i >= 2; i--)
        if (is_window_visible(g_windows[i]) && is_user_window(g_windows[i]))
            minimize_window(i);
}

bool activate_index_selection(Registry *registry)
{
    if (!g_index.active || g_index.selected_index < 0 || g_index.selected_index >= g_index.result_count)
        return false;

    IndexResult chosen = g_index.results[g_index.selected_index];
    close_index();

    switch (chosen.action) {
        case INDEX_ACTION_LAUNCH_APP:
            launch_or_focus_app(registry, chosen.title, chosen.path);
            return true;
        case INDEX_ACTION_OPEN_CONTROL_PANEL:
            toggle_control_center();
            return true;
        case INDEX_ACTION_OPEN_STORAGE_PROMPT:
            open_storage_prompt();
            return true;
        case INDEX_ACTION_SHOW_DESKTOP:
            show_desktop_windows();
            return true;
        case INDEX_ACTION_TOGGLE_THEME:
            if (registry) {
                registry->theme_mode = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_DARK : GUI_THEME_LIGHT;
                publish_settings_changed(registry);
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
            return true;
        case INDEX_ACTION_TOGGLE_DESKTOP_GRID:
            if (registry) {
                registry->system_flags ^= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
                g_system_flags = registry->system_flags;
                publish_settings_changed(registry);
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, wm_menubar_h());
            if (registry && registry->window_count > 1)
                enqueue_damage_rect(registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                                    registry->windows[1].h);
            return true;
        case INDEX_ACTION_TOGGLE_CLOCK_SECONDS:
            if (registry) {
                registry->system_flags ^= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
                g_system_flags = registry->system_flags;
                publish_settings_changed(registry);
                persist_wm_settings();
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, wm_menubar_h());
            return true;
        case INDEX_ACTION_TOGGLE_ANIMATIONS:
            g_control_center.animations_enabled = !g_control_center.animations_enabled;
            if (registry) {
                registry->animations_enabled = g_control_center.animations_enabled;
                publish_settings_changed(registry);
            } else {
                persist_wm_settings();
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
            return true;
        case INDEX_ACTION_TOGGLE_TRANSPARENCY:
            g_control_center.transparency_level = (g_control_center.transparency_level > 200) ? 180 : 255;
            if (registry) {
                registry->transparency_level = g_control_center.transparency_level;
                publish_settings_changed(registry);
            } else {
                persist_wm_settings();
            }
            enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
            return true;
        default:
            return false;
    }
}

bool handle_index_pointer_down(Registry *registry, int mouse_x, int mouse_y)
{
    if (!g_index.active)
        return false;
    if (!point_in_rect(index_overlay_bounds(), mouse_x, mouse_y))
        return false;
    int hit = index_result_at(mouse_x, mouse_y);
    if (hit >= 0) {
        g_index.selected_index = hit;
        DirtyRect damage = index_damage_bounds();
        enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
        activate_index_selection(registry);
    }
    return true;
}

void update_index_hover(int mouse_x, int mouse_y)
{
    if (!g_index.active)
        return;
    int hit = index_result_at(mouse_x, mouse_y);
    if (hit == g_index.hovered_index)
        return;
    g_index.hovered_index = hit;
    if (hit >= 0)
        g_index.selected_index = hit;
    DirtyRect damage = index_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

DirtyRect control_center_bounds()
{
    int margin = gui_space_1();
    int max_w = (int)g_screen.width - margin * 2;
    int max_h = (int)g_screen.height - wm_menubar_h() - margin * 2;
    int min_w = gui_scaled_metric(280);
    int min_h = gui_scaled_metric(300);
    int bw = gui_scaled_metric(348);
    int bh = gui_scaled_metric(366);
    if (max_w > 0 && bw > max_w)
        bw = max_w;
    if (max_h > 0 && bh > max_h)
        bh = max_h;
    if (bw < min_w && max_w >= min_w)
        bw = min_w;
    if (bh < min_h && max_h >= min_h)
        bh = min_h;
    if (bw <= 0)
        bw = (int)g_screen.width;
    if (bh <= 0)
        bh = (int)g_screen.height;
    int x = (int)g_screen.width - bw - margin;
    int y = wm_menubar_h() + margin;
    if (x < margin)
        x = margin;
    if (y < margin)
        y = margin;
    return {x, y, bw, bh};
}

static DirtyRect control_center_panel_damage_bounds()
{
    return rect_expand(control_center_bounds(), gui_scaled_metric(14));
}

static DirtyRect control_center_damage_bounds()
{
    DirtyRect cc = control_center_bounds();
    DirtyRect damage = rect_expand(cc, gui_scaled_metric(14));
    if (g_notifications.count > 0) {
        int notif_h = gui_scaled_metric(240);
        int notif_y = cc.y + cc.h + gui_space_2();
        DirtyRect notif_damage = rect_expand({cc.x, notif_y, cc.w, notif_h}, gui_scaled_metric(14));
        damage = rect_union(damage, notif_damage);
    }
    return damage;
}

static int control_panel_card_h()
{
    int h = gui_scaled_metric(54);
    return h < gui_scaled_metric(44) ? gui_scaled_metric(44) : h;
}

static DirtyRect control_panel_item_rect(ControlPanelItem item)
{
    DirtyRect box = control_center_bounds();
    int pad = gui_space_1_5();
    int gap = gui_space_1();
    int header_h = gui_card_header_h();
    int card_h = control_panel_card_h();
    int half_w = (box.w - pad * 2 - gap) / 2;
    int y = box.y + header_h + pad;

    if (item == CONTROL_ITEM_NETWORK)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_DARK_MODE)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_DESKTOP_GRID)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_CLOCK_SECONDS)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_ANIMATIONS)
        return {box.x + pad, y, half_w, card_h};
    if (item == CONTROL_ITEM_TRANSPARENCY)
        return {box.x + pad + half_w + gap, y, half_w, card_h};

    y += card_h + gap;
    if (item == CONTROL_ITEM_VOLUME)
        return {box.x + pad, y, box.w - pad * 2, gui_scaled_metric(62)};

    int action_h = gui_app_control_h();
    int action_y = box.y + box.h - pad - action_h;
    int action_w = (box.w - pad * 2 - gap) / 2;
    if (item == CONTROL_ITEM_STORAGE)
        return {box.x + pad, action_y, action_w, action_h};
    if (item == CONTROL_ITEM_SETTINGS)
        return {box.x + pad + action_w + gap, action_y, action_w, action_h};

    return {0, 0, 0, 0};
}

static ControlPanelItem control_panel_item_at(int mouse_x, int mouse_y)
{
    if (!point_in_rect(control_center_bounds(), mouse_x, mouse_y))
        return CONTROL_ITEM_NONE;
    ControlPanelItem items[] = {CONTROL_ITEM_NETWORK,       CONTROL_ITEM_DARK_MODE,  CONTROL_ITEM_DESKTOP_GRID,
                                CONTROL_ITEM_CLOCK_SECONDS, CONTROL_ITEM_ANIMATIONS, CONTROL_ITEM_TRANSPARENCY,
                                CONTROL_ITEM_VOLUME,        CONTROL_ITEM_STORAGE,    CONTROL_ITEM_SETTINGS};
    for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        if (point_in_rect(control_panel_item_rect(items[i]), mouse_x, mouse_y))
            return items[i];
    return CONTROL_ITEM_NONE;
}

static DirtyRect control_panel_volume_track_rect()
{
    DirtyRect card = control_panel_item_rect(CONTROL_ITEM_VOLUME);
    int pad = gui_space_1_5();
    int h = gui_scaled_metric(16);
    int y = card.y + card.h - pad - h;
    return {card.x + pad, y, card.w - pad * 2, h};
}

static bool set_control_center_volume_from_x(int mouse_x)
{
    DirtyRect track = control_panel_volume_track_rect();
    if (track.w <= 0)
        return false;
    int rel = mouse_x - track.x;
    if (rel < 0)
        rel = 0;
    if (rel > track.w)
        rel = track.w;
    uint32_t next = (uint32_t)((rel * 100 + track.w / 2) / track.w);
    if (next > 100)
        next = 100;
    if (next == g_control_center.volume)
        return true;
    g_control_center.volume = next;
    Registry *registry = gui_registry();
    if (registry) {
        registry->volume_level = next;
        publish_settings_changed(registry);
    }
    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
    return true;
}

void sync_control_center_state_from_registry(const Registry *registry)
{
    if (!registry)
        return;
    g_control_center.dark_mode = registry->theme_mode != GUI_THEME_LIGHT;
    g_control_center.desktop_grid = (registry->system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0;
    g_control_center.clock_seconds = (registry->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0;
    g_control_center.network_enabled = registry->ethernet_enabled;
    g_control_center.animations_enabled = registry->animations_enabled;
    g_control_center.transparency_level = registry->transparency_level;
    g_control_center.volume = registry->volume_level <= 100 ? registry->volume_level : 100;
}

void toggle_control_center()
{
    if (g_control_center.open) {
        close_control_center();
        return;
    }
    close_index();
    close_context_menu();
    sync_control_center_state_from_registry(gui_registry());
    g_control_center.open = true;
    Registry *reg = gui_registry();
    if (reg) {
        reg->cp_open = true;
        asm volatile("sfence" ::: "memory");
    }
    g_control_center.hovered_item = CONTROL_ITEM_NONE;
    g_control_center.volume_dragging = false;
    DirtyRect damage = control_center_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

void close_control_center()
{
    if (!g_control_center.open)
        return;
    DirtyRect damage = control_center_damage_bounds();
    g_control_center.open = false;
    Registry *reg = gui_registry();
    if (reg) {
        reg->cp_open = false;
        asm volatile("sfence" ::: "memory");
    }
    g_control_center.hovered_item = CONTROL_ITEM_NONE;
    g_control_center.volume_dragging = false;
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

static void set_theme_from_control_panel(Registry *registry, bool dark)
{
    if (!registry)
        return;
    registry->theme_mode = dark ? GUI_THEME_DARK : GUI_THEME_LIGHT;
    g_control_center.dark_mode = dark;
    publish_settings_changed(registry);
    enqueue_damage_rect(0, 0, (int)g_screen.width, (int)g_screen.height);
}

static void set_system_flag_from_control_panel(Registry *registry, uint32_t flag, bool enabled)
{
    if (!registry)
        return;
    if (enabled)
        registry->system_flags |= flag;
    else
        registry->system_flags &= ~flag;
    g_system_flags = registry->system_flags;
    sync_control_center_state_from_registry(registry);
    publish_settings_changed(registry);
    enqueue_damage_rect(0, 0, (int)g_screen.width, wm_menubar_h());
    if (registry->window_count > 1)
        enqueue_damage_rect(registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                            registry->windows[1].h);
}

static void publish_control_center_settings(Registry *registry)
{
    if (!registry) {
        persist_wm_settings();
        return;
    }
    registry->ethernet_enabled = g_control_center.network_enabled;
    registry->animations_enabled = g_control_center.animations_enabled;
    registry->transparency_level = g_control_center.transparency_level;
    registry->volume_level = g_control_center.volume;
    publish_settings_changed(registry);
}

bool handle_control_center_pointer_down(Registry *registry, int mouse_x, int mouse_y)
{
    if (!g_control_center.open)
        return false;
    DirtyRect cc_box = control_center_bounds();
    if (!point_in_rect(cc_box, mouse_x, mouse_y))
        return false;

    ControlPanelItem hit = control_panel_item_at(mouse_x, mouse_y);
    g_control_center.hovered_item = hit;
    if (hit == CONTROL_ITEM_NETWORK) {
        g_control_center.network_enabled = !g_control_center.network_enabled;
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_DARK_MODE) {
        set_theme_from_control_panel(registry, !g_control_center.dark_mode);
    } else if (hit == CONTROL_ITEM_DESKTOP_GRID) {
        set_system_flag_from_control_panel(registry, SYSTEM_FLAG_SHOW_DESKTOP_GRID, !g_control_center.desktop_grid);
    } else if (hit == CONTROL_ITEM_CLOCK_SECONDS) {
        set_system_flag_from_control_panel(registry, SYSTEM_FLAG_CLOCK_SHOW_SECONDS, !g_control_center.clock_seconds);
        persist_wm_settings();
    } else if (hit == CONTROL_ITEM_ANIMATIONS) {
        g_control_center.animations_enabled = !g_control_center.animations_enabled;
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_TRANSPARENCY) {
        g_control_center.transparency_level = (g_control_center.transparency_level > 200) ? 180 : 255;
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_VOLUME) {
        g_control_center.volume_dragging = true;
        set_control_center_volume_from_x(mouse_x);
        publish_control_center_settings(registry);
    } else if (hit == CONTROL_ITEM_STORAGE) {
        close_control_center();
        open_storage_prompt();
        return true;
    } else if (hit == CONTROL_ITEM_SETTINGS) {
        close_control_center();
        launch_or_focus_app(registry, "Settings", "/bin/preferences.elf");
        return true;
    }

    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
    return true;
}

void handle_control_center_pointer_up()
{
    if (!g_control_center.volume_dragging)
        return;
    g_control_center.volume_dragging = false;
    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

void update_control_center_hover(int mouse_x, int mouse_y)
{
    if (!g_control_center.open)
        return;
    ControlPanelItem hit = control_panel_item_at(mouse_x, mouse_y);
    if (hit == g_control_center.hovered_item)
        return;
    g_control_center.hovered_item = hit;
    DirtyRect damage = control_center_panel_damage_bounds();
    enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
}

bool update_control_center_drag(int mouse_x, int mouse_y)
{
    (void)mouse_y;
    if (!g_control_center.open || !g_control_center.volume_dragging)
        return false;
    return set_control_center_volume_from_x(mouse_x);
}

bool handle_control_center_scroll(Registry *registry, int mouse_x, int mouse_y, int scroll_y)
{
    if (!g_control_center.open || !point_in_rect(control_center_bounds(), mouse_x, mouse_y))
        return false;
    if (control_panel_item_at(mouse_x, mouse_y) == CONTROL_ITEM_VOLUME) {
        int delta = scroll_y > 0 ? 5 : (scroll_y < 0 ? -5 : 0);
        int next = (int)g_control_center.volume + delta;
        if (next < 0)
            next = 0;
        if (next > 100)
            next = 100;
        if ((uint32_t)next != g_control_center.volume) {
            g_control_center.volume = (uint32_t)next;
            publish_control_center_settings(registry);
            DirtyRect damage = control_center_panel_damage_bounds();
            enqueue_damage_rect(damage.x, damage.y, damage.w, damage.h);
        }
    }
    return true;
}

StoragePromptLayout storage_prompt_layout()
{
    StoragePromptLayout L = {};
    int bw = gui_scaled_metric(540);
    int bh = gui_scaled_metric(276);
    int btn_w = gui_scaled_metric(136);
    int btn_h = gui_app_control_h();
    int gap = gui_space_1();
    int outer_pad = gui_space_2();
    L.box = {(int)(g_screen.width - bw) / 2, (int)(g_screen.height - bh) / 2, bw, bh};
    int by = L.box.y + L.box.h - outer_pad - btn_h;
    L.writable_button = {L.box.x + L.box.w - outer_pad - btn_w, by, btn_w, btn_h};
    L.readonly_button = {L.writable_button.x - gap - btn_w, by, btn_w, btn_h};
    L.off_button = {L.readonly_button.x - gap - btn_w, by, btn_w, btn_h};
    return L;
}

void sync_storage_prompt_state(bool force)
{
    int mode = get_storage_mode();
    bool was = g_storage_prompt.visible;
    if (mode == STORAGE_MODE_WRITABLE && !force) {
        g_storage_prompt.visible = false;
        g_storage_prompt.dismissed = false;
        g_storage_prompt.hovered_button = -1;
    } else if (force || !g_storage_prompt.dismissed) {
        g_storage_prompt.visible = true;
        g_storage_prompt.hovered_button = -1;
    }
    if (g_storage_prompt.visible)
        clear_hover_feedback_state();
    if (was != g_storage_prompt.visible || force)
        enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
}

void open_storage_prompt()
{
    close_context_menu();
    g_storage_prompt.dismissed = false;
    sync_storage_prompt_state(true);
}
void dismiss_storage_prompt()
{
    if (!g_storage_prompt.visible)
        return;
    g_storage_prompt.visible = false;
    g_storage_prompt.dismissed = true;
    g_storage_prompt.hovered_button = -1;
    enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
}
void update_storage_prompt_hover(int mx, int my)
{
    if (!g_storage_prompt.visible)
        return;
    StoragePromptLayout L = storage_prompt_layout();
    int hov =
        point_in_rect(L.off_button, mx, my)
            ? 0
            : (point_in_rect(L.readonly_button, mx, my) ? 1 : (point_in_rect(L.writable_button, mx, my) ? 2 : -1));
    if (hov != g_storage_prompt.hovered_button) {
        g_storage_prompt.hovered_button = hov;
        enqueue_damage_rect(L.box.x, L.box.y, L.box.w, L.box.h);
    }
}

static void ensure_default_storage_file(const char *path, const char *fallback_path)
{
    VNodeStat st = {};
    if (!path || !fallback_path || (stat(path, &st) == 0 && !st.is_dir))
        return;

    int in = open(fallback_path, O_RDONLY);
    if (in < 0)
        return;
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return;
    }

    char buf[512];
    bool ok = true;
    while (true) {
        int n = read(in, buf, sizeof(buf));
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (write(out, buf, (size_t)n) != n) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);
    if (!ok)
        unlink(path);
}

void ensure_default_user_storage_layout()
{
    if (get_storage_mode() != STORAGE_MODE_WRITABLE)
        return;
    VNodeStat st = {};
    if (stat("/data", &st) != 0 || !st.is_dir)
        return;
    struct StandardDir
    {
        const char *c;
    } dirs[] = {{"/data/Desktop"}, {"/data/Documents"}, {"/data/Downloads"}, {"/data/Pictures"}};
    for (auto d : dirs) {
        if (stat(d.c, &st) == 0 && st.is_dir)
            continue;
        mkdir(d.c);
    }
    ensure_default_storage_file(SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH);
    ensure_default_storage_file(WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH);
}

bool apply_storage_mode_request(Registry *registry, int mode)
{
    if (!registry || mode < STORAGE_MODE_OFF || mode > STORAGE_MODE_WRITABLE || set_storage_mode(mode) != 0)
        return false;
    if (mode == STORAGE_MODE_WRITABLE)
        ensure_default_user_storage_layout();
    registry->storage_mode = mode;
    registry->storage_request_mode = mode;
    asm volatile("sfence" ::: "memory");
    sync_storage_prompt_state(false);
    return true;
}

bool activate_storage_prompt_button(Registry *registry, int mx, int my)
{
    if (!g_storage_prompt.visible)
        return false;
    StoragePromptLayout L = storage_prompt_layout();
    if (point_in_rect(L.off_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_OFF);
        dismiss_storage_prompt();
        return true;
    }
    if (point_in_rect(L.readonly_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_READ_ONLY);
        dismiss_storage_prompt();
        return true;
    }
    if (point_in_rect(L.writable_button, mx, my)) {
        apply_storage_mode_request(registry, STORAGE_MODE_WRITABLE);
        dismiss_storage_prompt();
        return true;
    }
    return false;
}

static int resolve_context_menu_target_index()
{
    if (!g_context_menu.open || g_context_menu.kind != CONTEXT_MENU_WINDOW)
        return -1;
    if (g_context_menu.target_entry) {
        int idx = find_window_by_entry(g_context_menu.target_entry);
        if (idx >= 2 && idx < g_window_count && is_window_visible(g_windows[idx]) &&
            g_windows[idx].entry == g_context_menu.target_entry) {
            g_context_menu.target_index = idx;
            return idx;
        }
    }
    if (g_context_menu.target_index >= 2 && g_context_menu.target_index < g_window_count) {
        const Window &w = g_windows[g_context_menu.target_index];
        if (w.entry && is_window_visible(w)) {
            g_context_menu.target_entry = w.entry;
            return g_context_menu.target_index;
        }
    }
    return -1;
}

static bool context_menu_targets_window_entry(const WindowEntry *entry)
{
    return entry && g_context_menu.open && g_context_menu.kind == CONTEXT_MENU_WINDOW &&
           g_context_menu.target_entry == entry;
}

static bool ensure_context_menu_target_valid()
{
    if (!g_context_menu.open || g_context_menu.kind != CONTEXT_MENU_WINDOW)
        return true;
    if (resolve_context_menu_target_index() >= 2)
        return true;
    close_context_menu();
    return false;
}

int build_context_menu_items(const Registry *registry, GuiMenuItem *items, int max_items)
{
    (void)registry;
    if (!items || max_items <= 0 || !g_context_menu.open)
        return 0;
    if (g_context_menu.kind == CONTEXT_MENU_DESKTOP && max_items >= 5) {
        items[0] = {"Open Terminal", true, false};
        items[1] = {"Open Files", true, false};
        items[2] = {"Settings", true, false};
        items[3] = {"Storage Mode", true, false};
        items[4] = {"Refresh Desktop", true, false};
        return 5;
    }
    if (g_context_menu.kind == CONTEXT_MENU_WINDOW && max_items >= 5) {
        int target_index = resolve_context_menu_target_index();
        bool can_act = target_index >= 2 && g_windows[target_index].entry;
        bool maxed = can_act && g_windows[target_index].entry->state == WIN_MAXIMIZED;
        items[0] = {maxed ? "Restore Window" : "Maximize Window", can_act, false};
        items[1] = {"Minimize Window", can_act, false};
        items[2] = {"Close Window", can_act, false};
        items[3] = {"Settings", true, false};
        items[4] = {"Storage Mode", true, false};
        return 5;
    }
    return 0;
}

void open_context_menu(const Registry *registry, ContextMenuKind kind, int target_index, int anchor_x, int anchor_y)
{
    if (!registry || kind == CONTEXT_MENU_NONE)
        return;
    if (g_context_menu.open) {
        DirtyRect prev = context_menu_bounds();
        enqueue_damage_rect(prev.x, prev.y, prev.w, prev.h);
    }
    clear_hover_feedback_state();

    g_context_menu = {};
    g_context_menu.open = true;
    g_context_menu.kind = kind;
    g_context_menu.target_index = target_index;
    g_context_menu.target_entry = (kind == CONTEXT_MENU_WINDOW && target_index >= 2 && target_index < g_window_count)
                                      ? g_windows[target_index].entry
                                      : nullptr;

    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count <= 0) {
        g_context_menu = {};
        return;
    }
    g_context_menu.w = gui_popup_menu_width(items, count, gui_scaled_metric(176));
    g_context_menu.h = gui_popup_menu_height(items, count);
    g_context_menu.x = anchor_x;
    g_context_menu.y = anchor_y;
    if (g_context_menu.x + g_context_menu.w > (int)g_screen.width)
        g_context_menu.x = g_screen.width - g_context_menu.w - gui_scaled_metric(8);
    if (g_context_menu.y + g_context_menu.h > (int)g_screen.height)
        g_context_menu.y = g_screen.height - g_context_menu.h - gui_scaled_metric(8);
    if (g_context_menu.x < 0)
        g_context_menu.x = 0;
    if (g_context_menu.y < 0)
        g_context_menu.y = 0;
    g_context_menu.hovered_index =
        gui_popup_menu_hit_test(items, count, g_context_menu.x, g_context_menu.y, g_context_menu.w, anchor_x, anchor_y);
    enqueue_damage_rect(g_context_menu.x, g_context_menu.y, g_context_menu.w, g_context_menu.h);
}

void close_context_menu()
{
    if (!g_context_menu.open)
        return;
    DirtyRect bounds = context_menu_bounds();
    enqueue_damage_rect(bounds.x, bounds.y, bounds.w, bounds.h);
    g_context_menu = {};
}

void update_context_menu_hover(const Registry *registry, int mx, int my)
{
    if (!g_context_menu.open)
        return;
    if (!ensure_context_menu_target_valid())
        return;
    GuiMenuItem items[8];
    int count = build_context_menu_items(registry, items, 8);
    if (count <= 0) {
        close_context_menu();
        return;
    }
    int hov = gui_popup_menu_hit_test(items, count, g_context_menu.x, g_context_menu.y, g_context_menu.w, mx, my);
    if (hov != g_context_menu.hovered_index) {
        g_context_menu.hovered_index = hov;
        DirtyRect b = context_menu_bounds();
        enqueue_damage_rect(b.x, b.y, b.w, b.h);
    }
}

static void launch_or_focus_app(Registry *registry, const char *title, const char *path)
{
    for (uint32_t i = 2; i < (registry->window_count > MAX_WINDOWS ? MAX_WINDOWS : registry->window_count); i++) {
        WindowEntry &e = registry->windows[i];
        if (!e.ready || !gui_shm_id_is_valid(e.shm_id) || !e.owner_pid || strcmp(e.title, title) != 0)
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

bool activate_context_menu_item(Registry *registry, int index)
{
    if (!registry || !g_context_menu.open || index < 0)
        return false;
    if (g_context_menu.kind == CONTEXT_MENU_DESKTOP) {
        if (index == 0)
            launch_or_focus_app(registry, "Terminal", "/bin/terminal.elf");
        else if (index == 1)
            launch_or_focus_app(registry, "Files", "/bin/files.elf");
        else if (index == 2)
            launch_or_focus_app(registry, "Settings", "/bin/preferences.elf");
        else if (index == 3)
            open_storage_prompt();
        else if (index == 4)
            enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
        else
            return false;
        close_context_menu();
        return true;
    }
    if (g_context_menu.kind == CONTEXT_MENU_WINDOW) {
        int target_index = resolve_context_menu_target_index();
        if (target_index < 2) {
            close_context_menu();
            return false;
        }
        const Window &t = g_windows[target_index];
        if (!t.entry) {
            close_context_menu();
            return false;
        }
        t.entry->request_focus = true;
        if (index == 0) {
            if (t.entry->state == WIN_MAXIMIZED)
                t.entry->request_restore = true;
            else
                t.entry->request_maximize = true;
        } else if (index == 1)
            t.entry->request_minimize = true;
        else if (index == 2)
            t.entry->request_close = true;
        else if (index == 3)
            launch_or_focus_app(registry, "Settings", "/bin/preferences.elf");
        else if (index == 4)
            open_storage_prompt();
        else
            return false;
        asm volatile("sfence" ::: "memory");
        close_context_menu();
        return true;
    }
    return false;
}

DirtyRect context_menu_bounds()
{
    if (!g_context_menu.open)
        return {0, 0, 0, 0};
    int shadow_pad_x = gui_scaled_metric(8);
    int shadow_pad_y = gui_scaled_metric(12);
    return {g_context_menu.x - shadow_pad_x, g_context_menu.y, g_context_menu.w + shadow_pad_x * 2,
            g_context_menu.h + shadow_pad_y};
}

static bool cfg_value_enabled(const char *value, bool fallback)
{
    if (!value || !*value)
        return fallback;
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0 || strcmp(value, "no") == 0)
        return false;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0 || strcmp(value, "yes") == 0)
        return true;
    return fallback;
}

static const char *flag_text(uint32_t flags, uint32_t flag)
{
    return (flags & flag) ? "1" : "0";
}

static uint32_t cfg_uint_clamped(const char *value, uint32_t fallback, uint32_t min, uint32_t max)
{
    if (!value || !*value)
        return fallback;
    int parsed = atoi(value);
    if (parsed < (int)min)
        return min;
    if (parsed > (int)max)
        return max;
    return (uint32_t)parsed;
}

RuntimeGuiSettings load_runtime_settings()
{
    RuntimeGuiSettings s = {GUI_THEME_DARK, SYSTEM_FLAG_SHOW_DESKTOP_GRID, true, true, true, 180, 75};
    char cfg[512], val[64];
    const char *cands[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(cands, 2, cfg, sizeof(cfg))) {
        if (cfg_line_value(cfg, "theme", val, sizeof(val))) {
            if (strcmp(val, "light") == 0)
                s.theme_mode = GUI_THEME_LIGHT;
            else if (strcmp(val, "dark") == 0)
                s.theme_mode = GUI_THEME_DARK;
        }
        if (cfg_line_value(cfg, "show_desktop_grid", val, sizeof(val))) {
            if (cfg_value_enabled(val, (s.system_flags & SYSTEM_FLAG_SHOW_DESKTOP_GRID) != 0))
                s.system_flags |= SYSTEM_FLAG_SHOW_DESKTOP_GRID;
            else
                s.system_flags &= ~SYSTEM_FLAG_SHOW_DESKTOP_GRID;
        }
        if (cfg_line_value(cfg, "clock_show_seconds", val, sizeof(val))) {
            if (cfg_value_enabled(val, (s.system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0))
                s.system_flags |= SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
            else
                s.system_flags &= ~SYSTEM_FLAG_CLOCK_SHOW_SECONDS;
        }
        if (cfg_line_value(cfg, "launch_terminal_on_boot", val, sizeof(val))) {
            if (cfg_value_enabled(val, (s.system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) != 0))
                s.system_flags |= SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
            else
                s.system_flags &= ~SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT;
        }
        if (cfg_line_value(cfg, "ethernet_enabled", val, sizeof(val)))
            s.ethernet_enabled = cfg_value_enabled(val, s.ethernet_enabled);
        if (cfg_line_value(cfg, "ethernet_use_dhcp", val, sizeof(val)))
            s.ethernet_use_dhcp = cfg_value_enabled(val, s.ethernet_use_dhcp);
        if (cfg_line_value(cfg, "animations_enabled", val, sizeof(val)))
            s.animations_enabled = cfg_value_enabled(val, s.animations_enabled);
        if (cfg_line_value(cfg, "transparency_level", val, sizeof(val)))
            s.transparency_level = cfg_uint_clamped(val, s.transparency_level, 0, 255);
        if (cfg_line_value(cfg, "volume_level", val, sizeof(val)))
            s.volume_level = cfg_uint_clamped(val, s.volume_level, 0, 100);
    }
    return s;
}

bool persist_runtime_settings(const Registry *registry)
{
    if (!registry)
        return false;

    uint32_t flags = registry->system_flags;
    GuiThemeMode mode = registry->theme_mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK;
    char contents[384];
    int n = snprintf(contents, sizeof(contents),
                     "theme=%s\n"
                     "show_desktop_grid=%s\n"
                     "clock_show_seconds=%s\n"
                     "launch_terminal_on_boot=%s\n"
                     "ethernet_enabled=%d\n"
                     "ethernet_use_dhcp=%d\n"
                     "animations_enabled=%d\n"
                     "transparency_level=%u\n"
                     "volume_level=%u\n",
                     mode == GUI_THEME_LIGHT ? "light" : "dark", flag_text(flags, SYSTEM_FLAG_SHOW_DESKTOP_GRID),
                     flag_text(flags, SYSTEM_FLAG_CLOCK_SHOW_SECONDS),
                     flag_text(flags, SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT), registry->ethernet_enabled ? 1 : 0,
                     registry->ethernet_use_dhcp ? 1 : 0, registry->animations_enabled ? 1 : 0,
                     registry->transparency_level, registry->volume_level <= 100 ? registry->volume_level : 100);
    if (n <= 0 || (size_t)n >= sizeof(contents))
        return false;
    return cfg_write_text_file(SYSTEM_CONFIG_PATH, contents);
}

void add_win_internal(int shm_id, int x, int y, int w, int h, const char *title, Damage *d_ptr, WindowEntry *entry,
                      bool transparent)
{
    if (w <= 0 || h <= 0 || !gui_shm_id_is_valid(shm_id)) {
        if (g_add_fail_logs < 8) {
            LOG_WARN("wm", "add skipped: shm=%d", shm_id);
            g_add_fail_logs++;
        }
        return;
    }
    uint64_t ptr = syscall1(SYS_SHM_MAP, (uint64_t)shm_id);
    if (ptr == 0 || ptr == (uint64_t)-1) {
        if (g_add_fail_logs < 8) {
            LOG_ERROR("wm", "add MAP failed: shm=%d", shm_id);
            g_add_fail_logs++;
        }
        return;
    }
    if (g_window_count >= MAX_WINDOWS) {
        if (g_add_fail_logs < 8) {
            LOG_WARN("wm", "table full");
            g_add_fail_logs++;
        }
        syscall1(SYS_SHM_UNMAP, (uint64_t)shm_id);
        return;
    }

    int bw = (entry && entry->buffer_w > 0) ? entry->buffer_w : w;
    int bh = (entry && entry->buffer_h > 0) ? entry->buffer_h : h;
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
    win.scroll_x = 0;
    win.scroll_y = 0;
    win.min_w = entry ? entry->min_w : 0;
    win.min_h = entry ? entry->min_h : 0;
    win.active = true;
    win.transparent = transparent;
    win.needs_full_redraw = false;       // Wait for first damage to avoid black flash
    win.last_commit_ticks = get_ticks(); // Track birth time for watchdog
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

    if (update_control_center_drag(g_input.mouse_x, g_input.mouse_y)) {
        mark_cursor_transition_damage(g_input.old_mouse_x, g_input.old_mouse_y, g_input.cursor_kind, g_input.mouse_x,
                                      g_input.mouse_y, g_input.cursor_kind);
        return;
    }

    if (g_input.pointer_down && g_input.drag_index >= 2 && g_input.drag_index < g_window_count) {
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
    if (g_storage_prompt.visible)
        return;
    update_index_hover(g_input.mouse_x, g_input.mouse_y);
    if (g_index.active)
        return;
    update_control_center_hover(g_input.mouse_x, g_input.mouse_y);
    if (g_control_center.open)
        return;
    update_context_menu_hover(registry, g_input.mouse_x, g_input.mouse_y);
    if (pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y))
        return;

    if (g_input.drag_index < 2) {
        int focus = find_registry_focused_user_window(registry);
        if (focus >= 2) {
            const Window &fw = g_windows[focus];
            bool hit = fw.transparent ? point_hits_window_visible_pixel(fw, g_input.mouse_x, g_input.mouse_y)
                                      : point_in_client(fw, g_input.mouse_x, g_input.mouse_y);
            if (hit)
                post_mouse_event_to_window(fw, EVT_MOUSE_MOVE, g_input.mouse_x, g_input.mouse_y, 0);
        }
    }
}

void update_hover_feedback()
{
    int nhf = -1, nre = RESIZE_NONE, nhb = -1;
    if (!g_input.pointer_down && !pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y)) {
        for (int i = g_window_count - 1; i >= 2; i--) {
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
    if (nhf >= 2 && nhf != g_window_count - 1) {
        nhf = -1;
        nre = RESIZE_NONE;
        nhb = -1;
    }
    if (nhf == g_input.hover_frame_index && nre == g_input.hover_resize_edges && nhb == g_input.hover_button)
        return;
    int old_hover_frame = g_input.hover_frame_index;
    if (old_hover_frame >= 2 && old_hover_frame < g_window_count)
        mark_window_chrome_damage(g_windows[old_hover_frame]);
    g_input.hover_frame_index = nhf;
    g_input.hover_resize_edges = nre;
    g_input.hover_button = nhb;
    if (g_input.hover_frame_index >= 2 && g_input.hover_frame_index < g_window_count &&
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
    if (g_input.pointer_down && g_input.drag_index >= 2)
        n_k = g_input.drag_edges == RESIZE_NONE ? GUI_CURSOR_MOVE : ck_edges(g_input.drag_edges);
    else if (g_input.hover_resize_edges != RESIZE_NONE)
        n_k = ck_edges(g_input.hover_resize_edges);
    else if (g_input.hover_frame_index >= 2)
        n_k = GUI_CURSOR_MOVE;
    if (!g_input.pointer_down)
        reset_window_snap_state();
    if (n_k != g_input.cursor_kind) {
        mark_cursor_transition_damage(g_input.mouse_x, g_input.mouse_y, g_input.cursor_kind, g_input.mouse_x,
                                      g_input.mouse_y, n_k);
        g_input.cursor_kind = n_k;
    }
}
void persist_wm_settings()
{
    char config[512];
    snprintf(config, sizeof(config),
             "theme=%s\n"
             "show_desktop_grid=%d\n"
             "clock_show_seconds=%d\n"
             "launch_terminal_on_boot=%d\n"
             "ethernet_enabled=%d\n"
             "ethernet_use_dhcp=%d\n"
             "animations_enabled=%d\n"
             "transparency_level=%u\n"
             "volume_level=%u\n",
             g_control_center.dark_mode ? "dark" : "light", g_control_center.desktop_grid ? 1 : 0,
             g_control_center.clock_seconds ? 1 : 0, (g_system_flags & SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT) ? 1 : 0,
             g_control_center.network_enabled ? 1 : 0, 1, g_control_center.animations_enabled ? 1 : 0,
             g_control_center.transparency_level, g_control_center.volume);

    cfg_write_text_file(SYSTEM_CONFIG_PATH, config);
}

void load_wm_settings()
{
    char config[1024];
    const char *candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(candidates, 2, config, sizeof(config))) {
        char value[64];
        if (cfg_line_value(config, "theme", value, sizeof(value))) {
            g_control_center.dark_mode = (strcmp(value, "light") != 0);
        }
        if (cfg_line_value(config, "show_desktop_grid", value, sizeof(value))) {
            g_control_center.desktop_grid = (value[0] != '0');
        }
        if (cfg_line_value(config, "clock_show_seconds", value, sizeof(value))) {
            g_control_center.clock_seconds = (value[0] != '0');
        }
        if (cfg_line_value(config, "ethernet_enabled", value, sizeof(value))) {
            g_control_center.network_enabled = cfg_value_enabled(value, g_control_center.network_enabled);
        }
        if (cfg_line_value(config, "animations_enabled", value, sizeof(value))) {
            g_control_center.animations_enabled = cfg_value_enabled(value, g_control_center.animations_enabled);
        }
        if (cfg_line_value(config, "transparency_level", value, sizeof(value))) {
            g_control_center.transparency_level = cfg_uint_clamped(value, g_control_center.transparency_level, 0, 255);
        }
        if (cfg_line_value(config, "volume_level", value, sizeof(value))) {
            g_control_center.volume = cfg_uint_clamped(value, g_control_center.volume, 0, 100);
        }
    }
}
