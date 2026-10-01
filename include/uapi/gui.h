#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <uapi/display.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_WINDOWS 32
#define DAMAGE_QUEUE_CAPACITY 8
// Max client-declared interactive rects within the unified headerbar band.
#define WINDOW_HEADER_INPUT_MAX 8
// Unified headerbar traffic-light geometry (base px at 100% UI scale). The WM
// overlays the close/minimize/maximize cluster at the left of the headerband;
// apps use the cluster width to keep their own controls clear of it.
#define HEADER_TRAFFIC_INSET_X 14
#define HEADER_TRAFFIC_SIZE 13
#define HEADER_TRAFFIC_SPACING 21
// Right edge of the 3-button cluster, measured from the window's left edge.
#define HEADER_TRAFFIC_CLUSTER_W (HEADER_TRAFFIC_INSET_X + HEADER_TRAFFIC_SPACING * 2 + HEADER_TRAFFIC_SIZE)
#define MENU_MAX_MENUS 8
#define MENU_MAX_ITEMS 16
#define MENU_LABEL_MAX 20
#define MENU_ACCEL_MAX 7
#define MENU_FLAG_DISABLED 0x1u
#define MENU_FLAG_CHECKED 0x2u
#define MENU_CLIPBOARD_CAP 4095u
// Menu command IDs at or above this value are handled by the menubar itself
// and are never dispatched to the publishing app.
#define MENU_CMD_RESERVED_BASE 0xF000u
// App menu command ids must stay below this. The menubar reserves the range
// from here upward for its own window-operation (0xE001..), system (0xF000..)
// and window-list (0x10000..) commands; publishing an id in that range would be
// intercepted instead of dispatched to the app.
#define MENU_CMD_APP_MAX 0xE000u
#define MENU_CMD_ABOUT_UNIOS 0xF000u
// Delivered to the Settings window (via WindowEntry.menu_command_id) to switch
// it to its About section; a fresh launch passes "about" through the
// open-request slot instead.
#define MENU_CMD_PREFERENCES_ABOUT 0xF001u
#define WIN_FLAG_TRANSPARENT 0x1u
#define WIN_FLAG_SYSTEM 0x2u
#define WIN_FLAG_RESIZABLE 0x4u
#define REGISTRY_MAGIC 0x52454749u
#define WIN_SHM_INVALID (-1)
#define WIN_SHM_RESERVED (-2)
#define WALLPAPER_STATUS_DEFAULT 1u

static inline bool gui_shm_id_is_valid(int shm_id)
{
    return shm_id != WIN_SHM_INVALID && shm_id != WIN_SHM_RESERVED;
}
#define WALLPAPER_STATUS_CUSTOM 2u
#define WALLPAPER_STATUS_SOLID 3u

typedef enum
{
    GUI_THEME_DARK = 0,
    GUI_THEME_LIGHT = 1,
} GuiThemeMode;

enum
{
    SYSTEM_FLAG_SHOW_DESKTOP_GRID = 1u << 0,
    SYSTEM_FLAG_CLOCK_SHOW_SECONDS = 1u << 1,
    SYSTEM_FLAG_LAUNCH_TERMINAL_ON_BOOT = 1u << 2,
    SYSTEM_FLAG_SHOW_DEBUG_STATS = 1u << 3,
    SYSTEM_FLAG_WM_BENCH_DRAG = 1u << 4,
    SYSTEM_FLAG_WM_BENCH_RESIZE = 1u << 5,
    SYSTEM_FLAG_WM_PIXEL_SELFTEST = 1u << 6,
};

// On-accent foreground: labels on accent-filled controls stay white in both
// themes (primary buttons, toggle knobs, the calendar today chip).
#define COLOR_WHITE 0xFFFFFFFF

typedef struct DamageEntry
{
    Rect rect;
    uint32_t seq;
    uint32_t reserved;
} DamageEntry;

typedef struct Damage
{
    volatile uint32_t producer_seq;
    volatile uint32_t consumer_seq;
    volatile uint32_t dropped_updates;
    volatile uint32_t reserved;
    DamageEntry entries[DAMAGE_QUEUE_CAPACITY];
} Damage;

