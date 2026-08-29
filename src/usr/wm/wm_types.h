#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
static constexpr int WM_FIRST_USER_WINDOW = 2; // Indices 0=menubar, 1=dock are system windows
#define MAX_DIRTY_RECTS 128
#define MAX_VISIBLE_REGIONS 512
#define CURSOR_DAMAGE_PAD 4
static constexpr int WINDOW_DAMAGE_PAD_BASE = 3;

static constexpr int FRAME_BORDER = 1;
static constexpr int FRAME_OCCLUSION_INSET = 4;
static constexpr int RESIZE_GRIP = 9;
static constexpr int BTN_SIZE = 13;
static constexpr int BTN_INSET_X = 10;
static constexpr int BTN_INSET_Y = 0;
static constexpr int BTN_SPACING = 20;
static constexpr int MIN_WINDOW_W = 180;
static constexpr int MIN_WINDOW_H = 120;
static constexpr int DESKTOP_MARGIN = 6;
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
static constexpr int WM_SNAP_THRESHOLD_BASE = 14;
static constexpr int WM_SNAP_ESCAPE_BASE = 28;
static constexpr uint64_t WM_RESIZE_CONFIGURE_RETRY_TICKS = 8;
static constexpr int INDEX_MAX_RESULTS = 10;

static constexpr uint32_t GUI_ROUNDED_EDGE_TOP = 1u;
static constexpr uint32_t GUI_ROUNDED_EDGE_BOTTOM = 2u;
static constexpr uint32_t GUI_ROUNDED_EDGE_ALL = GUI_ROUNDED_EDGE_TOP | GUI_ROUNDED_EDGE_BOTTOM;

// Key codes.
#define KEY_UP_ARROW 0x80
#define KEY_DOWN_ARROW 0x81
#define KEY_LEFT_ARROW 0x82
#define KEY_RIGHT_ARROW 0x83

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
    int last_rendered_x, last_rendered_y, last_rendered_w, last_rendered_h;
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

    // Internal state.
    uint32_t buffer_generation_seen;
    uint32_t buffer_generation_acked;
    uint32_t configure_serial;
    uint32_t pending_configure_serial;
    uint32_t entry_resize_serial;
    uint32_t buffer_resize_serial;
    uint64_t last_commit_ticks;
    uint64_t last_configure_ticks;
    bool resize_configure_pending;
    bool first_damage_received;

    // Synchronous resize state: while a configure is outstanding the visible
    // bounds stay at the last committed frame and the entry carries the
    // target. When the client acks, the bounds flip to the pending geometry
    // in one step — never ahead of what the client actually drew.
    int pending_x = 0, pending_y = 0, pending_w = 0, pending_h = 0;

    // WM-owned copy of the last committed frame, captured when a resize
    // configure is posted (the buffer is stable then) and refreshed on the
    // ack. While the client redraws for the outstanding configure the
    // compositor renders from this copy — never from the shared buffer it is
    // actively overwriting.
    Surface resize_snapshot;
    int resize_snapshot_y0 = 0;

    // Consecutive frames the shared WindowEntry could not be sampled stable.
    // Bounded so one busy client cannot force endless full-window re-damage.
    int unstable_sample_count = 0;

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
    uint64_t last_compose_ticks;
    uint64_t total_compose_ticks;
    uint64_t last_present_ticks;
    uint64_t total_present_ticks;
    uint64_t last_frame_ticks;
    uint64_t max_frame_ticks;
    uint64_t last_input_ticks;
    uint64_t last_input_to_submit_ticks;
    uint64_t last_vram_copy_ticks;
    uint64_t last_present_pixels;
    // Synchronous resize accounting: geometry flips applied when the client
    // acknowledged a configure, and acks whose serial fell out of the
    // configure history (client too far behind) and were dropped.
    uint64_t resize_flips;
    uint64_t resize_stale_acks;
};

struct WmBenchState
{
    bool active;
    bool resize_mode;
    int win_index;
    int origin_x;
    int origin_y;
    int origin_w;
    int origin_h;
    uint64_t frames_target;
    uint64_t start_submitted;
    uint64_t start_compose_total;
    uint64_t start_present_total;
    uint64_t start_max_frame_ticks;
    uint64_t start_dirty_area_accum;
    uint64_t start_resize_flips;
    uint64_t start_resize_stale_acks;
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
    GuiCursorKind last_cursor_kind = GUI_CURSOR_ARROW;
    int last_cursor_x = 0;
    int last_cursor_y = 0;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    int drag_origin_mouse_x = 0;
    int drag_origin_mouse_y = 0;
    Window drag_origin = {};
    bool have_pending_move = false;
    int pending_mouse_x = 0;
    int pending_mouse_y = 0;
    bool alt_down = false;
    int snap_edges = RESIZE_NONE;
    DirtyRect snap_preview = {};
    // Client pointer grab: set while a user window holds an active button
    // press. While grabbed, moves and the release are delivered to the
    // grabbed window even when the pointer leaves its client area, so
    // in-window drags (sliders, scrollbars, selections) keep working.
    WindowEntry *client_grab_entry = nullptr;
    uint8_t client_grab_button = 0;
    // Window that last received a forwarded EVT_MOUSE_MOVE; used to post
    // EVT_MOUSE_LEAVE when the pointer exits the client area.
    WindowEntry *move_target_entry = nullptr;
    // Titlebar double-click tracking (maximize/restore). The shm_id/owner
    // pair guards against the registry slot being reused by a different
    // window within the double-click window (ABA).
    WindowEntry *titlebar_click_entry = nullptr;
    uint64_t titlebar_click_ticks = 0;
    bool titlebar_click_was_maximized = false;
    int titlebar_click_shm_id = WIN_SHM_INVALID;
    uint32_t titlebar_click_owner_pid = 0;
};

struct WmMetrics
{
    int resize_grip, button_size, button_inset_x, button_inset_y, button_spacing, title_bar_h, menubar_h,
        desktop_margin, dock_reserved_h, frame_border, frame_shadow_offset_x, frame_shadow_offset_y, default_min_w,
        default_min_h;
};

#define MAX_NOTIFICATIONS 32
#define TOAST_DURATION_TICKS 4000

struct Notification
{
    char title[64];
    char message[128];
    uint64_t timestamp_ticks;
    bool read;
    bool active_toast;
};

struct NotificationCenterState
{
    Notification history[MAX_NOTIFICATIONS];
    int count;
    int head; // Ring buffer head
};
