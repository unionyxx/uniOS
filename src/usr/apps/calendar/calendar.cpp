#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/sysinfo.h>

#include "../../libapp/app.h"
#include "../../libapp/widgets.h"
#include "../../libc/unistd.h"

// Menubar command IDs (dispatched through WindowEntry.menu_command_id).
enum
{
    CAL_MENU_HELP = 0x80,
};

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

static void calendar_goto_today(CalendarState *state)
{
    if (!state)
        return;
    state->year = state->today_year;
    state->month = state->today_month;
    state->selected_day = state->today_day;
}

// Move the selection by delta days, crossing month boundaries.
static void calendar_step_day(CalendarState *state, int delta)
{
    if (!state || delta == 0)
        return;
    int day = state->selected_day + delta;
    int guard = 0;
    while (day < 1 && guard++ < 24) {
        calendar_prev_month(state);
        day += days_in_month(state->year, state->month);
    }
    int dim = days_in_month(state->year, state->month);
    while (day > dim && guard++ < 48) {
        day -= dim;
        calendar_next_month(state);
        dim = days_in_month(state->year, state->month);
    }
    state->selected_day = day;
}

struct CalendarRects
{
    Rect prev_btn;
    Rect next_btn;
    Rect today_btn;
    Rect day_cells[6][7];
};