typedef struct WindowEntry
{
    volatile int shm_id;
    volatile int x, y, w, h;
    volatile uint32_t position_serial;
    volatile int restore_x, restore_y, restore_w, restore_h;
    volatile int buffer_w, buffer_h;
    volatile int content_w, content_h;
    volatile int scroll_x, scroll_y;
    volatile int min_w, min_h;
    char title[64];
    volatile uint32_t flags;
    volatile uint32_t owner_pid;
    volatile uint32_t state;
    // Resize/configure protocol:
    //   resize_serial is written by the WM when it requests a new client size.
    //   buffer_resize_serial is written by the client when it has redrawn and
    //   committed a buffer for that resize_serial.  This keeps visible frame
    //   geometry separate from the asynchronously committed client backing:
    //   the compositor flips the window bounds to the configured geometry of
    //   the acknowledged serial, never ahead of it.
    volatile uint32_t resize_serial;
    volatile uint32_t buffer_resize_serial;
    volatile uint32_t buffer_generation;
    volatile uint32_t buffer_ack_generation;
    volatile DisplayBufferHandle buffer_handle;
    Damage damage;
    volatile bool active;
    volatile bool ready;
    volatile bool request_close;
    volatile bool request_focus;
    volatile bool request_minimize;
    volatile bool request_maximize;
    volatile bool request_restore;
    // Menu command dispatch: the menubar writes menu_command_id then bumps
    // menu_command_seq. The owning app polls and consumes each new seq.
    volatile uint32_t menu_command_seq;
    volatile uint32_t menu_command_id;
    // Headerbar input regions: client-canvas-space rects the app declares
    // interactive inside the unified headerbar drag band. The WM forwards
    // clicks/hovers landing in them to the client instead of dragging the
    // window. Writer bumps header_input_seq before and after the payload
    // (store fences around both); the WM reads a stable snapshot.
    volatile uint32_t header_input_seq;
    volatile int32_t header_input_count;
    Rect header_input[WINDOW_HEADER_INPUT_MAX];
} WindowEntry;

typedef struct MenuItem
{
    char label[MENU_LABEL_MAX]; // empty => separator row
    char accel[MENU_ACCEL_MAX]; // display text, e.g. "Ctrl+S"; empty = none
    uint32_t id;                // 0 = separator
    uint8_t flags;              // MENU_FLAG_DISABLED | MENU_FLAG_CHECKED
    uint8_t reserved[3];
} MenuItem;

typedef struct MenuDef
{
    char name[8];
    uint8_t count;
    MenuItem items[MENU_MAX_ITEMS];
} MenuDef;

// Published by the focused app; the menubar renders it only while
// owner_pid == focused_owner_pid. Writer bumps seq before and after the
// payload (store fences around both); the reader requires a stable seq.
typedef struct MenuModel
{
    volatile uint32_t owner_pid;
    volatile uint32_t seq;
    uint8_t menu_count;
    MenuDef menus[MENU_MAX_MENUS];
} MenuModel;

static inline bool window_entry_has_buffer_handle(const WindowEntry *entry)
{
    return entry && entry->buffer_handle != 0;
}

