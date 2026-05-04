#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/gui.h>
#include <uapi/signal.h>
#include <uapi/syscalls.h>
#include <unistd.h>
#include <wm/interaction_policy.h>

#include "../libc/config_utils.h"
#include "../libc/log.h"
#include "../libc/syscall.h"
#include "../libc/wallpaper_defaults.h"
#include "../libgui/gui.h"
#include "../libgui/gui_canvas_utils.h"
#include "../shell_layout.h"

#define MAX_WINDOWS 32
#define MAX_DIRTY_RECTS 128
#define MAX_VISIBLE_REGIONS 256
#define CURSOR_WIDTH 16
#define CURSOR_HEIGHT 16
#define CURSOR_DAMAGE_PAD 4
static constexpr int WINDOW_DAMAGE_PAD_BASE = 3;

static constexpr int FRAME_BORDER = 1;
static constexpr int FRAME_OCCLUSION_INSET = 4;
static constexpr int RESIZE_GRIP = 9;
static constexpr int BTN_SIZE = 12;
static constexpr int BTN_INSET_X = 10;
static constexpr int BTN_INSET_Y = 0;
static constexpr int BTN_SPACING = 20;
static constexpr int MIN_WINDOW_W = 180;
static constexpr int MIN_WINDOW_H = 120;
static constexpr int DESKTOP_MARGIN = 6;
static constexpr int DOCK_RESERVED_H = 72;
static constexpr int CURSOR_MAX_SIZE = 64;
static constexpr const char *WALLPAPER_CONFIG_PATH = "/data/WALLPAPR.CFG";
static constexpr const char *WALLPAPER_BOOTSTRAP_CONFIG_PATH = "/etc/wallpaper.conf";
static constexpr const char *SYSTEM_CONFIG_PATH = "/data/SYSTEM.CFG";
static constexpr const char *SYSTEM_BOOTSTRAP_CONFIG_PATH = "/etc/system.conf";
static constexpr uint32_t MAX_PENDING_PRESENTS = 2;
static constexpr uint32_t MAX_PRESENT_BUFFER_SLOTS = MAX_PENDING_PRESENTS + 1u;
static constexpr uint32_t DIRTY_COLLAPSE_RATIO_NUM = 5;
static constexpr uint32_t DIRTY_COLLAPSE_RATIO_DEN = 4;
static constexpr uint32_t WM_FRAME_STATS_HISTORY = 120;
static constexpr uint32_t WM_TARGET_FPS = 60;
static constexpr uint32_t WM_FRAME_TICKS = 1000 / WM_TARGET_FPS;
static constexpr int WM_SNAP_THRESHOLD_BASE = 14;
static constexpr int WM_SNAP_ESCAPE_BASE = 28;
static constexpr uint64_t WM_RESIZE_CONFIGURE_RETRY_TICKS = 8;
static constexpr int INDEX_MAX_RESULTS = 10;

static constexpr uint32_t GUI_ROUNDED_EDGE_TOP = 1u;
static constexpr uint32_t GUI_ROUNDED_EDGE_BOTTOM = 2u;
static constexpr uint32_t GUI_ROUNDED_EDGE_ALL = GUI_ROUNDED_EDGE_TOP | GUI_ROUNDED_EDGE_BOTTOM;

enum ResizeEdge
{
    RESIZE_NONE = 0,
    RESIZE_LEFT = 1 << 0,
    RESIZE_RIGHT = 1 << 1,
    RESIZE_TOP = 1 << 2,
    RESIZE_BOTTOM = 1 << 3,
};

enum ContextMenuKind
{
    CONTEXT_MENU_NONE = 0,
    CONTEXT_MENU_DESKTOP,
    CONTEXT_MENU_WINDOW,
};

struct DirtyRect
{
    int x, y, w, h;
};

struct Window
{
    int shm_id;
    uint32_t *buffer;
    uint32_t owner_pid;
    int x, y, w, h;
    int target_x, target_y, target_w, target_h;
    int buffer_w, buffer_h;
    int content_w, content_h;
    int scroll_x, scroll_y;
    int min_w, min_h;
    bool active;
    bool transparent;
    bool needs_full_redraw;
    Damage *damage_ptr;
    WindowEntry *entry;
    Surface decoration_cache;
    int decoration_cache_alloc_w;
    int decoration_cache_alloc_h;
    int decoration_cache_w;
    int decoration_cache_h;
    uint32_t decoration_cache_theme_sig;
    bool decoration_cache_focused;
    char decoration_cache_title[64];

