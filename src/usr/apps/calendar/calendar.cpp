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
    if (month < 1 || month > 12)
        return 31;
    if (month == 2 && is_leap_year(year))
        return 29;
    return days[month - 1];
}

static int weekday(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 1 || month > 12)
        month = 1;
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
    int year = (int)now.year;
    int month = (int)now.month;
    int day = (int)now.day;
    // The RTC fields come straight from CMOS; clamp them into range so the
    // month/day tables are never indexed out of bounds.
    if (month < 1 || month > 12)
        month = 1;
    if (day < 1 || day > days_in_month(year, month))
        day = 1;
    state->year = year;
    state->month = month;
    state->selected_day = day;
    state->today_year = year;
    state->today_month = month;
    state->today_day = day;
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
    WidgetHelp help;
    int hover_row;
    int hover_col;
    int hover_arrow; // -1 prev, 1 next, 0 none
    bool hover_today;
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
    int pad = gui_scaled_metric(12);

    const GuiFont *title_font = gui_font_title();
    const GuiFont *def_font = gui_font_default();
    int line_h = gui_line_height();

    static const char *month_names[] = {"January", "February", "March",     "April",   "May",      "June",
                                        "July",    "August",   "September", "October", "November", "December"};
    static const char *day_labels[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

    // Headerbar: prev arrow, month/year title, next arrow, and a small "Today"
    // text button live in the unified headerbar band, starting clear of the WM
    // traffic lights. The band blends seamlessly into the grid below.
    int header_h = gui_headerbar_h();
    int control_h = gui_scaled_metric(24);
    int control_y = (header_h - control_h) / 2;
    int arrow_btn = gui_scaled_metric(20);
    int nav_x = gui_traffic_lights_w() + gui_scaled_metric(8);

    char header[64];
    snprintf(header, sizeof(header), "%s %d", month_names[(state->month - 1) % 12], state->year);
    int header_w = gui_measure_text(title_font, header);

    rects->prev_btn = gui_rect_make(nav_x, control_y, arrow_btn, control_h);
    int title_x = nav_x + arrow_btn + gui_scaled_metric(6);
    int header_text_y = gui_align_text_y(title_font, control_y, control_h);
    gui_draw_text_clipped(win, title_font, title_x, header_text_y, header_w, header, g_gui_style.text,
                          g_gui_style.app_bg);
    rects->next_btn = gui_rect_make(title_x + header_w + gui_scaled_metric(6), control_y, arrow_btn, control_h);

    int arrow_size = gui_scaled_metric(7);
    uint32_t arrow_color = app->hover_arrow == -1 ? g_gui_style.text : g_gui_style.text_muted;
    draw_chevron(win, rects->prev_btn.x + rects->prev_btn.w / 2, control_y + control_h / 2, arrow_size, true,
                 arrow_color);
    arrow_color = app->hover_arrow == 1 ? g_gui_style.text : g_gui_style.text_muted;
    draw_chevron(win, rects->next_btn.x + rects->next_btn.w / 2, control_y + control_h / 2, arrow_size, false,
                 arrow_color);

    bool viewing_today_month = state->year == state->today_year && state->month == state->today_month;
    int today_w = gui_measure_text(def_font, "Today") + gui_space_1();
    rects->today_btn = gui_rect_make(w - pad - today_w, control_y, today_w, control_h);
    {
        int r = gui_radius_xs();
        if (app->hover_today)
            gui_fill_rounded_rect(win, rects->today_btn.x, rects->today_btn.y, rects->today_btn.w, rects->today_btn.h,
                                  r, gui_hover_wash_color());
        uint32_t today_fg = viewing_today_month ? g_gui_style.text_muted : g_gui_style.accent;
        gui_draw_text_clipped(win, def_font, rects->today_btn.x + gui_space_0_5(),
                              gui_align_text_y(def_font, control_y, control_h), today_w, "Today", today_fg, 0);
    }

    // Keep the month navigation clickable inside the headerbar drag zone.
    Rect header_input[3] = {rects->prev_btn, rects->next_btn, rects->today_btn};
    gui_window_set_header_input(header_input, 3);

    // Weekday labels start below the headerbar band.
    int grid_x = pad;
    int grid_w = w - pad * 2;
    int slot_w = grid_w / 7;
    int labels_y = header_h + gui_scaled_metric(8);
    for (int d = 0; d < 7; d++) {
        int dx = grid_x + d * slot_w;
        int dw = (d == 6) ? (grid_x + grid_w - dx) : slot_w;
        int label_w = gui_measure_text(def_font, day_labels[d]);
        int label_x = dx + (dw - label_w) / 2;
        gui_draw_text_clipped(win, def_font, label_x, labels_y, dw, day_labels[d], g_gui_style.text_muted,
                              g_gui_style.app_bg);
    }

    // Day grid: circular hitboxes, filled circle for today, outlined for the
    // selected day.
    int circle_d = gui_scaled_metric(28);
    int row_pitch = circle_d + gui_scaled_metric(4);
    int cells_y = labels_y + line_h + gui_scaled_metric(4);
    int grid_h = 6 * row_pitch;
    int extra_y = h - (cells_y + grid_h) - gui_scaled_metric(8);
    if (extra_y > 0)
        cells_y += extra_y / 2;

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
            int slot_x = grid_x + col * slot_w;
            int slot_w_actual = (col == 6) ? (grid_x + grid_w - slot_x) : slot_w;
            int cy = cells_y + row * row_pitch;

            rects->day_cells[row][col] = gui_rect_make(slot_x, cy, slot_w_actual, row_pitch);

            char day_str[4];
            bool in_month = true;
            bool is_today = false;
            bool is_selected = false;

            if (row == 0 && col < start_wd) {
                format_day_string(prev_dim - start_wd + col + 1, day_str);
                in_month = false;
            } else if (current_month_day > dim) {
                format_day_string(next_month_day, day_str);
                next_month_day++;
                in_month = false;
            } else {
                is_today = state->year == state->today_year && state->month == state->today_month &&
                           current_month_day == state->today_day;
                is_selected = current_month_day == state->selected_day;
                format_day_string(current_month_day, day_str);
                current_month_day++;
            }

            int cx = slot_x + (slot_w_actual - circle_d) / 2;
            int circle_r = circle_d / 2;
            int ccx = cx + circle_r;
            int ccy = cy + row_pitch / 2;
            bool is_hovered = (app->hover_row == row && app->hover_col == col);

            if (in_month && is_today) {
                gui_fill_circle(win, ccx, ccy, circle_r, g_gui_style.accent);
            } else if (in_month && is_selected) {
                gui_draw_circle_stroke(win, ccx, ccy, circle_r - 1, gui_scaled_metric(1) + 1, g_gui_style.accent);
            } else if (is_hovered) {
                gui_fill_circle(win, ccx, ccy, circle_r, gui_hover_wash_color());
            }

            uint32_t fg;
            if (in_month && is_today)
                fg = COLOR_WHITE;
            else if (in_month)
                fg = g_gui_style.text;
            else
                fg = g_gui_style.text_muted;

            int tw = gui_measure_text(def_font, day_str);
            int tx = ccx - tw / 2;
            int ty = gui_align_text_y(def_font, ccy - circle_r, circle_d);
            gui_draw_text_clipped(win, def_font, tx, ty, tw, day_str, fg, 0);
        }
    }

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
            cal->hover_today = false;
            app_invalidate_all(app);
            break;

        case EVT_MOUSE_SCROLL:
            // Don't change the month while the Help overlay is up.
            if (cal->help.open)
                break;
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
            bool new_hover_today = point_in_rect(rects->today_btn, ev->mouse.x, ev->mouse.y);

            bool changed = row != cal->hover_row || col != cal->hover_col || new_hover_arrow != cal->hover_arrow ||
                           new_hover_today != cal->hover_today;
            cal->hover_row = row;
            cal->hover_col = col;
            cal->hover_arrow = new_hover_arrow;
            cal->hover_today = new_hover_today;
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
            if (point_in_rect(rects->today_btn, ev->mouse.x, ev->mouse.y)) {
                calendar_goto_today(state);
                calendar_clear_hover(cal);
                app_invalidate_all(app);
                break;
            }
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
    config.width = gui_scaled_metric(320);
    config.height = gui_scaled_metric(300);
    config.min_width = gui_scaled_metric(320);
    config.min_height = gui_scaled_metric(300);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 16;
    config.on_draw = calendar_draw;
    config.on_event = calendar_event;
    config.on_menu = calendar_menu;
    config.on_menus = calendar_menus;
    config.on_idle = calendar_idle;

    return app_run(&config, &cal);
}
