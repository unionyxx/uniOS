#include <kernel/gui.h>
#include <drivers/video/framebuffer.h>
#include <drivers/class/hid/input.h>
#include <kernel/time/timer.h>
#include <drivers/rtc/rtc.h>
#include <kernel/scheduler.h>
#include <boot/limine.h>

extern struct limine_framebuffer* g_framebuffer;

constexpr int     WINDOW_COUNT      = 3;
constexpr int32_t TASKBAR_HEIGHT    = 40;
constexpr int32_t CURSOR_W          = 12;
constexpr int32_t CURSOR_H          = 19;
constexpr int32_t TITLE_BAR_HEIGHT  = 24;
constexpr int32_t CLOSE_BTN_SIZE    = 16;
constexpr int32_t CLOSE_BTN_MARGIN  = 4;
constexpr int32_t DESKTOP_ICON_X    = 30;
constexpr int32_t DESKTOP_ICON_SIZE = 48;
constexpr int32_t DESKTOP_ICON_STEP = 80;
constexpr int32_t CLOCK_CLEAR_W     = 250;

static uint32_t cursor_backup[CURSOR_W * CURSOR_H];
static int32_t  backup_x = -1, backup_y = -1;

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
    if (backup_x < 0) return;
    uint32_t* fb     = gfx_get_buffer();
    uint32_t  pitch  = g_framebuffer->pitch / 4;
    int32_t   width  = (int32_t)g_framebuffer->width;
    int32_t   height = (int32_t)g_framebuffer->height;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = backup_y + row;
        if (py >= 0 && py < height) {
            uint32_t* fb_row     = &fb[py * pitch];
            uint32_t* backup_row = &cursor_backup[row * CURSOR_W];
            for (int col = 0; col < CURSOR_W; col++) {
                int32_t px = backup_x + col;
                if (px >= 0 && px < width) {
                    fb_row[px] = backup_row[col];
                }
            }
        }
    }
    gfx_mark_dirty(backup_x, backup_y, CURSOR_W, CURSOR_H);
}

static void save_cursor_area(int32_t x, int32_t y) {
    uint32_t* fb     = gfx_get_buffer();
    uint32_t  pitch  = g_framebuffer->pitch / 4;
    int32_t   width  = (int32_t)g_framebuffer->width;
    int32_t   height = (int32_t)g_framebuffer->height;

    for (int row = 0; row < CURSOR_H; row++) {
        int32_t py = y + row;
        if (py >= 0 && py < height) {
            uint32_t* fb_row     = &fb[py * pitch];
            uint32_t* backup_row = &cursor_backup[row * CURSOR_W];
            for (int col = 0; col < CURSOR_W; col++) {
                int32_t px = x + col;
                if (px >= 0 && px < width) {
                    backup_row[col] = fb_row[px];
                }
            }
        }
    }
    backup_x = x;
    backup_y = y;
}

static void draw_window(Window* win, bool active) {
    if (!win->visible) return;

    // Window background and 1px separation border
    gfx_fill_rect(win->x, win->y, win->width, win->height, win->color);
    gfx_draw_rect(win->x, win->y, win->width, win->height, COLOR_INACTIVE_TITLE);

    // Title bar with padding
    uint32_t title_color = active ? COLOR_ACCENT : COLOR_INACTIVE_TITLE;
    gfx_fill_rect(win->x, win->y, win->width, TITLE_BAR_HEIGHT, title_color);
    gfx_draw_string(win->x + 10, win->y + 4, win->title, COLOR_WHITE);

    // Centered 'x' close button
    int32_t close_x = win->x + win->width - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
    gfx_fill_rect(close_x, win->y + CLOSE_BTN_MARGIN,
                  CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, COLOR_RED);
    gfx_draw_char(close_x + 4, win->y + 4, 'x', COLOR_WHITE);
}