    Surface button_cache;
    int button_cache_alloc_w;
    int button_cache_alloc_h;
    int button_cache_w;
    int button_cache_h;
    uint32_t button_cache_theme_sig;
    bool button_cache_focused;
    bool button_cache_hovered_frame;
    int button_cache_hovered_button;

    // Compositor-side commit/configure bookkeeping. These fields are local WM
    // state, not ABI. They let the WM distinguish a stable committed surface
    // generation from transient registry mutations during resize or buffer swaps.
    uint32_t buffer_generation_seen;
    uint32_t buffer_generation_acked;
    uint32_t configure_serial;
    uint32_t pending_configure_serial;
    uint64_t last_commit_ticks;
    uint64_t last_configure_ticks;
    bool resize_configure_pending;
    bool first_damage_received;

    char title[64];
};

struct PresentBufferSlot
{
    Surface surface;
    DisplayBufferHandle handle;
    uint32_t in_flight_sequence;
    DirtyRect stale_rects[MAX_DIRTY_RECTS];
    int stale_count;
};

struct DisplayQueueState
{
    uint32_t completed_sequence;
    uint64_t last_vblank_ticks;
    uint64_t vblank_count;
};

struct WmFrameStats
{
    uint64_t frames_built;
    uint64_t frames_submitted;
    uint64_t frames_skipped;
    uint64_t full_repaints;
    uint64_t clipped_repaints;
    uint64_t stale_slot_repairs;
    uint64_t cursor_backend_frames;
    uint64_t cursor_software_frames;
    uint64_t dirty_area_accum;
    uint32_t last_dirty_rects;
    uint64_t last_dirty_area;
    uint32_t max_dirty_rects;
    uint64_t max_dirty_area;
    uint32_t present_queue_depth;

    // Timing (ticks)
    uint64_t last_build_ticks;
    uint64_t last_submit_ticks;
    uint64_t last_compose_ticks;
    uint64_t last_copy_ticks;
    uint64_t last_blur_ticks;

    // Missed frames
    uint64_t missed_frames;
    uint64_t last_vblank_count;

    // Alpha count
    uint64_t alpha_pixels_last_frame;
    uint64_t damage_collapse_count;
};

struct RuntimeGuiSettings
{
    GuiThemeMode theme_mode;
    uint32_t system_flags;
    bool ethernet_enabled;
    bool ethernet_use_dhcp;
    bool animations_enabled;
    uint32_t transparency_level;
    uint32_t volume_level;
};

struct ContextMenuState
{
    bool open;
    ContextMenuKind kind;
    int target_index;
    WindowEntry *target_entry;
    int x, y, w, h;
    int hovered_index;
};

struct StoragePromptState
{
    bool visible;
    bool dismissed;
    int hovered_button;
};

struct StoragePromptLayout
{
    DirtyRect box;
    DirtyRect off_button;
    DirtyRect readonly_button;
    DirtyRect writable_button;
};

enum IndexActionKind
{
    INDEX_ACTION_NONE = 0,
    INDEX_ACTION_LAUNCH_APP,
    INDEX_ACTION_OPEN_CONTROL_PANEL,
    INDEX_ACTION_OPEN_STORAGE_PROMPT,
    INDEX_ACTION_SHOW_DESKTOP,
    INDEX_ACTION_TOGGLE_THEME,
    INDEX_ACTION_TOGGLE_DESKTOP_GRID,
    INDEX_ACTION_TOGGLE_CLOCK_SECONDS,
    INDEX_ACTION_TOGGLE_ANIMATIONS,
    INDEX_ACTION_TOGGLE_TRANSPARENCY,
    INDEX_ACTION_TORTURE_TEST,
};

enum ControlPanelItem
{
    CONTROL_ITEM_NONE = -1,
    CONTROL_ITEM_NETWORK = 0,
    CONTROL_ITEM_DARK_MODE,
    CONTROL_ITEM_DESKTOP_GRID,
    CONTROL_ITEM_CLOCK_SECONDS,
    CONTROL_ITEM_ANIMATIONS,
    CONTROL_ITEM_TRANSPARENCY,
    CONTROL_ITEM_VOLUME,
    CONTROL_ITEM_STORAGE,
    CONTROL_ITEM_SETTINGS,
};

