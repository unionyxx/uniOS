#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/sysinfo.h>

#include "../../libc/unistd.h"
#include "../../libgui/gui.h"

struct CalendarState
{
    int year;
    int month;
    int selected_day;
    int today_year;
    int today_month;
    int today_day;
};

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year))
        return 29;
    return days[month - 1];
}

static int weekday(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    int m = month;
    int d = day;
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void calendar_init(CalendarState *state)
{
    if (!state)
        return;
    SysTime now = {};
    get_time(&now);
    state->year = (int)now.year;
    state->month = (int)now.month;
    state->selected_day = (int)now.day;
    state->today_year = (int)now.year;
    state->today_month = (int)now.month;
    state->today_day = (int)now.day;
}

static void calendar_prev_month(CalendarState *state)
{
    if (!state)
        return;
    state->month--;
    if (state->month < 1) {
        state->month = 12;
        state->year--;
    }
    int dim = days_in_month(state->year, state->month);
    if (state->selected_day > dim)
        state->selected_day = dim;
}

static void calendar_next_month(CalendarState *state)
{
    if (!state)
        return;
    state->month++;
    if (state->month > 12) {
        state->month = 1;
        state->year++;
    }
    int dim = days_in_month(state->year, state->month);
    if (state->selected_day > dim)
        state->selected_day = dim;
}

struct CalendarRects
{
    Rect prev_btn;
    Rect next_btn;
    Rect day_cells[6][7];
};

static bool point_in_rect(const Rect &rect, int x, int y)
{
    return rect.w > 0 && rect.h > 0 && x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

static void draw_chevron(Surface *win, int cx, int cy, int size, bool left, uint32_t color)
{
    int half = size / 2;
    for (int i = 0; i <= half; i++) {
        int x = left ? (cx + i - half / 2) : (cx - i + half / 2);
        int y1 = cy - i;
        int y2 = cy + i;
        if (x >= 0 && x < (int)win->width) {
            if (y1 >= 0 && y1 < (int)win->height)
                gui_draw_pixel(win, x, y1, color);
            if (y2 >= 0 && y2 < (int)win->height)
                gui_draw_pixel(win, x, y2, color);
        }
    }
}

static void draw_calendar(Surface *win, CalendarState *state, CalendarRects *rects, int hover_day_row,
                          int hover_day_col, int hover_arrow)
{
    if (!win || !win->buffer || !state || !rects)
        return;

    gui_fill_surface(win, g_gui_style.app_bg);

    int w = (int)win->width;
    int h = (int)win->height;
    int pad = gui_scaled_metric(16);
    int top_pad = gui_scaled_metric(8);
    int gap = gui_scaled_metric(6);

    const GuiFont *title_font = gui_font_title();
    const GuiFont *def_font = gui_font_default();
    int line_h = gui_line_height();

    static const char *month_names[] = {"January", "February", "March", "April",
                                        "May", "June", "July", "August",
                                        "September", "October", "November", "December"};
    static const char *day_labels[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    char header[64];
    snprintf(header, sizeof(header), "%s %d", month_names[(state->month - 1) % 12], state->year);
    int header_w = gui_measure_text(title_font, header);
    int header_x = (w - header_w) / 2;
    int header_y = top_pad;
    gui_draw_text_clipped(win, title_font, header_x, header_y, w - pad * 2, header, g_gui_style.text, g_gui_style.app_bg);

    int arrow_size = gui_scaled_metric(8);
    int arrow_y = header_y + line_h / 2;
    rects->prev_btn = gui_rect_make(pad, header_y, gui_scaled_metric(28), line_h);
    rects->next_btn = gui_rect_make(w - pad - gui_scaled_metric(28), header_y, gui_scaled_metric(28), line_h);

    uint32_t arrow_color = hover_arrow == -1 ? g_gui_style.text : g_gui_style.text_dim;
    draw_chevron(win, rects->prev_btn.x + rects->prev_btn.w / 2, arrow_y, arrow_size, true, arrow_color);
    arrow_color = hover_arrow == 1 ? g_gui_style.text : g_gui_style.text_dim;
    draw_chevron(win, rects->next_btn.x + rects->next_btn.w / 2, arrow_y, arrow_size, false, arrow_color);

    int grid_y = header_y + line_h + gui_space_2();
    int day_label_h = line_h + gui_space_1();
    int cell_w = (w - pad * 2 - gap * 6) / 7;
    int cell_h = (h - grid_y - day_label_h - pad - gap * 5) / 6;
    if (cell_h < gui_scaled_metric(32))
        cell_h = gui_scaled_metric(32);

    for (int d = 0; d < 7; d++) {
        int dx = pad + d * (cell_w + gap);
        int dw = (d == 6) ? (w - pad - dx) : cell_w;
        int label_w = gui_measure_text(def_font, day_labels[d]);
        int label_x = dx + (dw - label_w) / 2;
        gui_draw_text_clipped(win, def_font, label_x, grid_y, dw, day_labels[d], g_gui_style.text_muted,
                              g_gui_style.app_bg);
    }

    int cells_y = grid_y + day_label_h;
    int start_wd = weekday(state->year, state->month, 1);
    int dim = days_in_month(state->year, state->month);
    
    int prev_m = state->month - 1;
    int prev_y = state->year;
    if (prev_m < 1) { prev_m = 12; prev_y--; }
    int prev_dim = days_in_month(prev_y, prev_m);

    int current_month_day = 1;
    int next_month_day = 1;

    auto format_day_string = [](int val, char *buf) {
        if (val >= 10) {
            buf[0] = '0' + (val / 10);
            buf[1] = '0' + (val % 10);
            buf[2] = '\0';
        } else {
            buf[0] = '0' + val;
            buf[1] = '\0';
        }
    };

    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 7; col++) {
            int cx = pad + col * (cell_w + gap);
            int cy = cells_y + row * (cell_h + gap);
            int cw = (col == 6) ? (w - pad - cx) : cell_w;

            int r_corner = gui_corner_radius(cw, cell_h, gui_radius_sm());
            char day_str[4];
            uint32_t bg = g_gui_style.app_surface;
            uint32_t fg = g_gui_style.text_muted;
            uint32_t border = g_gui_style.border;
            bool is_hovered = (hover_day_row == row && hover_day_col == col);

            rects->day_cells[row][col] = gui_rect_make(cx, cy, cw, cell_h);

            if (row == 0 && col < start_wd) {
                int d_num = prev_dim - start_wd + col + 1;
                format_day_string(d_num, day_str);
                bg = is_hovered ? g_gui_style.chrome_bg_alt : g_gui_style.app_bg;
                fg = g_gui_style.text_muted;
            } else if (current_month_day > dim) {
                format_day_string(next_month_day, day_str);
                bg = is_hovered ? g_gui_style.chrome_bg_alt : g_gui_style.app_bg;
                fg = g_gui_style.text_muted;
                next_month_day++;
            } else {
                bool is_today = state->year == state->today_year && state->month == state->today_month && current_month_day == state->today_day;
                bool is_selected = current_month_day == state->selected_day;

                bg = is_selected ? g_gui_style.accent : (is_hovered ? g_gui_style.chrome_bg_alt : g_gui_style.app_surface);
                fg = is_selected ? 0xFFFFFFFFu : (is_today ? g_gui_style.accent : g_gui_style.text);
                border = is_today ? g_gui_style.accent : g_gui_style.border;

                format_day_string(current_month_day, day_str);
                current_month_day++;
            }

            gui_fill_rounded_rect(win, cx, cy, cw, cell_h, r_corner, bg);
            gui_draw_rounded_rect(win, cx, cy, cw, cell_h, r_corner, border);

            int tw = gui_measure_text(def_font, day_str);
            int tx = cx + (cw - tw) / 2;
            int text_h = gui_font_ascent(def_font);
            int ty = cy + (cell_h - text_h) / 2;
            gui_draw_text_clipped(win, def_font, tx, ty, cw, day_str, fg, bg);
        }
    }
}

static void find_day_at(CalendarRects *rects, int x, int y, int *out_row, int *out_col)
{
    if (!rects || !out_row || !out_col)
        return;
    *out_row = -1;
    *out_col = -1;
    for (int row = 0; row < 6; row++) {
        for (int col = 0; col < 7; col++) {
            if (point_in_rect(rects->day_cells[row][col], x, y)) {
                *out_row = row;
                *out_col = col;
                return;
            }
        }
    }
}

extern "C" int main()
{
    int win_w = gui_scaled_metric(380);
    int win_h = gui_scaled_metric(400);
    Surface win = gui_register_window_ex("Calendar", (uint32_t)win_w, (uint32_t)win_h, WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;
    gui_window_set_min_size(gui_scaled_metric(300), gui_scaled_metric(320));

    gui_sync_theme_from_registry();
    gui_request_focus();

    size_t backbuffer_capacity = (size_t)win.height * win.pitch;
    uint32_t *backbuffer_data = (uint32_t *)malloc(backbuffer_capacity);
    if (!backbuffer_data)
        return 1;

    Surface backbuffer = win;
    backbuffer.buffer = backbuffer_data;
    backbuffer.owns_buffer = false;

    CalendarState state = {};
    calendar_init(&state);
    CalendarRects rects = {};

    int hover_row = -1, hover_col = -1;
    int hover_arrow = 0;
    bool needs_redraw = true;

    Registry *registry = gui_registry();
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            if (ev.type == EVT_WINDOW_CLOSE) {
                free(backbuffer_data);
                return 0;
            }
            if (ev.type == EVT_WINDOW_RESIZE) {
                if (gui_sync_window_size(&win) > 0) {
                    size_t needed_capacity = (size_t)win.height * win.pitch;
                    if (needed_capacity > backbuffer_capacity) {
                        uint32_t *new_ptr = (uint32_t *)realloc(backbuffer_data, needed_capacity);
                        if (new_ptr) {
                            backbuffer_data = new_ptr;
                            backbuffer_capacity = needed_capacity;
                        }
                    }
                    if (backbuffer_data) {
                        backbuffer = win;
                        backbuffer.buffer = backbuffer_data;
                        needs_redraw = true;
                    }
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_MOVE) {
                int row = -1, col = -1;
                find_day_at(&rects, ev.mouse.x, ev.mouse.y, &row, &col);
                int new_hover_arrow = 0;
                if (point_in_rect(rects.prev_btn, ev.mouse.x, ev.mouse.y))
                    new_hover_arrow = -1;
                else if (point_in_rect(rects.next_btn, ev.mouse.x, ev.mouse.y))
                    new_hover_arrow = 1;

                if (row != hover_row || col != hover_col || new_hover_arrow != hover_arrow) {
                    hover_row = row;
                    hover_col = col;
                    hover_arrow = new_hover_arrow;
                    needs_redraw = true;
                }
                continue;
            }
            if (ev.type == EVT_MOUSE_DOWN && ev.mouse.button == 1) {
                if (point_in_rect(rects.prev_btn, ev.mouse.x, ev.mouse.y)) {
                    calendar_prev_month(&state);
                    needs_redraw = true;
                    continue;
                }
                if (point_in_rect(rects.next_btn, ev.mouse.x, ev.mouse.y)) {
                    calendar_next_month(&state);
                    needs_redraw = true;
                    continue;
                }
                int row = -1, col = -1;
                find_day_at(&rects, ev.mouse.x, ev.mouse.y, &row, &col);
                if (row >= 0 && col >= 0) {
                    int start_wd = weekday(state.year, state.month, 1);
                    int clicked_day = row * 7 + col - start_wd + 1;
                    int dim = days_in_month(state.year, state.month);
                    
                    if (clicked_day < 1) {
                        calendar_prev_month(&state);
                        state.selected_day = days_in_month(state.year, state.month) + clicked_day;
                    } else if (clicked_day > dim) {
                        calendar_next_month(&state);
                        state.selected_day = clicked_day - dim;
                    } else {
                        state.selected_day = clicked_day;
                    }
                    needs_redraw = true;
                }
                continue;
            }
        }

        registry = gui_registry();
        if (registry && registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            if (gui_sync_theme_from_registry()) {
                needs_redraw = true;
            }
        }

        SysTime now;
        if (get_time(&now) == 0) {
            bool day_changed = (state.today_year != (int)now.year || state.today_month != (int)now.month ||
                                state.today_day != (int)now.day);
            if (day_changed) {
                state.today_year = (int)now.year;
                state.today_month = (int)now.month;
                state.today_day = (int)now.day;
                needs_redraw = true;
            }
        }

        if (needs_redraw && backbuffer_data) {
            draw_calendar(&backbuffer, &state, &rects, hover_row, hover_col, hover_arrow);
            memcpy(win.buffer, backbuffer.buffer, win.height * win.pitch);
            gui_blit_to_screen_rect(&win, 0, 0, win.width, win.height);
            needs_redraw = false;
        }

        sleep_ms(16);
    }
}