struct CalendarApp
{
    CalendarState state;
    CalendarRects rects;
    WidgetButton today;
    WidgetHelp help;
    int hover_row;
    int hover_col;
    int hover_arrow; // -1 prev, 1 next, 0 none
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

static void calendar_draw_help(Surface *win)
{
    static const char *tips[] = {
        "Click a day to select it, or use the arrow keys",
        "Page Up / Page Down or the wheel change the month",
        "T jumps to today, the Today button does too",
        "Dimmed days belong to the previous or next month",
    };
    widget_help_draw(win, (int)win->width, (int)win->height, 0, "Calendar Help", tips, 4);
}

static void draw_calendar(Surface *win, CalendarApp *app)
{
    if (!win || !win->buffer)
        return;
    CalendarState *state = &app->state;
    CalendarRects *rects = &app->rects;

    gui_fill_surface(win, g_gui_style.app_bg);

    int w = (int)win->width;
    int h = (int)win->height;
    int pad = gui_app_outer_padding();
    int top_pad = gui_space_1();
    int gap = gui_space_0_5();

    const GuiFont *title_font = gui_font_title();
    const GuiFont *def_font = gui_font_default();
    int line_h = gui_line_height();

    static const char *month_names[] = {"January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December"};
    static const char *day_labels[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    char header[64];
    snprintf(header, sizeof(header), "%s %d", month_names[(state->month - 1) % 12], state->year);
    int header_w = gui_measure_text(title_font, header);
    int header_x = (w - header_w) / 2;
    int header_y = top_pad;
    gui_draw_text_clipped(win, title_font, header_x, header_y, w - pad * 2, header, g_gui_style.text,
                          g_gui_style.app_bg);

    int arrow_size = gui_scaled_metric(8);
    int arrow_y = header_y + line_h / 2;
    int nav_btn = gui_app_control_h();
    int nav_y = header_y + (line_h - nav_btn) / 2;
    rects->prev_btn = gui_rect_make(pad, nav_y, nav_btn, nav_btn);
    rects->next_btn = gui_rect_make(w - pad - nav_btn, nav_y, nav_btn, nav_btn);

    uint32_t arrow_color = app->hover_arrow == -1 ? g_gui_style.text : g_gui_style.text_dim;
    draw_chevron(win, rects->prev_btn.x + rects->prev_btn.w / 2, arrow_y, arrow_size, true, arrow_color);
    arrow_color = app->hover_arrow == 1 ? g_gui_style.text : g_gui_style.text_dim;
    draw_chevron(win, rects->next_btn.x + rects->next_btn.w / 2, arrow_y, arrow_size, false, arrow_color);

    int footer_h = gui_app_control_h() + gui_space_1();
    int grid_y = header_y + line_h + gui_space_2();
    int day_label_h = line_h + gui_space_1();
    int cell_w = (w - pad * 2 - gap * 6) / 7;
    int cell_h = (h - grid_y - day_label_h - pad - gap * 5 - footer_h) / 6;
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
    if (prev_m < 1) {
        prev_m = 12;
        prev_y--;
    }
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
            bool is_hovered = (app->hover_row == row && app->hover_col == col);

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
                bool is_today = state->year == state->today_year && state->month == state->today_month &&
                                current_month_day == state->today_day;
                bool is_selected = current_month_day == state->selected_day;

                bg = is_selected ? g_gui_style.accent
                                 : (is_hovered ? g_gui_style.chrome_bg_alt : g_gui_style.app_surface);
                fg = is_selected ? COLOR_WHITE : (is_today ? g_gui_style.accent : g_gui_style.text);
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

    // Footer: jump back to today's date.
    bool viewing_today_month = state->year == state->today_year && state->month == state->today_month;
    int today_w = gui_scaled_metric(96);
    rects->today_btn = gui_rect_make((w - today_w) / 2, h - pad - gui_app_control_h(), today_w, gui_app_control_h());
    app->today.rect = rects->today_btn;
    widget_button_draw(win, &app->today, viewing_today_month ? "Today" : "Go to Today", false, false);

    if (app->help.open)
        calendar_draw_help(win);
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

static void calendar_menus(App *app)
{
    (void)app;
    MenuModel model;
    gui_menu_model_reset(&model);

    app_menus_add_help(&model, CAL_MENU_HELP);

    gui_menu_publish(&model);
}

static void calendar_draw(App *app, Surface *canvas)
{
    CalendarApp *cal = (CalendarApp *)app_user(app);
    draw_calendar(canvas, cal);
}

static void calendar_clear_hover(CalendarApp *cal)
{
    cal->hover_row = -1;
    cal->hover_col = -1;
}

static void calendar_event(App *app, const Event *ev)
{
    CalendarApp *cal = (CalendarApp *)app_user(app);
    CalendarState *state = &cal->state;
    CalendarRects *rects = &cal->rects;

    switch (ev->type) {
        case EVT_UNFOCUS:
        case EVT_MOUSE_LEAVE:
            calendar_clear_hover(cal);
            cal->hover_arrow = 0;
            widget_button_reset(&cal->today);
            app_invalidate_all(app);
            break;

        case EVT_MOUSE_SCROLL:
            if (ev->mouse.scroll_y > 0)
                calendar_prev_month(state);
            else if (ev->mouse.scroll_y < 0)
                calendar_next_month(state);
            else
                break;
            // The cells under the pointer now show different dates.
            calendar_clear_hover(cal);
            app_invalidate_all(app);
            break;

        case EVT_MOUSE_MOVE: {
            int row = -1, col = -1;
            find_day_at(rects, ev->mouse.x, ev->mouse.y, &row, &col);
            int new_hover_arrow = 0;
            if (point_in_rect(rects->prev_btn, ev->mouse.x, ev->mouse.y))
                new_hover_arrow = -1;
            else if (point_in_rect(rects->next_btn, ev->mouse.x, ev->mouse.y))
                new_hover_arrow = 1;

            bool changed = row != cal->hover_row || col != cal->hover_col || new_hover_arrow != cal->hover_arrow;
            cal->hover_row = row;
            cal->hover_col = col;
            cal->hover_arrow = new_hover_arrow;
            if (widget_button_event(&cal->today, ev) & WIDGET_CHANGED)
                changed = true;
            if (changed)
                app_invalidate_all(app);
            break;
        }

        case EVT_MOUSE_DOWN: {
            if (ev->mouse.button != 1)
                break;
            if (cal->help.open) {
                if (widget_help_event(&cal->help, ev))
                    app_invalidate_all(app);
                break;
            }
            if (point_in_rect(rects->prev_btn, ev->mouse.x, ev->mouse.y)) {
                calendar_prev_month(state);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
                break;
            }
            if (point_in_rect(rects->next_btn, ev->mouse.x, ev->mouse.y)) {
                calendar_next_month(state);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
                break;
            }
            if (widget_button_event(&cal->today, ev) & WIDGET_CHANGED)
                app_invalidate_all(app);
            int row = -1, col = -1;
            find_day_at(rects, ev->mouse.x, ev->mouse.y, &row, &col);
            if (row >= 0 && col >= 0) {
                int start_wd = weekday(state->year, state->month, 1);
                int clicked_day = row * 7 + col - start_wd + 1;
                int dim = days_in_month(state->year, state->month);

                if (clicked_day < 1) {
                    calendar_prev_month(state);
                    state->selected_day = days_in_month(state->year, state->month) + clicked_day;
                } else if (clicked_day > dim) {
                    calendar_next_month(state);
                    state->selected_day = clicked_day - dim;
                } else {
                    state->selected_day = clicked_day;
                }
                app_invalidate_all(app);
            }
            break;
        }

        case EVT_MOUSE_UP: {
            if (ev->mouse.button != 1)
                break;
            int rc = widget_button_event(&cal->today, ev);
            if (rc & WIDGET_CLICKED) {
                calendar_goto_today(state);
                calendar_clear_hover(cal);
            }
            if (rc & (WIDGET_CLICKED | WIDGET_CHANGED))
                app_invalidate_all(app);
            break;
        }

        case EVT_KEY_DOWN: {
            if (cal->help.open) {
                if (widget_help_event(&cal->help, ev))
                    app_invalidate_all(app);
                break;
            }
            uint8_t key = (uint8_t)ev->key.c;
            if (ev->key.c == 't' || ev->key.c == 'T') {
                calendar_goto_today(state);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            } else if (key == 0x82) { // Left
                calendar_step_day(state, -1);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            } else if (key == 0x83) { // Right
                calendar_step_day(state, 1);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            } else if (key == 0x80) { // Up
                calendar_step_day(state, -7);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            } else if (key == 0x81) { // Down
                calendar_step_day(state, 7);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            } else if (key == 0x87) { // Page Up
                calendar_prev_month(state);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            } else if (key == 0x88) { // Page Down
                calendar_next_month(state);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
            }
            break;
        }

        default:
            break;
    }
}

static void calendar_menu(App *app, uint32_t cmd)
{
    CalendarApp *cal = (CalendarApp *)app_user(app);
    if (cmd == CAL_MENU_HELP) {
        cal->help.open = true;
        app_invalidate_all(app);
    }
}

static void calendar_idle(App *app)
{
    CalendarApp *cal = (CalendarApp *)app_user(app);
    SysTime now;
    if (get_time(&now) != 0)
        return;
    bool day_changed = (cal->state.today_year != (int)now.year || cal->state.today_month != (int)now.month ||
                        cal->state.today_day != (int)now.day);
    if (day_changed) {
        cal->state.today_year = (int)now.year;
        cal->state.today_month = (int)now.month;
        cal->state.today_day = (int)now.day;
        app_invalidate_all(app);
    }
}

extern "C" int main()
{
    static CalendarApp cal = {};
    cal.hover_row = -1;
    cal.hover_col = -1;
    calendar_init(&cal.state);

    AppConfig config = {};
    config.title = "Calendar";
    config.width = gui_scaled_metric(380);
    config.height = gui_scaled_metric(400);
    config.min_width = gui_scaled_metric(300);
    config.min_height = gui_scaled_metric(320);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 16;
    config.on_draw = calendar_draw;
    config.on_event = calendar_event;
    config.on_menu = calendar_menu;
    config.on_menus = calendar_menus;
    config.on_idle = calendar_idle;

    return app_run(&config, &cal);
}
