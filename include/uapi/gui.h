#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <uapi/display.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_WINDOWS 32
#define DAMAGE_QUEUE_CAPACITY 8
#define MENUBAR_HEIGHT 36
#define MENUBAR_CANVAS_HEIGHT 272
#define TITLE_BAR_HEIGHT 24
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
};

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0xFF000000
#define COLOR_RED 0xFFFF0000
#define COLOR_GREEN 0xFF00FF00
#define COLOR_BLUE 0xFF0000FF
#define COLOR_GRAY 0xFF808080

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
    volatile int restore_x, restore_y, restore_w, restore_h;
    volatile int buffer_w, buffer_h;
    volatile int content_w, content_h;
    volatile int scroll_x, scroll_y;
    volatile int min_w, min_h;
    char title[64];
    volatile uint32_t flags;
    volatile uint32_t owner_pid;
    volatile uint32_t state;
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
} WindowEntry;

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
    volatile bool mb_menu_dismiss_requested;
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
        uint32_t newest = (producer - 1u) % DAMAGE_QUEUE_CAPACITY;
        damage->entries[newest].rect = gui_rect_union(damage->entries[newest].rect, rect);
        damage->dropped_updates = damage->dropped_updates + 1u;
        __sync_synchronize();
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

#ifdef __cplusplus
}
#endif
