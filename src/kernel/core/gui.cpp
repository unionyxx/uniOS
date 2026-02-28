#include <kernel/gui.h>
#include <drivers/video/framebuffer.h>
#include <drivers/class/hid/input.h>
#include <kernel/time/timer.h>
#include <drivers/rtc/rtc.h>
#include <kernel/scheduler.h>
#include <boot/limine.h>

extern struct limine_framebuffer* g_framebuffer;

static constexpr int     WINDOW_COUNT      = 3;
static constexpr int32_t TASKBAR_HEIGHT    = 40;
static constexpr int32_t CURSOR_W          = 12;
static constexpr int32_t CURSOR_H          = 19;
static constexpr int32_t TITLE_BAR_HEIGHT  = 24;
static constexpr int32_t CLOSE_BTN_SIZE    = 16;
static constexpr int32_t CLOSE_BTN_MARGIN  = 4;
static constexpr int32_t DESKTOP_ICON_X    = 30;
static constexpr int32_t DESKTOP_ICON_SIZE = 48;
static constexpr int32_t DESKTOP_ICON_STEP = 80;

static uint32_t g_cursor_backup[CURSOR_W * CURSOR_H];
static int32_t  g_backup_x = -1, g_backup_y = -1;

struct Window {
    int32_t     x, y;
    int32_t     width, height;
    const char* title;
    uint32_t    color;
    bool        dragging;
    int32_t     drag_offset_x;
    int32_t     drag_offset_y;
    bool        visible;
};

static void restore_cursor_area() {
    if (g_backup_x < 0) return;
    uint32_t* fb = gfx_get_buffer();
    const uint32_t pitch = g_framebuffer->pitch / 4;
    const int32_t width = static_cast<int32_t>(g_framebuffer->width);
    const int32_t height = static_cast<int32_t>(g_framebuffer->height);

    for (int row = 0; row < CURSOR_H; row++) {
        const int32_t py = g_backup_y + row;
        if (py < 0 || py >= height) continue;
        uint32_t* fb_row = &fb[py * pitch];
        const uint32_t* backup_row = &g_cursor_backup[row * CURSOR_W];
        for (int col = 0; col < CURSOR_W; col++) {
            const int32_t px = g_backup_x + col;
            if (px >= 0 && px < width) fb_row[px] = backup_row[col];
        }
    }
    gfx_mark_dirty(g_backup_x, g_backup_y, CURSOR_W, CURSOR_H);
}

static void save_cursor_area(int32_t x, int32_t y) {
    uint32_t* fb = gfx_get_buffer();
    const uint32_t pitch = g_framebuffer->pitch / 4;
    const int32_t width = static_cast<int32_t>(g_framebuffer->width);
    const int32_t height = static_cast<int32_t>(g_framebuffer->height);

    for (int row = 0; row < CURSOR_H; row++) {
        const int32_t py = y + row;
        if (py < 0 || py >= height) continue;
        uint32_t* fb_row = &fb[py * pitch];
        uint32_t* backup_row = &g_cursor_backup[row * CURSOR_W];
        for (int col = 0; col < CURSOR_W; col++) {
            const int32_t px = x + col;
            if (px >= 0 && px < width) backup_row[col] = fb_row[px];
        }
    }
    g_backup_x = x; g_backup_y = y;
}

static void draw_window(const Window& win, bool active) {
    if (!win.visible) return;
    gfx_fill_rect(win.x, win.y, win.width, win.height, win.color);
    gfx_draw_rect(win.x, win.y, win.width, win.height, COLOR_INACTIVE_TITLE);
    gfx_fill_rect(win.x, win.y, win.width, TITLE_BAR_HEIGHT, active ? COLOR_ACCENT : COLOR_INACTIVE_TITLE);
    gfx_draw_string(win.x + 10, win.y + 4, win.title, COLOR_WHITE);
    const int32_t close_x = win.x + win.width - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
    gfx_fill_rect(close_x, win.y + CLOSE_BTN_MARGIN, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, COLOR_RED);
    gfx_draw_char(close_x + 4, win.y + 4, 'x', COLOR_WHITE);
}

static void draw_clock(uint32_t screen_width, uint32_t screen_height) {
    gfx_fill_rect(static_cast<int32_t>(screen_width - 90), static_cast<int32_t>(screen_height - TASKBAR_HEIGHT + 8), 85, TASKBAR_HEIGHT - 16, COLOR_TASKBAR);
    RTCTime time; rtc_get_time(&time);
    char time_str[9] = {
        static_cast<char>('0' + (time.hour / 10)), static_cast<char>('0' + (time.hour % 10)), ':',
        static_cast<char>('0' + (time.minute / 10)), static_cast<char>('0' + (time.minute % 10)), ':',
        static_cast<char>('0' + (time.second / 10)), static_cast<char>('0' + (time.second % 10)), '\0'
    };
    gfx_draw_string(static_cast<int32_t>(screen_width - 80), static_cast<int32_t>(screen_height - TASKBAR_HEIGHT + 12), time_str, COLOR_WHITE);
}