typedef struct Registry
{
    volatile uint32_t magic;
    volatile uint32_t mouse_x, mouse_y;
    volatile bool mouse_clicked;

    volatile int mb_shm_id;
    volatile int dk_shm_id;
    volatile int mb_blur_shm_id;
    volatile int dk_blur_shm_id;
    volatile uint32_t dk_width;
    volatile uint32_t mb_blur_generation;
    volatile uint32_t dk_blur_generation;

    volatile bool mb_clicked;
    volatile uint32_t mb_click_x, mb_click_y;
    // Live screen-space left edge of the menubar date/control-center button,
    // published by the menubar so the WM's click fast path matches the drawn
    // button exactly. 0 = not yet published (WM falls back to a margin).
    volatile int32_t mb_cc_zone_x;
    volatile bool mb_menu_dismiss_requested;
    volatile bool cp_toggle_requested;
    volatile bool dk_clicked;
    volatile uint32_t dk_click_x, dk_click_y;
    volatile bool cp_open;

    volatile int focused_window;
    volatile uint32_t focused_owner_pid;
    volatile uint32_t theme_mode;
    volatile uint32_t settings_generation;
    volatile uint32_t system_flags;
    volatile bool ethernet_enabled;
    volatile bool ethernet_use_dhcp;
    volatile bool animations_enabled;
    volatile uint32_t transparency_level;
    volatile uint32_t volume_level;
    volatile uint32_t storage_mode;
    volatile uint32_t storage_request_generation;
    volatile uint32_t storage_request_mode;
    volatile uint32_t wallpaper_generation;
    volatile uint32_t wallpaper_status;
    volatile bool wallpaper_reload_requested;
    char wallpaper_requested[256];
    char wallpaper_active[256];

    // App -> WM toast notification request: write title/message behind store
    // fences, then bump notify_generation. The WM consumes each generation.
    volatile uint32_t notify_generation;
    char notify_title[64];
    char notify_message[128];

    // Launcher -> app open request: the launcher writes open_path behind
    // store fences and bumps open_generation, then fork/execs the viewer.
    // The launched app takes the path at startup and clears the generation.
    // Single slot: a second request overwrites a still-pending one.
    volatile uint32_t open_generation;
    char open_path[256];

    // Focused app's published menu model (see MenuModel).
    MenuModel menu_model;

    // System text clipboard: write text behind store fences, then bump
    // clipboard_seq. Readers require a stable seq across the copy.
    volatile uint32_t clipboard_seq;
    volatile uint32_t clipboard_len; // 0..MENU_CLIPBOARD_CAP
    char clipboard[MENU_CLIPBOARD_CAP + 1];

    volatile uint32_t window_count;
    WindowEntry windows[MAX_WINDOWS];
} Registry;

typedef enum
{
    WIN_NORMAL,
    WIN_MINIMIZED,
    WIN_MAXIMIZED,
    WIN_HIDDEN
} WindowState;

typedef struct Point
{
    int32_t x;
    int32_t y;
} Point;

static inline void damage_reset(Damage *damage)
{
    if (!damage)
        return;
    damage->producer_seq = 0;
    damage->consumer_seq = 0;
    damage->dropped_updates = 0;
    damage->reserved = 0;
    for (uint32_t i = 0; i < DAMAGE_QUEUE_CAPACITY; i++) {
        damage->entries[i].rect = gui_rect_make(0, 0, 0, 0);
        damage->entries[i].seq = 0;
        damage->entries[i].reserved = 0;
    }
    __sync_synchronize();
}

static inline bool damage_push_rect(Damage *damage, Rect rect)
{
    if (!damage || gui_rect_is_empty(rect))
        return false;

    uint32_t producer = damage->producer_seq;
    uint32_t consumer = damage->consumer_seq;
    if (producer - consumer >= DAMAGE_QUEUE_CAPACITY) {
        __sync_fetch_and_add(&damage->dropped_updates, 1u);
        return false;
    }

    uint32_t slot = producer % DAMAGE_QUEUE_CAPACITY;
    damage->entries[slot].rect = rect;
    damage->entries[slot].seq = producer + 1u;
    __sync_synchronize();
    damage->producer_seq = producer + 1u;
    return true;
}

static inline bool damage_push(Damage *damage, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return damage_push_rect(damage, gui_rect_make(x, y, w, h));
}

static inline bool damage_pop_rect(Damage *damage, Rect *out_rect)
{
    if (!damage || !out_rect)
        return false;
    uint32_t consumer = damage->consumer_seq;
    uint32_t producer = damage->producer_seq;
    if (consumer == producer)
        return false;

    uint32_t slot = consumer % DAMAGE_QUEUE_CAPACITY;
    DamageEntry entry = damage->entries[slot];
    if (entry.seq != consumer + 1u)
        return false;

    *out_rect = entry.rect;
    __sync_synchronize();
    damage->consumer_seq = consumer + 1u;
    return true;
}

static inline uint32_t damage_take_dropped_updates(Damage *damage)
{
    if (!damage)
        return 0;
    return __sync_lock_test_and_set(&damage->dropped_updates, 0u);
}

#ifdef __cplusplus
}
#endif