struct IndexResult
{
    char title[64];
    char path[128];
    char detail[96];
    bool is_app;
    IndexActionKind action;
    int score;
};

struct IndexState
{
    bool active;
    char query[64];
    int query_len;
    IndexResult results[INDEX_MAX_RESULTS];
    int result_count;
    int selected_index;
    int hovered_index;
    uint64_t open_ticks;
};

struct ControlCenterState
{
    bool open;
    int hovered_item;
    uint32_t volume;
    bool network_enabled;
    bool dark_mode;
    bool desktop_grid;
    bool clock_seconds;
    bool animations_enabled;
    uint32_t transparency_level;
    bool volume_dragging;
};

struct WmInputState
{
    int mouse_x = 0;
    int mouse_y = 0;
    int old_mouse_x = 0;
    int old_mouse_y = 0;
    bool pointer_down = false;
    int drag_index = -1;
    int drag_edges = RESIZE_NONE;
    int hover_frame_index = -1;
    int hover_resize_edges = RESIZE_NONE;
    int hover_button = -1;
    GuiCursorKind cursor_kind = GUI_CURSOR_ARROW;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    int drag_origin_mouse_x = 0;
    int drag_origin_mouse_y = 0;
    Window drag_origin = {};
    bool have_pending_move = false;
    int pending_mouse_x = 0;
    int pending_mouse_y = 0;
    int snap_edges = RESIZE_NONE;
    DirtyRect snap_preview = {};
};

extern Surface g_screen;
extern Surface g_backbuffer;
extern Surface g_presentbuffer;
extern Surface g_wallpaper;
extern Surface g_menubar_blur;
extern Surface g_dock_blur;
extern Surface g_menubar_blur_source;
extern Surface g_dock_blur_source;
extern DisplayCaps g_display_caps;
extern bool g_display_copy_path;

extern DisplayBufferHandle g_presentbuffer_handle;
extern PresentBufferSlot g_presentbuffer_slots[MAX_PRESENT_BUFFER_SLOTS];
extern uint32_t g_presentbuffer_slot_count;
extern uint32_t g_presentbuffer_active_slot;
extern DisplayQueueState g_display_queue;
extern WmFrameStats g_frame_stats;

extern Window g_windows[MAX_WINDOWS];
extern int g_window_count;
extern int g_add_fail_logs;
extern uint32_t g_system_flags;

extern DirtyRect g_dirty_rects[MAX_DIRTY_RECTS];
extern int g_dirty_count;
extern DirtyRect g_window_outer_cache[MAX_WINDOWS];
extern DirtyRect g_window_client_cache[MAX_WINDOWS];
extern bool g_window_visible_cache[MAX_WINDOWS];
extern DirtyRect g_window_visible_regions[MAX_WINDOWS][MAX_VISIBLE_REGIONS];
extern int g_window_visible_region_count[MAX_WINDOWS];
extern bool g_window_visible_region_overflow;

extern IndexState g_index;
extern ControlCenterState g_control_center;

// Overlays
DirtyRect index_overlay_bounds();
DirtyRect control_center_bounds();
void open_index();
void close_index();
void update_index_search();
bool activate_index_selection(Registry *registry);
bool handle_index_pointer_down(Registry *registry, int mouse_x, int mouse_y);
void update_index_hover(int mouse_x, int mouse_y);
void toggle_control_center();
void close_control_center();
bool handle_control_center_pointer_down(Registry *registry, int mouse_x, int mouse_y);
void handle_control_center_pointer_up();
void update_control_center_hover(int mouse_x, int mouse_y);
bool update_control_center_drag(int mouse_x, int mouse_y);
bool handle_control_center_scroll(Registry *registry, int mouse_x, int mouse_y, int scroll_y);
void sync_control_center_state_from_registry(const Registry *registry);

void draw_index_overlay_clipped(const DirtyRect &clip, const Registry *registry);
void draw_control_center_overlay_clipped(const DirtyRect &clip);

// Special key codes (matching ps2_keyboard.cpp)
#define KEY_UP_ARROW 0x80
#define KEY_DOWN_ARROW 0x81
#define KEY_LEFT_ARROW 0x82
#define KEY_RIGHT_ARROW 0x83

void persist_wm_settings();
void load_wm_settings();
void enqueue_damage_rect(int x, int y, int w, int h);
extern bool g_window_visibility_cache_dirty;
extern bool g_dirty_frame_ready;