static void draw_clock(uint32_t screen_width, uint32_t screen_height) {
    // Clear background first to prevent overlapping numbers
    gfx_fill_rect((int32_t)(screen_width - 90),
                  (int32_t)(screen_height - TASKBAR_HEIGHT + 8),
                  85,
                  TASKBAR_HEIGHT - 16,
                  COLOR_TASKBAR);

    RTCTime time;
    rtc_get_time(&time);

    char time_str[9];
    int  ti = 0;
    time_str[ti++] = '0' + (time.hour   / 10);
    time_str[ti++] = '0' + (time.hour   % 10);
    time_str[ti++] = ':';
    time_str[ti++] = '0' + (time.minute / 10);
    time_str[ti++] = '0' + (time.minute % 10);
    time_str[ti++] = ':';
    time_str[ti++] = '0' + (time.second / 10);
    time_str[ti++] = '0' + (time.second % 10);
    time_str[ti]   = '\0';

    // Clock with right padding
    gfx_draw_string((int32_t)(screen_width - 80),
                    (int32_t)(screen_height - TASKBAR_HEIGHT + 12),
                    time_str, COLOR_WHITE);
}

static void draw_desktop(uint32_t width, uint32_t height,
                          Window* windows, int win_count, int active_idx) {
    gfx_draw_gradient_v(0, 0, width, height - TASKBAR_HEIGHT,
                        COLOR_DESKTOP_TOP, COLOR_DESKTOP_BOTTOM);

    const int32_t icon_y   = DESKTOP_ICON_X;
    const int32_t icon_gap = DESKTOP_ICON_STEP;

    // Clean, borderless icons
    gfx_draw_string(DESKTOP_ICON_X + 16, icon_y + 12, ">_", COLOR_CYAN);
    gfx_draw_string(DESKTOP_ICON_X,      icon_y + 40, "Shell", COLOR_WHITE);

    gfx_draw_string(DESKTOP_ICON_X + 20, icon_y + icon_gap + 12, "i", COLOR_SUCCESS);
    gfx_draw_string(DESKTOP_ICON_X,      icon_y + icon_gap + 40, "About", COLOR_WHITE);

    for (int i = 0; i < win_count; i++) {
        if (i != active_idx) draw_window(&windows[i], false);
    }
    if (active_idx >= 0 && active_idx < win_count) {
        draw_window(&windows[active_idx], true);
    }

    // Taskbar refinement
    gfx_fill_rect(0, (int32_t)(height - TASKBAR_HEIGHT),
                  width, TASKBAR_HEIGHT, COLOR_TASKBAR);
    
    // Centered start button
    gfx_fill_rect(8,  (int32_t)(height - TASKBAR_HEIGHT + 8), 80, 24, COLOR_ACCENT);
    gfx_draw_string(28, (int32_t)(height - TASKBAR_HEIGHT + 12), "uniOS", COLOR_WHITE);
}