static void draw_desktop(uint32_t width, uint32_t height, Window* windows, int win_count, int active_idx) {
    gfx_draw_gradient_v(0, 0, width, height - TASKBAR_HEIGHT, COLOR_DESKTOP_TOP, COLOR_DESKTOP_BOTTOM);
    gfx_draw_string(DESKTOP_ICON_X + 16, DESKTOP_ICON_X + 12, ">_", COLOR_CYAN);
    gfx_draw_string(DESKTOP_ICON_X, DESKTOP_ICON_X + 40, "Shell", COLOR_WHITE);
    gfx_draw_string(DESKTOP_ICON_X + 20, DESKTOP_ICON_X + DESKTOP_ICON_STEP + 12, "i", COLOR_SUCCESS);
    gfx_draw_string(DESKTOP_ICON_X, DESKTOP_ICON_X + DESKTOP_ICON_STEP + 40, "About", COLOR_WHITE);

    for (int i = 0; i < win_count; i++) if (i != active_idx) draw_window(windows[i], false);
    if (active_idx >= 0 && active_idx < win_count) draw_window(windows[active_idx], true);

    gfx_fill_rect(0, static_cast<int32_t>(height - TASKBAR_HEIGHT), width, TASKBAR_HEIGHT, COLOR_TASKBAR);
    gfx_fill_rect(8, static_cast<int32_t>(height - TASKBAR_HEIGHT + 8), 80, 24, COLOR_ACCENT);
    gfx_draw_string(28, static_cast<int32_t>(height - TASKBAR_HEIGHT + 12), "uniOS", COLOR_WHITE);
}

void gui_start() {
    const uint32_t screen_width = g_framebuffer->width;
    const uint32_t screen_height = g_framebuffer->height;
    Window windows[WINDOW_COUNT] = {
        {150, 100, 300, 200, "Welcome", 0x222222, false, 0, 0, true},
        {500, 150, 250, 300, "System Info", 0x1a1a2e, false, 0, 0, true},
        {200, 350, 400, 250, "Notepad", 0x2d2d2d, false, 0, 0, true}
    };

    int active_window_idx = 2;
    draw_desktop(screen_width, screen_height, windows, WINDOW_COUNT, active_window_idx);

    bool running = true;
    g_backup_x = -1;
    uint64_t last_time_update = 0;
    bool prev_mouse_left = false;

    while (running) {
        input_poll();
        InputMouseState ms; input_mouse_get_state(&ms);
        const uint64_t now = timer_get_ticks();
        bool need_full_redraw = false;

        if (ms.left && !prev_mouse_left) {
            int click_target = -1;
            if (active_window_idx >= 0 && active_window_idx < WINDOW_COUNT) {
                const auto& w = windows[active_window_idx];
                if (w.visible && ms.x >= w.x && ms.x < w.x + w.width && ms.y >= w.y && ms.y < w.y + w.height) click_target = active_window_idx;
            }
            if (click_target == -1) {
                for (int i = WINDOW_COUNT - 1; i >= 0; i--) {
                    if (i == active_window_idx) continue;
                    const auto& w = windows[i];
                    if (w.visible && ms.x >= w.x && ms.x < w.x + w.width && ms.y >= w.y && ms.y < w.y + w.height) { click_target = i; break; }
                }
            }

            if (click_target != -1) {
                active_window_idx = click_target;
                if (ms.y < windows[click_target].y + TITLE_BAR_HEIGHT) {
                    if (ms.x >= windows[click_target].x + windows[click_target].width - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN) {
                        windows[click_target].visible = false;
                    } else {
                        windows[click_target].dragging = true;
                        windows[click_target].drag_offset_x = ms.x - windows[click_target].x;
                        windows[click_target].drag_offset_y = ms.y - windows[click_target].y;
                    }
                }
                need_full_redraw = true;
            } else {
                if (ms.x >= DESKTOP_ICON_X && ms.x < DESKTOP_ICON_X + DESKTOP_ICON_SIZE) {
                    if (ms.y >= DESKTOP_ICON_X && ms.y < DESKTOP_ICON_X + DESKTOP_ICON_SIZE) {
                        windows[0].visible = true; windows[0].x = 150; windows[0].y = 100;
                        active_window_idx = 0; need_full_redraw = true;
                    } else if (ms.y >= DESKTOP_ICON_X + DESKTOP_ICON_STEP && ms.y < DESKTOP_ICON_X + DESKTOP_ICON_STEP + DESKTOP_ICON_SIZE) {
                        windows[1].visible = true; windows[1].x = 500; windows[1].y = 150;
                        active_window_idx = 1; need_full_redraw = true;
                    }
                }
            }
        }

        if (!ms.left && prev_mouse_left) for (auto& w : windows) w.dragging = false;
        if (ms.left) {
            for (auto& w : windows) {
                if (w.dragging) {
                    const int32_t nx = ms.x - w.drag_offset_x, ny = ms.y - w.drag_offset_y;
                    if (nx != w.x || ny != w.y) { w.x = nx; w.y = ny; need_full_redraw = true; }
                }
            }
        }
        prev_mouse_left = ms.left;

        if (now - last_time_update > 1000) {
            last_time_update = now;
            if (!need_full_redraw) {
                restore_cursor_area(); draw_clock(screen_width, screen_height);
                save_cursor_area(ms.x, ms.y); gfx_draw_cursor(ms.x, ms.y);
            }
        }

        if (need_full_redraw) {
            draw_desktop(screen_width, screen_height, windows, WINDOW_COUNT, active_window_idx);
            draw_clock(screen_width, screen_height);
            save_cursor_area(ms.x, ms.y); gfx_draw_cursor(ms.x, ms.y);
            g_backup_x = ms.x; g_backup_y = ms.y;
        } else if (ms.x != g_backup_x || ms.y != g_backup_y) {
            restore_cursor_area(); save_cursor_area(ms.x, ms.y);
            gfx_draw_cursor(ms.x, ms.y);
            g_backup_x = ms.x; g_backup_y = ms.y;
        }

        if (input_keyboard_has_char()) {
            char c = input_keyboard_get_char();
            if (c == 'q' || c == 'Q' || c == 27) running = false;
        }
        gfx_swap_buffers();
        scheduler_yield();
    }
    gfx_clear(COLOR_BLACK);
    gfx_draw_string(50, 50, "uniOS Shell", COLOR_WHITE);
}