extern ContextMenuState g_context_menu;
extern StoragePromptState g_storage_prompt;
extern WmInputState g_input;

static inline float wm_fabsf(float x)
{
    return x < 0.0f ? -x : x;
}

static inline float wm_sqrtf(float n)
{
    if (n <= 0.0f)
        return 0.0f;
    float x = n, y = 1.0f, e = 0.000001f;
    for (int i = 0; i < 15; i++) {
        x = (x + y) * 0.5f;
        y = n / x;
        if (wm_fabsf(x - y) <= e)
            break;
    }
    return x;
}

static inline int64_t rect_right_i64(const DirtyRect &rect)
{
    return (int64_t)rect.x + (int64_t)rect.w;
}
static inline int64_t rect_bottom_i64(const DirtyRect &rect)
{
    return (int64_t)rect.y + (int64_t)rect.h;
}
static inline int clamp_i64_to_int(int64_t value)
{
    if (value < (int64_t)INT32_MIN)
        return INT32_MIN;
    if (value > (int64_t)INT32_MAX)
        return INT32_MAX;
    return (int)value;
}

static inline int clamp_dirty_rect_count(int count)
{
    if (count < 0)
        return 0;
    if (count > MAX_DIRTY_RECTS)
        return MAX_DIRTY_RECTS;
    return count;
}

static inline bool rect_contains(const DirtyRect &outer, const DirtyRect &inner)
{
    if (outer.w <= 0 || outer.h <= 0 || inner.w <= 0 || inner.h <= 0)
        return false;
    return inner.x >= outer.x && inner.y >= outer.y && rect_right_i64(inner) <= rect_right_i64(outer) &&
           rect_bottom_i64(inner) <= rect_bottom_i64(outer);
}

static inline bool point_in_rect(const DirtyRect &rect, int x, int y)
{
    if (rect.w <= 0 || rect.h <= 0)
        return false;
    return x >= rect.x && y >= rect.y && (int64_t)x < rect_right_i64(rect) && (int64_t)y < rect_bottom_i64(rect);
}

static inline bool rect_intersection(const DirtyRect &a, const DirtyRect &b, DirtyRect *out)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    int64_t x1 = (a.x > b.x) ? a.x : b.x;
    int64_t y1 = (a.y > b.y) ? a.y : b.y;
    int64_t x2 = (rect_right_i64(a) < rect_right_i64(b)) ? rect_right_i64(a) : rect_right_i64(b);
    int64_t y2 = (rect_bottom_i64(a) < rect_bottom_i64(b)) ? rect_bottom_i64(a) : rect_bottom_i64(b);
    if (x2 <= x1 || y2 <= y1)
        return false;
    if (out)
        *out = {clamp_i64_to_int(x1), clamp_i64_to_int(y1), clamp_i64_to_int(x2 - x1), clamp_i64_to_int(y2 - y1)};
    return true;
}

static inline DirtyRect rect_union(const DirtyRect &a, const DirtyRect &b)
{
    int64_t x1 = (a.x < b.x) ? a.x : b.x;
    int64_t y1 = (a.y < b.y) ? a.y : b.y;
    int64_t x2 = (rect_right_i64(a) > rect_right_i64(b)) ? rect_right_i64(a) : rect_right_i64(b);
    int64_t y2 = (rect_bottom_i64(a) > rect_bottom_i64(b)) ? rect_bottom_i64(a) : rect_bottom_i64(b);
    return {clamp_i64_to_int(x1), clamp_i64_to_int(y1), clamp_i64_to_int(x2 - x1), clamp_i64_to_int(y2 - y1)};
}

static inline bool dirty_rects_intersect(const DirtyRect &a, const DirtyRect &b)
{
    return rect_intersection(a, b, nullptr);
}

static inline wm::DirtyRect to_policy_rect(const DirtyRect &rect)
{
    return {rect.x, rect.y, rect.w, rect.h};
}
static inline DirtyRect from_policy_rect(const wm::DirtyRect &rect)
{
    return {rect.x, rect.y, rect.w, rect.h};
}

