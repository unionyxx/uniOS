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
bool g_window_visible_region_overflow = false;
int g_dirty_count = 0;
bool g_window_visibility_cache_dirty = true;
bool g_dirty_frame_ready = false;

ContextMenuState g_context_menu = {};
StoragePromptState g_storage_prompt = {};
WmInputState g_input;

struct CursorPresentBuffer
{
    DisplayBufferHandle handle;
    Surface surface;
    GuiCursorKind kind;
    int hot_x;
    int hot_y;
    bool valid;
};

static CursorPresentBuffer g_cursor_present_buffers[GUI_CURSOR_RESIZE_D2 + 1] = {};
static bool g_cursor_backend_disabled = false;
static DisplayBufferHandle g_frame_cursor_handle = 0;
static int g_frame_cursor_x = 0;
static int g_frame_cursor_y = 0;

struct WindowEntrySnapshot
{
    int shm_id;
    int x, y, w, h;
    int buffer_w, buffer_h;
    int content_w, content_h;
    int min_w, min_h;
    int scroll_x, scroll_y;
    uint32_t flags;
    uint32_t state;
    uint32_t owner_pid;
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

static void apply_display_event(const DisplayEvent &event)
{
    if (event.type == DISPLAY_EVENT_FLIP_COMPLETE && event.sequence > g_display_queue.completed_sequence)
        g_display_queue.completed_sequence = event.sequence;
    if ((event.type == DISPLAY_EVENT_FLIP_COMPLETE || event.type == DISPLAY_EVENT_VBLANK) &&
        event.timestamp_ticks > g_display_queue.last_vblank_ticks)
        g_display_queue.last_vblank_ticks = event.timestamp_ticks;
    if (event.vblank_count > g_display_queue.vblank_count)
        g_display_queue.vblank_count = event.vblank_count;
}

static uint64_t dirty_area_sum(const DirtyRect *rects, int rect_count)
{
    uint64_t area = 0;
    rect_count = clamp_dirty_rect_count(rect_count);
    for (int i = 0; i < rect_count; i++) {
        DirtyRect clipped = rects[i];
        if (!clip_dirty_rect_to_screen(clipped))
            continue;
        area += (uint64_t)clipped.w * (uint64_t)clipped.h;
    }
    return area;
}

void wm_stats_note_dirty_set(const DirtyRect *rects, int rect_count)
{
    rect_count = clamp_dirty_rect_count(rect_count);
    uint64_t area = dirty_area_sum(rects, rect_count);
    g_frame_stats.last_dirty_rects = (uint32_t)rect_count;
    g_frame_stats.last_dirty_area = area;
    g_frame_stats.dirty_area_accum += area;
    if ((uint32_t)rect_count > g_frame_stats.max_dirty_rects)
        g_frame_stats.max_dirty_rects = (uint32_t)rect_count;
    if (area > g_frame_stats.max_dirty_area)
        g_frame_stats.max_dirty_area = area;
    if (rect_count == 1 && area == (uint64_t)g_screen.width * (uint64_t)g_screen.height)
        g_frame_stats.full_repaints++;
    else
        g_frame_stats.clipped_repaints++;
}

void wm_stats_note_stale_repair(int rect_count)
{
    if (rect_count > 0)
        g_frame_stats.stale_slot_repairs += (uint64_t)rect_count;
}

static WindowEntrySnapshot read_window_entry_snapshot(const WindowEntry &e)
{
    WindowEntrySnapshot s = {};
    s.shm_id = e.shm_id;
    s.x = e.x;
    s.y = e.y;
    s.w = e.w;
    s.h = e.h;
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
           a.buffer_w == b.buffer_w && a.buffer_h == b.buffer_h && a.content_w == b.content_w &&
           a.content_h == b.content_h && a.min_w == b.min_w && a.min_h == b.min_h &&
           a.scroll_x == b.scroll_x && a.scroll_y == b.scroll_y && a.flags == b.flags && a.state == b.state &&
           a.owner_pid == b.owner_pid && a.buffer_generation == b.buffer_generation &&
           a.buffer_ack_generation == b.buffer_ack_generation && a.active == b.active && a.ready == b.ready &&
           memcmp(a.title, b.title, sizeof(a.title)) == 0;
}

static bool sample_stable_window_entry(const WindowEntry &entry, WindowEntrySnapshot *out)
{
    WindowEntrySnapshot a = read_window_entry_snapshot(entry);
    asm volatile("lfence" ::: "memory");
    WindowEntrySnapshot b = read_window_entry_snapshot(entry);
    if (!window_entry_snapshot_equal_for_commit(a, b))
        return false;
    if (out)
        *out = b;
    return true;
}

static bool cursor_backend_allowed()
{
    return !g_cursor_backend_disabled && !g_display_copy_path &&
           (g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0 &&
           (g_display_caps.flags & DISPLAY_FLAG_HAS_PAGE_FLIP) != 0 && g_presentbuffer_handle != 0;
}

static CursorPresentBuffer *ensure_cursor_present_buffer(GuiCursorKind kind)
{
    int index = (int)kind;
    if (index < 0 || index > (int)GUI_CURSOR_RESIZE_D2 || !cursor_backend_allowed())
        return nullptr;

    CursorPresentBuffer &slot = g_cursor_present_buffers[index];
    if (slot.valid && slot.handle != 0 && slot.surface.buffer)
        return &slot;

    int32_t bx = 0, by = 0, bw = 0, bh = 0;
    gui_get_cursor_bounds(kind, 0, 0, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0 || bw > CURSOR_MAX_SIZE || bh > CURSOR_MAX_SIZE)
        return nullptr;

    DisplayBufferCreate create = {};
    create.width = (uint32_t)bw;
    create.height = (uint32_t)bh;
    create.pixel_format = DISPLAY_PIXEL_FORMAT_XRGB8888;
    create.flags = DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR | DISPLAY_BUFFER_FLAG_RENDER_TARGET;
    if (display_buffer_create(&create) != 0 || create.handle == 0)
        return nullptr;

    DisplayBufferMap map = {};
    map.handle = create.handle;
    if (display_buffer_map(&map) != 0 || map.address == 0 || map.stride < (uint32_t)bw) {
        display_buffer_destroy(create.handle);
        return nullptr;
    }

    memset(&slot, 0, sizeof(slot));
    slot.handle = create.handle;
    slot.surface.width = (uint32_t)bw;
    slot.surface.height = (uint32_t)bh;
    slot.surface.pitch = map.stride * 4u;
    slot.surface.buffer = (uint32_t *)map.address;
    slot.kind = kind;
    gui_get_cursor_hotspot(kind, &slot.hot_x, &slot.hot_y);
    gui_fill_rect(&slot.surface, 0, 0, bw, bh, 0x00000000u);
    gui_draw_cursor_kind(&slot.surface, slot.hot_x, slot.hot_y, kind);
    asm volatile("sfence" ::: "memory");
    slot.valid = true;
    return &slot;
}

static void retire_completed_present_buffers()
{
    for (uint32_t i = 0; i < g_presentbuffer_slot_count; i++) {
        if (g_presentbuffer_slots[i].in_flight_sequence &&
            g_presentbuffer_slots[i].in_flight_sequence <= g_display_queue.completed_sequence)
            g_presentbuffer_slots[i].in_flight_sequence = 0;
    }
}

static void sync_presentbuffer_alias_from_active_slot()
{
    if (g_presentbuffer_slot_count == 0 || g_presentbuffer_active_slot >= g_presentbuffer_slot_count) {
        g_presentbuffer = {};
        g_presentbuffer_handle = 0;
        return;
    }
    g_presentbuffer = g_presentbuffer_slots[g_presentbuffer_active_slot].surface;
    g_presentbuffer_handle = g_presentbuffer_slots[g_presentbuffer_active_slot].handle;
}

static bool dirty_set_is_single_fullscreen_rect()
{
    if (clamp_dirty_rect_count(g_dirty_count) != 1)
        return false;
    DirtyRect rect = g_dirty_rects[0];
    if (!clip_dirty_rect_to_screen(rect))
        return false;
    return rect.x == 0 && rect.y == 0 && rect.w == (int)g_screen.width && rect.h == (int)g_screen.height;
}

static bool dirty_set_intersects_rect(const DirtyRect &target)
{
    int count = clamp_dirty_rect_count(g_dirty_count);
    for (int i = 0; i < count; i++) {
        DirtyRect dirty = g_dirty_rects[i];
        if (clip_dirty_rect_to_screen(dirty) && rect_intersection(dirty, target, nullptr))
            return true;
    }
    return false;
}

static bool dirty_set_contains_rect(const DirtyRect &target)
{
    int count = clamp_dirty_rect_count(g_dirty_count);
    for (int i = 0; i < count; i++) {
        DirtyRect dirty = g_dirty_rects[i];
        if (clip_dirty_rect_to_screen(dirty) && rect_contains(dirty, target))
            return true;
    }
    return false;
}

static bool prepare_cursor_overlay_damage(bool interactive, DirtyRect *cursor_rect_out)
{
    if (!cursor_rect_out)
        return false;

    DirtyRect cursor_rect = {};
    gui_get_cursor_bounds(g_input.cursor_kind, g_input.mouse_x, g_input.mouse_y, &cursor_rect.x, &cursor_rect.y,
                          &cursor_rect.w, &cursor_rect.h);
    if (!clip_dirty_rect_to_screen(cursor_rect))
        return false;
    if (!dirty_set_intersects_rect(cursor_rect))
        return false;

    if (!dirty_set_contains_rect(cursor_rect)) {
        enqueue_damage_rect(cursor_rect.x, cursor_rect.y, cursor_rect.w, cursor_rect.h);
        normalize_dirty_rects(interactive);
        gui_get_cursor_bounds(g_input.cursor_kind, g_input.mouse_x, g_input.mouse_y, &cursor_rect.x, &cursor_rect.y,
                              &cursor_rect.w, &cursor_rect.h);
        if (!clip_dirty_rect_to_screen(cursor_rect))
            return false;
    }

    *cursor_rect_out = cursor_rect;
    return true;
}

static void collapse_dirty_rects_to_bounds()
{
    int count = clamp_dirty_rect_count(g_dirty_count);
    DirtyRect bounds = {};
    bool have_bounds = false;
    for (int i = 0; i < count; i++) {
        DirtyRect dirty = g_dirty_rects[i];
        if (!clip_dirty_rect_to_screen(dirty))
            continue;
        bounds = have_bounds ? rect_union(bounds, dirty) : dirty;
        have_bounds = true;
    }

    if (!have_bounds) {
        g_dirty_count = 0;
        invalidate_dirty_frame();
        return;
    }

    g_dirty_rects[0] = bounds;
    g_dirty_count = 1;
    invalidate_dirty_frame();
}

static void clear_presentbuffer_slot_stale(PresentBufferSlot &slot)
{
    slot.stale_count = 0;
}

static void mark_presentbuffer_slot_stale(PresentBufferSlot &slot, const DirtyRect &dirty)
{
    DirtyRect clipped = dirty;
    if (!clip_dirty_rect_to_screen(clipped))
        return;

    wm::DirtyRect rects[MAX_DIRTY_RECTS];
    int count = clamp_dirty_rect_count(slot.stale_count);
    for (int i = 0; i < count; i++)
        rects[i] = to_policy_rect(slot.stale_rects[i]);

    wm::enqueue_damage_rect(rects, &count, MAX_DIRTY_RECTS, (int)g_screen.width, (int)g_screen.height,
                            to_policy_rect(clipped));

    slot.stale_count = clamp_dirty_rect_count(count);
    for (int i = 0; i < slot.stale_count; i++)
        slot.stale_rects[i] = from_policy_rect(rects[i]);
}

void mark_presentbuffer_slots_stale(const DirtyRect &dirty)
{
    for (uint32_t i = 0; i < g_presentbuffer_slot_count; i++)
        mark_presentbuffer_slot_stale(g_presentbuffer_slots[i], dirty);
}

static void mark_other_presentbuffer_slots_stale(const DirtyRect *rects, int rect_count, uint32_t fresh_slot)
{
    if (!rects || rect_count <= 0)
        return;
    rect_count = clamp_dirty_rect_count(rect_count);
    for (uint32_t i = 0; i < g_presentbuffer_slot_count; i++) {
        if (i == fresh_slot)
            continue;
        for (int r = 0; r < rect_count; r++)
            mark_presentbuffer_slot_stale(g_presentbuffer_slots[i], rects[r]);
    }
}

static bool sync_presentbuffer_slot_from_active(uint32_t slot_index, bool overwrite_full_frame)
{
    if (slot_index >= g_presentbuffer_slot_count)
        return false;

    PresentBufferSlot &dst = g_presentbuffer_slots[slot_index];
    if (!dst.surface.buffer)
        return false;
    if (overwrite_full_frame) {
        clear_presentbuffer_slot_stale(dst);
        return true;
    }

    int stale_count = clamp_dirty_rect_count(dst.stale_count);
    wm_stats_note_stale_repair(stale_count);
    for (int i = 0; i < stale_count; i++) {
        DirtyRect stale = dst.stale_rects[i];
        if (!clip_dirty_rect_to_screen(stale))
            continue;
        gui_blit_rect(&dst.surface, &g_backbuffer, stale.x, stale.y, stale.x, stale.y, stale.w, stale.h);
    }
    clear_presentbuffer_slot_stale(dst);
    return true;
}

static bool select_presentbuffer_slot_for_frame()
{
    retire_completed_present_buffers();
    if (g_presentbuffer_slot_count == 0)
        return false;
    bool overwrite_full_frame = dirty_set_is_single_fullscreen_rect();
    for (uint32_t offset = 0; offset < g_presentbuffer_slot_count; offset++) {
        uint32_t index = (g_presentbuffer_active_slot + offset) % g_presentbuffer_slot_count;
        if (g_presentbuffer_slots[index].in_flight_sequence != 0)
            continue;
        if (!sync_presentbuffer_slot_from_active(index, overwrite_full_frame))
            continue;
        g_presentbuffer_active_slot = index;
        sync_presentbuffer_alias_from_active_slot();
        return true;
    }
    sync_presentbuffer_alias_from_active_slot();
    return false;
}

static void drain_display_events()
{
    DisplayEvent event = {};
    while (display_poll_event(&event) == 0)
        apply_display_event(event);
    retire_completed_present_buffers();
}

static void refresh_display_queue_from_status()
{
    DisplayStatus status = {};
    if (display_get_status(&status) == 0) {
        if (status.completed_sequence > g_display_queue.completed_sequence)
            g_display_queue.completed_sequence = status.completed_sequence;
        if (status.last_vblank_ticks > g_display_queue.last_vblank_ticks)
            g_display_queue.last_vblank_ticks = status.last_vblank_ticks;
        if (status.vblank_count > g_display_queue.vblank_count)
            g_display_queue.vblank_count = status.vblank_count;
        retire_completed_present_buffers();
    }
}

static uint32_t present_frame(const Surface *source, const DirtyRect *rects, int rect_count, uint32_t frame_sequence,
                              DisplayBufferHandle cursor_handle, int cursor_x, int cursor_y)
{
    rect_count = clamp_dirty_rect_count(rect_count);
    if (!source || !source->buffer || rect_count <= 0)
        return 0;
    Rect present_rects[MAX_DIRTY_RECTS];
    int present_count = 0;
    for (int i = 0; i < rect_count; i++) {
        DirtyRect clipped = rects[i];
        if (clip_dirty_rect_to_screen(clipped))
            present_rects[present_count++] = gui_rect_make(clipped.x, clipped.y, clipped.w, clipped.h);
    }
    if (present_count == 0)
        return 0;

    if (g_presentbuffer_handle != 0) {
        DisplayComposeLayer layer = {};
        layer.buffer_handle = g_presentbuffer_handle;
        layer.src_rect = gui_rect_make(0, 0, source->width, source->height);
        layer.dst_rect = layer.src_rect;
        layer.alpha = 255u;
        layer.flags = DISPLAY_COMPOSE_LAYER_OPAQUE;

        DisplayComposeRequest req = {};
        req.layers = &layer;
        req.layer_count = 1;
        req.damage_rects = present_rects;
        req.damage_rect_count = (uint32_t)present_count;
        req.frame_sequence = frame_sequence;
        req.flags = DISPLAY_PRESENT_VBLANK;
        req.cursor_buffer_handle = cursor_handle;
        req.cursor_x = cursor_x;
        req.cursor_y = cursor_y;
        return display_compose_submit(&req);
    }

    DisplayPresentRequest req = {};
    req.buffer = source->buffer;
    req.stride = source->pitch / 4;
    req.rects = present_rects;
    req.rect_count = (uint32_t)present_count;
    req.frame_sequence = frame_sequence;
    req.flags = DISPLAY_PRESENT_VBLANK;
    (void)cursor_handle;
    (void)cursor_x;
    (void)cursor_y;
    return display_present(&req);
}

static bool point_targets_window_client_for_input(const Window &w, int px, int py)
{
    return w.transparent ? point_hits_window_visible_pixel(w, px, py) : point_in_client(w, px, py);
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

static void invalidate_window_decoration_cache(Window &w)
{
    gui_destroy_surface(&w.decoration_cache);
    w.decoration_cache_alloc_w = 0;
    w.decoration_cache_alloc_h = 0;
    w.decoration_cache_w = 0;
    w.decoration_cache_h = 0;
    w.decoration_cache_theme_sig = 0;
    w.decoration_cache_focused = false;
    memset(w.decoration_cache_title, 0, sizeof(w.decoration_cache_title));

    gui_destroy_surface(&w.button_cache);
    w.button_cache_alloc_w = 0;
    w.button_cache_alloc_h = 0;
    w.button_cache_w = 0;
    w.button_cache_h = 0;
    w.button_cache_theme_sig = 0;
    w.button_cache_focused = false;
    w.button_cache_hovered_frame = false;
    w.button_cache_hovered_button = -1;
}

static void sync_window_runtime_metadata(Window &w, const WindowEntrySnapshot &entry)
{
    if (!w.entry)
        return;

    const int desired_buffer_w = entry.buffer_w > 0 ? entry.buffer_w : entry.w;
    const int desired_buffer_h = entry.buffer_h > 0 ? entry.buffer_h : entry.h;
    if (desired_buffer_w > 0 && desired_buffer_h > 0 &&
        (w.buffer_w != desired_buffer_w || w.buffer_h != desired_buffer_h)) {
        w.buffer_w = desired_buffer_w;
        w.buffer_h = desired_buffer_h;
        if (clamp_window_scroll(w) && w.entry) {
            w.entry->scroll_x = w.scroll_x;
            w.entry->scroll_y = w.scroll_y;
            asm volatile("sfence" ::: "memory");
        }
        w.needs_full_redraw = true;
        mark_window_frame_damage(w);
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
                asm volatile("sfence" ::: "memory");
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

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_screen = gui_init_framebuffer();
    if (!g_screen.buffer)
        return 1;
    if (display_get_caps(&g_display_caps) == 0)
        g_display_copy_path = (g_display_caps.flags & DISPLAY_FLAG_USES_COPY_PATH) != 0 &&
                              (g_display_caps.flags & DISPLAY_FLAG_HAS_PAGE_FLIP) == 0;

    int reg_shm = (int)syscall1(SYS_SHM_GET, (sizeof(Registry) + 0xFFFu) & ~0xFFFu);
    if (reg_shm < 0)
        return 1;
    uint64_t reg_ptr = syscall1(SYS_SHM_MAP, (uint64_t)reg_shm);
    if (reg_ptr == 0 || reg_ptr == (uint64_t)-1)
        return 1;

    Registry *registry = (Registry *)reg_ptr;
    memset(registry, 0, (sizeof(Registry) + 0xFFFu) & ~0xFFFu);
    registry->mb_shm_id = WIN_SHM_INVALID;
    registry->dk_shm_id = WIN_SHM_INVALID;
    registry->mb_blur_shm_id = WIN_SHM_INVALID;
    registry->dk_blur_shm_id = WIN_SHM_INVALID;
    registry->focused_window = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        memset(&registry->windows[i], 0, sizeof(WindowEntry));
        registry->windows[i].shm_id = WIN_SHM_INVALID;
        registry->windows[i].state = WIN_HIDDEN;
    }

    syscall1(SYS_GUI_REGISTER_WM, 0);
    RuntimeGuiSettings runtime_settings = load_runtime_settings();
    g_system_flags = runtime_settings.system_flags;
    gui_apply_theme(runtime_settings.theme_mode);

    g_backbuffer = gui_create_surface(g_screen.width, g_screen.height);
    if (!g_backbuffer.buffer)
        return 1;

    if ((g_display_caps.flags & DISPLAY_FLAG_HAS_COMPOSITOR) != 0) {
        uint32_t requested_present_slots = g_display_copy_path ? 1u : MAX_PRESENT_BUFFER_SLOTS;
        for (uint32_t i = 0; i < requested_present_slots; i++) {
            DisplayBufferCreate create = {};
            create.width = g_screen.width;
            create.height = g_screen.height;
            create.pixel_format = DISPLAY_PIXEL_FORMAT_XRGB8888;
            create.flags =
                DISPLAY_BUFFER_FLAG_CPU_VISIBLE | DISPLAY_BUFFER_FLAG_LINEAR | DISPLAY_BUFFER_FLAG_RENDER_TARGET;
            if (!g_display_copy_path)
                create.flags |= DISPLAY_BUFFER_FLAG_SCANOUT;
            if (display_buffer_create(&create) != 0)
                break;

            DisplayBufferMap map = {};
            map.handle = create.handle;
            if (display_buffer_map(&map) != 0 || map.address == 0 || map.stride < g_screen.width) {
                display_buffer_destroy(create.handle);
                break;
            }

            PresentBufferSlot &slot = g_presentbuffer_slots[g_presentbuffer_slot_count++];
            memset(&slot, 0, sizeof(PresentBufferSlot));
            slot.surface.width = g_screen.width;
            slot.surface.height = g_screen.height;
            slot.surface.pitch = map.stride * 4u;
            slot.surface.buffer = (uint32_t *)map.address;
            slot.handle = create.handle;
            slot.in_flight_sequence = 0;
        }
    }
    if (g_presentbuffer_slot_count == 0) {
        PresentBufferSlot &slot = g_presentbuffer_slots[0];
        memset(&slot, 0, sizeof(PresentBufferSlot));
        slot.surface.width = g_screen.width;
        slot.surface.height = g_screen.height;
        slot.surface.pitch = g_screen.pitch;
        slot.surface.buffer = (uint32_t *)malloc((size_t)g_screen.pitch * (size_t)g_screen.height);
        slot.handle = 0;
        slot.in_flight_sequence = 0;
        if (slot.surface.buffer)
            g_presentbuffer_slot_count = 1;
    }
    if (g_presentbuffer_slot_count == 0)
        return 1;
    sync_presentbuffer_alias_from_active_slot();
    if (!g_presentbuffer.buffer)
        return 1;
    init_wallpaper();

    uint32_t dock_w = (uint32_t)shell_dock_window_w(SHELL_DOCK_ITEM_COUNT);
    uint32_t dock_h = (uint32_t)shell_dock_window_h();
    int menubar_h = wm_menubar_h();
    int mb_shm = syscall1(SYS_SHM_GET, g_screen.width * gui_system_menubar_canvas_h() * 4);
    int dk_shm = syscall1(SYS_SHM_GET, dock_w * dock_h * 4);
    int mb_blur_shm = syscall1(SYS_SHM_GET, g_screen.width * (uint32_t)menubar_h * 4);
    int dk_blur_shm = syscall1(SYS_SHM_GET, dock_w * dock_h * 4);

    registry->mb_shm_id = mb_shm;
    registry->dk_shm_id = dk_shm;
    registry->mb_blur_shm_id = mb_blur_shm;
    registry->dk_blur_shm_id = dk_blur_shm;
    registry->theme_mode = runtime_settings.theme_mode;
    registry->system_flags = runtime_settings.system_flags;
    registry->storage_mode = get_storage_mode();
    registry->storage_request_mode = registry->storage_mode;
    registry->wallpaper_status = WALLPAPER_STATUS_SOLID;
    registry->window_count = 2;

    WindowEntry &we0 = registry->windows[0];
    memset(&we0, 0, sizeof(WindowEntry));
    we0.shm_id = mb_shm;
    we0.x = 0;
    we0.y = 0;
    we0.w = (int)g_screen.width;
    we0.h = menubar_h;
    we0.restore_x = we0.x;
    we0.restore_y = we0.y;
    we0.restore_w = we0.w;
    we0.restore_h = we0.h;
    we0.buffer_w = we0.w;
    we0.buffer_h = gui_system_menubar_canvas_h();
    we0.min_w = we0.w;
    we0.min_h = we0.h;
    we0.flags = WIN_FLAG_TRANSPARENT | WIN_FLAG_SYSTEM;
    we0.state = WIN_NORMAL;
    we0.active = true;
    we0.ready = true;
    strncpy(we0.title, "Menubar", 63);
    damage_reset(&we0.damage);

    WindowEntry &we1 = registry->windows[1];
    memset(&we1, 0, sizeof(WindowEntry));
    we1.shm_id = dk_shm;
    we1.x = (int)(g_screen.width - dock_w) / 2;
    we1.y = (int)(g_screen.height - dock_h - shell_dock_bottom_inset());
    we1.w = (int)dock_w;
    we1.h = (int)dock_h;
    we1.restore_x = we1.x;
    we1.restore_y = we1.y;
    we1.restore_w = we1.w;
    we1.restore_h = we1.h;
    we1.buffer_w = we1.w;
    we1.buffer_h = we1.h;
    we1.min_w = we1.w;
    we1.min_h = we1.h;
    we1.flags = WIN_FLAG_TRANSPARENT | WIN_FLAG_SYSTEM;
    we1.state = WIN_NORMAL;
    we1.active = true;
    we1.ready = true;
    strncpy(we1.title, "Dock", 63);
    damage_reset(&we1.damage);

    add_win_internal(mb_shm, 0, 0, g_screen.width, menubar_h, "Menubar", &registry->windows[0].damage,
                     &registry->windows[0], true);
    add_win_internal(dk_shm, (g_screen.width - dock_w) / 2, g_screen.height - dock_h - shell_dock_bottom_inset(),
                     dock_w, dock_h, "Dock", &registry->windows[1].damage, &registry->windows[1], true);

    syscall1(SYS_SET_QUIET, 1);
    asm volatile("sfence" ::: "memory");

    if (!init_shell_blur_buffers(registry, dock_w, dock_h)) {
        registry->mb_blur_generation = 0;
        registry->dk_blur_generation = 0;
    }

    g_input.mouse_x = g_screen.width / 2;
    g_input.mouse_y = g_screen.height / 2;
    reload_wallpaper(registry, false);
    sync_storage_prompt_state(false);
    ensure_default_user_storage_layout();
    gui_blit_rect(&g_backbuffer, &g_wallpaper, 0, 0, 0, 0, g_screen.width, g_screen.height);
    capture_shell_backdrop_for_rect({0, 0, (int)g_screen.width, (int)g_screen.height}, registry);
    flush_shell_blur_updates(registry);
    select_presentbuffer_slot_for_frame();
    gui_blit_rect(&g_presentbuffer, &g_backbuffer, 0, 0, 0, 0, g_screen.width, g_screen.height);
    gui_draw_cursor_kind(&g_presentbuffer, g_input.mouse_x, g_input.mouse_y, g_input.cursor_kind);

    DirtyRect init_pres = {0, 0, (int)g_screen.width, (int)g_screen.height};
    uint32_t last_seq = present_frame(&g_presentbuffer, &init_pres, 1, 1, 0, 0, 0);
    if (last_seq && g_presentbuffer_slot_count)
        g_presentbuffer_slots[g_presentbuffer_active_slot].in_flight_sequence = last_seq;
    mark_other_presentbuffer_slots_stale(&init_pres, 1, g_presentbuffer_active_slot);
    uint32_t frame_seq = (last_seq ? last_seq : 1) + 1;
    asm volatile("sfence" ::: "memory");
    registry->magic = REGISTRY_MAGIC;
    asm volatile("sfence" ::: "memory");
    LOG_INFO("wm", "first desktop frame submitted at %llu ms", (unsigned long long)get_ticks());

    uint32_t last_settings_gen = registry->settings_generation, last_storage_gen = registry->storage_request_generation;
    Event ev;

    while (true) {
        while (get_event(&ev)) {
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

            if ((ev.type == EVT_MOUSE_DOWN || ev.type == EVT_MOUSE_UP) && g_input.mouse_y >= registry->windows[0].h) {
                registry->mb_menu_dismiss_requested = true;
                asm volatile("sfence" ::: "memory");
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
                        registry->mb_click_x = g_input.mouse_x;
                        registry->mb_click_y = g_input.mouse_y;
                        registry->mb_clicked = true;
                    } else if (sys_hit == 1) {
                        registry->dk_click_x = g_input.mouse_x;
                        registry->dk_click_y = g_input.mouse_y;
                        registry->dk_clicked = true;
                    }
                    asm volatile("sfence" ::: "memory");
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
                    if (hit_idx >= 2 && fwd_client)
                        post_mouse_event_to_window(g_windows[focus_window(hit_idx, true)], EVT_MOUSE_DOWN,
                                                   g_input.mouse_x, g_input.mouse_y, ev.mouse.button);
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
                    for (int i = g_window_count - 1; i >= 2; i--) {
                        if (!is_window_visible(g_windows[i]))
                            continue;
                        if (g_windows[i].transparent) {
                            if (!point_targets_window_client_for_input(g_windows[i], g_input.mouse_x, g_input.mouse_y))
                                continue;
                            if (is_user_window(g_windows[i])) {
                                post_mouse_event_to_window(g_windows[focus_window(i, true)], EVT_MOUSE_DOWN,
                                                           g_input.mouse_x, g_input.mouse_y, ev.mouse.button);
                                opened = true;
                            }
                            break;
                        }
                        if (!point_in_outer(g_windows[i], g_input.mouse_x, g_input.mouse_y))
                            continue;
                        if (point_in_client(g_windows[i], g_input.mouse_x, g_input.mouse_y)) {
                            if (is_user_window(g_windows[i])) {
                                post_mouse_event_to_window(g_windows[focus_window(i, true)], EVT_MOUSE_DOWN,
                                                           g_input.mouse_x, g_input.mouse_y, ev.mouse.button);
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
                mark_cursor_transition_damage(g_input.mouse_x, g_input.mouse_y, g_input.cursor_kind, g_input.mouse_x,
                                              g_input.mouse_y, g_input.cursor_kind);
                if (g_input.hover_frame_index >= 2 && g_input.hover_button >= 0) {
                    DirtyRect outer = window_outer_bounds(g_windows[g_input.hover_frame_index]);
                    int title_h = wm_title_bar_h() + wm_frame_border() + wm_frame_shadow_offset_y();
                    if (title_h > outer.h)
                        title_h = outer.h;
                    enqueue_damage_rect(outer.x, outer.y, outer.w, title_h);
                }
                if (c_idx < 2) {
                    int focus = find_registry_focused_user_window(registry);
                    if (focus >= 2 && !pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y) &&
                        point_targets_window_client_for_input(g_windows[focus], g_input.mouse_x, g_input.mouse_y))
                        post_mouse_event_to_window(g_windows[focus], EVT_MOUSE_UP, g_input.mouse_x, g_input.mouse_y,
                                                   ev.mouse.button);
                }
            } else if (ev.type == EVT_MOUSE_SCROLL) {
                if (g_input.pointer_down || g_storage_prompt.visible || g_context_menu.open)
                    continue;
                int tgt = -1;
                for (int i = g_window_count - 1; i >= 2; i--)
                    if (is_window_visible(g_windows[i]) &&
                        point_targets_window_client_for_input(g_windows[i], g_input.mouse_x, g_input.mouse_y)) {
                        tgt = i;
                        break;
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
                    for (int i = 2; i < g_window_count; i++)
                        if (is_window_visible(g_windows[i])) {
                            reorder = bring_window_to_front(i);
                            break;
                        }
                }
                if (reorder >= 2) {
                    g_input.hover_frame_index = -1;
                    g_input.hover_resize_edges = RESIZE_NONE;
                    g_input.hover_button = -1;
                    invalidate_window_visibility_cache();
                    enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
                    focus_window(find_top_visible_user_window(), false);
                }
            } else if (ev.type == EVT_MOUSE_DOWN || ev.type == EVT_MOUSE_UP) {
                int focus = find_registry_focused_user_window(registry);
                if (focus >= 2 && !pointer_blocked_by_shell_overlay(g_input.mouse_x, g_input.mouse_y) &&
                    point_targets_window_client_for_input(g_windows[focus], g_input.mouse_x, g_input.mouse_y))
                    post_mouse_event_to_window(g_windows[focus], ev.type, g_input.mouse_x, g_input.mouse_y,
                                               ev.mouse.button);
            }
        }

        if (g_input.have_pending_move) {
            apply_mouse_move(registry, g_input.pending_mouse_x, g_input.pending_mouse_y);
            g_input.have_pending_move = false;
        }
        drain_display_events();

        if (registry->settings_generation != last_settings_gen) {
            last_settings_gen = registry->settings_generation;
            g_system_flags = registry->system_flags;
            gui_apply_theme(registry->theme_mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK);
            reload_wallpaper(registry, true);
            enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
        }

        if (registry->storage_request_generation != last_storage_gen) {
            last_storage_gen = registry->storage_request_generation;
            if (!apply_storage_mode_request(registry, registry->storage_request_mode)) {
                registry->storage_mode = get_storage_mode();
                asm volatile("sfence" ::: "memory");
            }
        }

        if (registry->storage_mode != (uint32_t)get_storage_mode()) {
            registry->storage_mode = get_storage_mode();
            asm volatile("sfence" ::: "memory");
        }
        if (registry->wallpaper_reload_requested) {
            registry->wallpaper_reload_requested = false;
            reload_wallpaper(registry, true);
            enqueue_damage_rect(0, 0, g_screen.width, g_screen.height);
        }

        registry->mouse_x = g_input.mouse_x;
        registry->mouse_y = g_input.mouse_y;
        update_hover_feedback();
        update_cursor_kind();
        if (g_context_menu.open)
            update_context_menu_hover(registry, g_input.mouse_x, g_input.mouse_y);

        if (registry->window_count > 2) {
            for (uint32_t i = 2; i < (registry->window_count > MAX_WINDOWS ? MAX_WINDOWS : registry->window_count);
                 i++) {
                WindowEntry &e = registry->windows[i];
                if (!e.ready || !gui_shm_id_is_valid(e.shm_id) || e.w <= 0 || e.h <= 0 || !e.owner_pid || !e.title[0])
                    continue;
                if (find_window_by_entry(&e) >= 0 || find_window_by_shm(e.shm_id) >= 0)
                    continue;
                add_win_internal(e.shm_id, e.x, e.y, e.w, e.h, e.title, &e.damage, &e,
                                 (e.flags & WIN_FLAG_TRANSPARENT));
            }
        }

        for (int i = 0; i < g_window_count; i++) {
            Window &w = g_windows[i];
            if (w.entry) {
                if (w.entry->request_close) {
                    w.entry->request_close = false;
                    close_window(i);
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
                    w.needs_full_redraw = true;
                    continue;
                }
                sync_window_runtime_metadata(w, entry_snapshot);
                int nx = entry_snapshot.x, ny = entry_snapshot.y, nw = entry_snapshot.w, nh = entry_snapshot.h;
                // Clamp snapshots to sanity
                if (nw > (int)g_screen.width * 2) nw = (int)g_screen.width * 2;
                if (nh > (int)g_screen.height * 2) nh = (int)g_screen.height * 2;
                if (gui_shm_id_is_valid(entry_snapshot.shm_id) && entry_snapshot.shm_id != w.shm_id) {
                    uint64_t m = syscall1(SYS_SHM_MAP, entry_snapshot.shm_id);
                    if (m != 0 && m != (uint64_t)-1) {
                        int old = w.shm_id;
                        w.shm_id = entry_snapshot.shm_id;
                        w.buffer = (uint32_t *)m;
                        if (gui_shm_id_is_valid(old))
                            syscall1(SYS_SHM_UNMAP, old);
                        sync_window_runtime_metadata(w, entry_snapshot);
                        w.entry->buffer_ack_generation = entry_snapshot.buffer_generation;
                        w.buffer_generation_acked = entry_snapshot.buffer_generation;
                        w.buffer_generation_seen = entry_snapshot.buffer_generation;
                        asm volatile("sfence" ::: "memory");
                        w.needs_full_redraw = true;
                        mark_window_frame_damage(w);
                        invalidate_window_visibility_cache();
                    }
                }
                w.min_w = entry_snapshot.min_w > 0 ? entry_snapshot.min_w : 0;
                w.min_h = entry_snapshot.min_h > 0 ? entry_snapshot.min_h : 0;
                if (nw > 0 && nh > 0 && (w.x != nx || w.y != ny || w.w != nw || w.h != nh)) {
                    Window old = w;
                    w.x = nx;
                    w.y = ny;
                    w.w = nw;
                    w.h = nh;
                    w.needs_full_redraw = (old.w != nw) || (old.h != nh);
                    if (clamp_window_scroll(w) && w.entry) {
                        w.entry->scroll_x = w.scroll_x;
                        w.entry->scroll_y = w.scroll_y;
                        asm volatile("sfence" ::: "memory");
                    }
                    mark_window_transition_damage(old, w);
                    invalidate_window_visibility_cache();
                }
                if (entry_snapshot.owner_pid)
                    w.owner_pid = entry_snapshot.owner_pid;
                w.buffer_generation_seen = entry_snapshot.buffer_generation;
                w.buffer_generation_acked = entry_snapshot.buffer_ack_generation;
                if (w.resize_configure_pending && w.buffer_w >= w.w && w.buffer_h >= w.h) {
                    w.resize_configure_pending = false;
                    w.last_configure_ticks = 0;
                    w.last_commit_ticks = get_ticks();
                } else if (w.resize_configure_pending && w.owner_pid && (w.buffer_w < w.w || w.buffer_h < w.h)) {
                    uint64_t now = get_ticks();
                    if (w.last_configure_ticks == 0 || now - w.last_configure_ticks >= WM_RESIZE_CONFIGURE_RETRY_TICKS)
                        post_window_resize_configure(w);
                }
            }
            if (w.damage_ptr) {
                Rect d = {};
                while (damage_pop_rect(w.damage_ptr, &d)) {
                    if (!w.first_damage_received) {
                        w.first_damage_received = true;
                        w.needs_full_redraw = true;
                    }
                    if (is_window_visible(w)) {
                        DirtyRect damaged = {w.x + d.x - w.scroll_x, w.y + d.y - w.scroll_y, d.w, d.h};
                        DirtyRect client = window_visible_client_bounds(w);
                        DirtyRect visible = {};
                        if (rect_intersection(damaged, client, &visible))
                            enqueue_damage_rect(visible.x, visible.y, visible.w, visible.h);
                    }
                    w.active = true;
                    if (w.entry)
                        w.entry->active = true;
                }

                // Startup watchdog: if no damage received for a while, force a redraw anyway
                if (!w.first_damage_received && (get_ticks() - w.last_commit_ticks > 90)) {
                    w.first_damage_received = true;
                    w.needs_full_redraw = true;
                }
            }
        }

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

        bool manip = g_input.pointer_down && g_input.drag_index >= 2;
        bool inter = manip || g_input.hover_resize_edges != RESIZE_NONE || g_input.hover_button >= 0 ||
                     g_context_menu.open || g_storage_prompt.visible;
        bool resizing = manip && g_input.drag_edges != RESIZE_NONE;
        uint32_t limit = g_display_copy_path ? 1u : MAX_PENDING_PRESENTS;

        if (!g_dirty_frame_ready && g_dirty_count > 0) {
            if (g_window_visibility_cache_dirty) {
                refresh_window_cache();
                refresh_window_visible_regions();
                g_window_visibility_cache_dirty = false;
            }
            normalize_dirty_rects(inter);
            if (resizing && g_dirty_count > 1)
                collapse_dirty_rects_to_bounds();
        }

        if (!g_dirty_frame_ready && g_dirty_count > 0) {
            uint32_t build_pending = wm::pending_presents(last_seq, g_display_queue.completed_sequence);
            if (build_pending) {
                refresh_display_queue_from_status();
                build_pending = wm::pending_presents(last_seq, g_display_queue.completed_sequence);
            }
            wm::PresentPolicyDecision build_action = wm::choose_present_policy(
                {build_pending, limit, (g_display_caps.flags & DISPLAY_FLAG_STRICT_SYNC_ONLY) != 0, inter,
                 g_display_copy_path, manip});
            if (build_action == wm::PresentPolicyDecision::Skip) {
                g_frame_stats.frames_skipped++;
                yield();
                continue;
            }

            DirtyRect cursor_rect = {};
            bool draw_cursor = prepare_cursor_overlay_damage(inter, &cursor_rect);
            g_frame_cursor_handle = 0;
            g_frame_cursor_x = 0;
            g_frame_cursor_y = 0;
            bool draw_software_cursor = draw_cursor;
            if (draw_cursor && cursor_backend_allowed()) {
                CursorPresentBuffer *cursor_buffer = ensure_cursor_present_buffer(g_input.cursor_kind);
                if (cursor_buffer && cursor_buffer->handle != 0) {
                    g_frame_cursor_handle = cursor_buffer->handle;
                    g_frame_cursor_x = cursor_rect.x;
                    g_frame_cursor_y = cursor_rect.y;
                    draw_software_cursor = false;
                } else {
                    g_cursor_backend_disabled = true;
                }
            }
            if (select_presentbuffer_slot_for_frame()) {
                int focus = find_registry_focused_user_window(registry);
                int dirty_count = clamp_dirty_rect_count(g_dirty_count);
                for (int d = 0; d < dirty_count; d++) {
                    DirtyRect &r = g_dirty_rects[d];
                    if (!clip_dirty_rect_to_screen(r))
                        continue;
                    if (!compose_rect_clipped(r, focus, g_input.hover_frame_index, g_input.hover_button, registry))
                        compose_rect_unclipped(r, focus, g_input.hover_frame_index, g_input.hover_button, registry);
                    gui_blit_rect(&g_presentbuffer, &g_backbuffer, r.x, r.y, r.x, r.y, r.w, r.h);
                }
                flush_shell_blur_updates(registry);
                if (draw_cursor) {
                    gui_blit_rect(&g_presentbuffer, &g_backbuffer, cursor_rect.x, cursor_rect.y, cursor_rect.x,
                                  cursor_rect.y, cursor_rect.w, cursor_rect.h);
                    if (draw_software_cursor) {
                        gui_draw_cursor_kind(&g_presentbuffer, g_input.mouse_x, g_input.mouse_y, g_input.cursor_kind);
                        g_frame_stats.cursor_software_frames++;
                    } else {
                        g_frame_stats.cursor_backend_frames++;
                    }
                }
                wm_stats_note_dirty_set(g_dirty_rects, g_dirty_count);
                g_frame_stats.frames_built++;
                g_dirty_frame_ready = true;
            }
        }

        uint32_t pending = wm::pending_presents(last_seq, g_display_queue.completed_sequence);
        if (pending) {
            refresh_display_queue_from_status();
            pending = wm::pending_presents(last_seq, g_display_queue.completed_sequence);
        }

        wm::PresentPolicyDecision action =
            wm::choose_present_policy({pending, limit, (g_display_caps.flags & DISPLAY_FLAG_STRICT_SYNC_ONLY) != 0,
                                       inter, g_display_copy_path, manip});

        if (g_dirty_frame_ready && action == wm::PresentPolicyDecision::Submit) {
            uint32_t sub =
                present_frame(&g_presentbuffer, g_dirty_rects, clamp_dirty_rect_count(g_dirty_count), frame_seq,
                              g_frame_cursor_handle, g_frame_cursor_x, g_frame_cursor_y);
            if (sub) {
                if (g_presentbuffer_slot_count)
                    g_presentbuffer_slots[g_presentbuffer_active_slot].in_flight_sequence = sub;
                mark_other_presentbuffer_slots_stale(g_dirty_rects, clamp_dirty_rect_count(g_dirty_count),
                                                     g_presentbuffer_active_slot);
                g_frame_stats.frames_submitted++;
                last_seq = sub;
                frame_seq = sub + 1;
                g_dirty_count = 0;
                g_dirty_frame_ready = false;
            }
        } else if (!g_dirty_frame_ready || action == wm::PresentPolicyDecision::Skip) {
            if (action == wm::PresentPolicyDecision::Skip)
                g_frame_stats.frames_skipped++;
            yield();
        } else {
            uint32_t tgt = wm::completion_target_for_available_slot(last_seq, limit);
            while (g_display_queue.completed_sequence < tgt) {
                DisplayEvent ev_w = {};
                if (display_wait_event(&ev_w) != 0) {
                    display_wait();
                    refresh_display_queue_from_status();
                    drain_display_events();
                    break;
                }
                apply_display_event(ev_w);
                drain_display_events();
            }
        }
    }
    return 0;
}