void gui_start() {
    uint32_t screen_width  = g_framebuffer->width;
    uint32_t screen_height = g_framebuffer->height;

    Window windows[WINDOW_COUNT];

    windows[0].x             = 150; windows[0].y      = 100;
    windows[0].width         = 300; windows[0].height  = 200;
    windows[0].title         = "Welcome";
    windows[0].color         = 0x222222;
    windows[0].dragging      = false;
    windows[0].drag_offset_x = 0;   windows[0].drag_offset_y = 0;
    windows[0].visible       = true;

    windows[1].x             = 500; windows[1].y      = 150;
    windows[1].width         = 250; windows[1].height  = 300;
    windows[1].title         = "System Info";
    windows[1].color         = 0x1a1a2e;
    windows[1].dragging      = false;
    windows[1].drag_offset_x = 0;   windows[1].drag_offset_y = 0;
    windows[1].visible       = true;

    windows[2].x             = 200; windows[2].y      = 350;
    windows[2].width         = 400; windows[2].height  = 250;
    windows[2].title         = "Notepad";
    windows[2].color         = 0x2d2d2d;
    windows[2].dragging      = false;
    windows[2].drag_offset_x = 0;   windows[2].drag_offset_y = 0;
    windows[2].visible       = true;

    int active_window_idx = 2;

    draw_desktop(screen_width, screen_height, windows, WINDOW_COUNT, active_window_idx);

    bool     running          = true;
    backup_x                  = -1;
    uint64_t last_time_update = 0;
    bool     prev_mouse_left  = false;

    while (running) {
        input_poll();

        InputMouseState mouse_state;
        input_mouse_get_state(&mouse_state);

        int32_t  mx              = mouse_state.x;
        int32_t  my              = mouse_state.y;
        uint64_t now             = timer_get_ticks();
        bool     need_full_redraw = false;

        if (mouse_state.left && !prev_mouse_left) {
            int click_target = -1;

            if (active_window_idx >= 0 && active_window_idx < WINDOW_COUNT) {
                Window* win = &windows[active_window_idx];
                if (win->visible &&
                    mx >= win->x && mx < win->x + win->width &&
                    my >= win->y && my < win->y + win->height) {
                    click_target = active_window_idx;
                }
            }

            if (click_target == -1) {
                for (int i = WINDOW_COUNT - 1; i >= 0; i--) {
                    if (i == active_window_idx) continue;
                    Window* win = &windows[i];
                    if (win->visible &&
                        mx >= win->x && mx < win->x + win->width &&
                        my >= win->y && my < win->y + win->height) {
                        click_target = i;
                        break;
                    }
                }
            }

            if (click_target != -1) {
                active_window_idx = click_target;

                if (my < windows[click_target].y + TITLE_BAR_HEIGHT) {
                    int32_t close_x = windows[click_target].x
                                      + windows[click_target].width
                                      - CLOSE_BTN_SIZE - CLOSE_BTN_MARGIN;
                    if (mx >= close_x) {
                        windows[click_target].visible = false;
                    } else {
                        windows[click_target].dragging      = true;
                        windows[click_target].drag_offset_x = mx - windows[click_target].x;
                        windows[click_target].drag_offset_y = my - windows[click_target].y;
                    }
                }
                need_full_redraw = true;
            }

            if (click_target == -1) {
                const int32_t icon_y   = DESKTOP_ICON_X;
                const int32_t icon_gap = DESKTOP_ICON_STEP;

                if (mx >= DESKTOP_ICON_X &&
                    mx <  DESKTOP_ICON_X + DESKTOP_ICON_SIZE &&
                    my >= icon_y &&
                    my <  icon_y + DESKTOP_ICON_SIZE) {
                    windows[0].visible = true;
                    windows[0].x = 150; windows[0].y = 100;
                    active_window_idx = 0;
                    need_full_redraw  = true;
                }

                if (mx >= DESKTOP_ICON_X &&
                    mx <  DESKTOP_ICON_X + DESKTOP_ICON_SIZE &&
                    my >= icon_y + icon_gap &&
                    my <  icon_y + icon_gap + DESKTOP_ICON_SIZE) {
                    windows[1].visible = true;
                    windows[1].x = 500; windows[1].y = 150;
                    active_window_idx = 1;
                    need_full_redraw  = true;
                }
            }
        }

        if (!mouse_state.left && prev_mouse_left) {
            for (int i = 0; i < WINDOW_COUNT; i++) {
                windows[i].dragging = false;
            }
        }

        if (mouse_state.left) {
            for (int i = 0; i < WINDOW_COUNT; i++) {
                if (windows[i].dragging) {
                    int32_t new_x = mx - windows[i].drag_offset_x;
                    int32_t new_y = my - windows[i].drag_offset_y;
                    if (new_x != windows[i].x || new_y != windows[i].y) {
                        windows[i].x     = new_x;
                        windows[i].y     = new_y;
                        need_full_redraw = true;
                    }
                }
            }
        }

        prev_mouse_left = mouse_state.left;

        if (now - last_time_update > 1000) {
            last_time_update = now;

            if (!need_full_redraw) {
                restore_cursor_area();
                draw_clock(screen_width, screen_height);
                save_cursor_area(mx, my);
                gfx_draw_cursor(mx, my);
            }
        }

        if (need_full_redraw) {
            draw_desktop(screen_width, screen_height,
                         windows, WINDOW_COUNT, active_window_idx);
            draw_clock(screen_width, screen_height);
            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
        } else if (mx != backup_x || my != backup_y) {
            restore_cursor_area();
            save_cursor_area(mx, my);
            gfx_draw_cursor(mx, my);
            backup_x = mx;
            backup_y = my;
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