static inline int wm_resize_grip()
{
    return gui_scaled_metric(RESIZE_GRIP);
}
static inline int wm_button_size()
{
    return gui_scaled_metric(BTN_SIZE);
}
static inline int wm_button_inset_x()
{
    return gui_scaled_metric(BTN_INSET_X);
}
static inline int wm_button_inset_y()
{
    return gui_scaled_metric(BTN_INSET_Y);
}
static inline int wm_button_spacing()
{
    return gui_scaled_metric(BTN_SPACING);
}
static inline int wm_title_bar_h()
{
    return gui_title_bar_h();
}
static inline int wm_menubar_h()
{
    return gui_menubar_h();
}
static inline int wm_desktop_margin()
{
    return gui_scaled_metric(DESKTOP_MARGIN);
}
static inline int wm_dock_reserved_h()
{
    return shell_dock_reserved_h();
}
static inline int wm_default_min_w()
{
    return gui_scaled_metric(MIN_WINDOW_W);
}
static inline int wm_default_min_h()
{
    return gui_scaled_metric(MIN_WINDOW_H);
}
static inline int wm_frame_border()
{
    int border = gui_scaled_metric(FRAME_BORDER);
    return border < 1 ? 1 : border;
}
static inline int wm_frame_shadow_offset_x()
{
    int offset = gui_scaled_metric(1);
    return offset < 1 ? 1 : offset;
}
static inline int wm_frame_shadow_offset_y()
{
    // Focused frames draw a slightly deeper shadow. Use the maximum compositor shadow
    // extent in geometry/damage calculations so moves and resizes do not leave
    // single-pixel shadow remnants behind.
    int offset = gui_scaled_metric(3);
    return offset < 1 ? 1 : offset;
}
static inline int wm_window_damage_pad()
{
    int pad = gui_scaled_metric(WINDOW_DAMAGE_PAD_BASE) + wm_frame_border() + wm_frame_shadow_offset_y();
    return pad < CURSOR_DAMAGE_PAD ? CURSOR_DAMAGE_PAD : pad;
}
static inline DirtyRect rect_expand(const DirtyRect &rect, int pad)
{
    if (pad <= 0 || rect.w <= 0 || rect.h <= 0)
        return rect;
    int64_t x = (int64_t)rect.x - pad;
    int64_t y = (int64_t)rect.y - pad;
    int64_t w = (int64_t)rect.w + (int64_t)pad * 2;
    int64_t h = (int64_t)rect.h + (int64_t)pad * 2;
    return {clamp_i64_to_int(x), clamp_i64_to_int(y), clamp_i64_to_int(w), clamp_i64_to_int(h)};
}
static inline DirtyRect window_button_bounds(const Window &w, int button_index)
{
    int button_size = wm_button_size();
    int title_bar_h = wm_title_bar_h();
    int button_y = w.y - title_bar_h + (title_bar_h - button_size) / 2 + wm_button_inset_y();
    return {w.x + wm_button_inset_x() + button_index * wm_button_spacing(), button_y, button_size, button_size};
}
static inline void window_button_center(const Window &w, int button_index, int *cx, int *cy)
{
    DirtyRect button = window_button_bounds(w, button_index);
    if (cx)
        *cx = button.x + button.w / 2;
    if (cy)
        *cy = button.y + button.h / 2;
}

static inline DirtyRect window_visible_client_bounds(const Window &w)
{
    if (w.transparent)
        return {w.x, w.y, w.w, w.h};

    int border = wm_frame_border();
    int left = w.x + border;
    int top = w.y;
    int right = w.x + w.w - border;
    int bottom = w.y + w.h - border;
    int width = right - left;
    int height = bottom - top;
    if (width <= 0 || height <= 0)
        return {w.x, w.y, w.w, w.h};
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

void init_wallpaper();
void reload_wallpaper(Registry *registry, bool prefer_requested);
bool init_shell_blur_buffers(Registry *registry, uint32_t dock_w, uint32_t dock_h);
void capture_shell_backdrop_for_rect(const DirtyRect &rect, Registry *registry);
void flush_shell_blur_updates(Registry *registry);
bool move_backbuffer_rect(const DirtyRect &old_rect, const DirtyRect &new_rect);
void draw_context_menu_overlay(const Registry *registry);
void draw_storage_prompt_overlay();
bool compose_rect_clipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                          const Registry *registry);
void compose_rect_unclipped(const DirtyRect &r, int focused_index, int hover_frame_index, int hover_button,
                            const Registry *registry);
void paint_desktop_base(Surface *surface);
void draw_debug_overlay(Surface *dst);
void mark_presentbuffer_slots_stale(const DirtyRect &dirty);
void wm_stats_note_dirty_set(const DirtyRect *rects, int rect_count);
void wm_stats_note_stale_repair(int rect_count);

// Logic (Damage & Window State)
void enqueue_damage_rect(int x, int y, int w, int h);
void invalidate_dirty_frame();
void invalidate_window_visibility_cache();
void refresh_window_cache();
void refresh_window_visible_regions();
void normalize_dirty_rects(bool interactive);
bool clip_dirty_rect_to_screen(DirtyRect &rect);
DirtyRect window_client_bounds(const Window &w);
DirtyRect window_outer_bounds(const Window &w);
DirtyRect window_occlusion_bounds(const Window &w);
DirtyRect window_opaque_bounds(const Window &w);
bool is_window_visible(const Window &w);
bool is_user_window(const Window &w);
int find_top_visible_user_window();
int find_registry_focused_user_window(const Registry *registry);
int find_window_by_shm(int shm_id);
int find_window_by_entry(const WindowEntry *entry);
void focus_window_owner(const Window *w);
void publish_focus(Registry *registry, const Window *w);
void clear_window_focus(Registry *registry);
int focus_window(int index, bool raise);
void close_window(int index);
void minimize_window(int index);
void maximize_window(int index);
void toggle_maximize_window(int index);
void restore_window(int index, bool raise);
void set_window_bounds(Window &w, int x, int y, int width, int height);
int bring_window_to_front(int index);
int send_window_to_back(int index);
void add_win_internal(int shm_id, int x, int y, int w, int h, const char *title, Damage *d_ptr, WindowEntry *entry,
                      bool transparent);
int find_top_opaque_covering_window(const DirtyRect &r);
bool rect_intersects_window_chrome(const Window &w, const DirtyRect &r);

#ifdef __cplusplus
extern "C" {
#endif
uint8_t gui_rounded_rect_coverage_local(int32_t col, int32_t row, int32_t w, int32_t h, int32_t r,
                                        uint32_t rounded_edges);
#ifdef __cplusplus
}
#endif

int hit_test_resize(const Window &w, int px, int py);
bool point_in_titlebar(const Window &w, int px, int py);
bool point_in_client(const Window &w, int px, int py);
bool point_in_outer(const Window &w, int px, int py);
bool point_in_button(const Window &w, int px, int py, int button_index);
int system_window_hit(int px, int py);
bool pointer_blocked_by_shell_overlay(int px, int py);
void post_mouse_event_to_window(const Window &w, EventType type, int px, int py, uint8_t button, int8_t scroll_y = 0);
void post_key_event_to_window(const Window &w, EventType type, char c, uint8_t scancode);
void mark_window_frame_damage(const Window &w);
void mark_window_chrome_damage(const Window &w);
void invalidate_window_decoration_cache(Window &w);
void mark_window_transition_damage(const Window &old_w, const Window &new_w);
bool post_window_resize_configure(Window &w);
void mark_cursor_transition_damage(int old_x, int old_y, GuiCursorKind old_kind, int new_x, int new_y,
                                   GuiCursorKind new_kind);
bool clamp_window_scroll(Window &w);
bool scroll_window_content(Window &w, int delta_x, int delta_y);
void apply_mouse_move(Registry *registry, int new_mouse_x, int new_mouse_y);
void update_hover_feedback();
void update_cursor_kind();

StoragePromptLayout storage_prompt_layout();
void sync_storage_prompt_state(bool force_visible);
void ensure_default_user_storage_layout();
void open_storage_prompt();
void dismiss_storage_prompt();
void update_storage_prompt_hover(int mouse_x, int mouse_y);
bool apply_storage_mode_request(Registry *registry, int mode);
bool activate_storage_prompt_button(Registry *registry, int mouse_x, int mouse_y);
void open_context_menu(const Registry *registry, ContextMenuKind kind, int target_index, int anchor_x, int anchor_y);
void close_context_menu();
void update_context_menu_hover(const Registry *registry, int mouse_x, int mouse_y);
bool activate_context_menu_item(Registry *registry, int index);
DirtyRect context_menu_bounds();
int build_context_menu_items(const Registry *registry, GuiMenuItem *items, int max_items);
RuntimeGuiSettings load_runtime_settings();
bool persist_runtime_settings(const Registry *registry);
