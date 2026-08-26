#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>

#include "../../libapp/app.h"
#include "../../libapp/widgets.h"
#include "../../libc/unistd.h"

static constexpr int COLS = 4;
static constexpr int ROWS = 5;
static constexpr int MAX_DECIMAL_PLACES = 15;

// Menubar command IDs (dispatched through WindowEntry.menu_command_id).
enum
{
    CALC_MENU_COPY = 1,
    CALC_MENU_CLEAR,
    CALC_MENU_CLEAR_ALL,
    CALC_MENU_HELP = 0x80,
};

enum class CalcOp : uint8_t
{
    None,
    Add,
    Sub,
    Mul,
    Div
};

enum class BtnAction : uint8_t
{
    Digit,
    Op,
    Eq,
    Clear,
    ClearAll,
    Decimal,
    Percent,
    ToggleSign
};

struct CalcState
{
    double accumulator;
    double current;
    CalcOp pending;
    bool fresh;
    bool has_decimal;
    bool error;
    uint8_t decimal_places;
    char display[32];
};

struct ButtonDef
{
    const char *label;
    bool primary;
    BtnAction action;
    uint8_t value; // Holds either digit (0-9) or CalcOp
};

static constexpr ButtonDef k_buttons[ROWS][COLS] = {
    {{"AC", false, BtnAction::ClearAll, 0},
     {"+/-", false, BtnAction::ToggleSign, 0},
     {"%", false, BtnAction::Percent, 0},
     {"/", false, BtnAction::Op, (uint8_t)CalcOp::Div}},
    {{"7", true, BtnAction::Digit, 7},
     {"8", true, BtnAction::Digit, 8},
     {"9", true, BtnAction::Digit, 9},
     {"*", false, BtnAction::Op, (uint8_t)CalcOp::Mul}},
    {{"4", true, BtnAction::Digit, 4},
     {"5", true, BtnAction::Digit, 5},
     {"6", true, BtnAction::Digit, 6},
     {"-", false, BtnAction::Op, (uint8_t)CalcOp::Sub}},
    {{"1", true, BtnAction::Digit, 1},
     {"2", true, BtnAction::Digit, 2},
     {"3", true, BtnAction::Digit, 3},
     {"+", false, BtnAction::Op, (uint8_t)CalcOp::Add}},
    {{"0", true, BtnAction::Digit, 0},
     {".", true, BtnAction::Decimal, 0},
     {"=", false, BtnAction::Eq, 0},
     {"C", false, BtnAction::Clear, 0}},
};

struct CalcRects
{
    Rect display;
    Rect buttons[ROWS][COLS];
};

// Pre-computed FPU lookup table to prevent precision drift and division latency
static constexpr double k_pow10[] = {1e0, 1e1, 1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                     1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};

static void calc_update_display(CalcState *state)
{
    if (state->error) {
        snprintf(state->display, sizeof(state->display), "Error");
        return;
    }
    if (state->has_decimal && state->decimal_places > 0) {
        snprintf(state->display, sizeof(state->display), "%.*f", state->decimal_places, state->current);
    } else {
        double val = state->current;
        if (val >= 1e15 || (val != 0.0 && val < 1e-15 && val > -1e-15)) {
            snprintf(state->display, sizeof(state->display), "%.6e", val);
        } else {
            int64_t iv = (int64_t)val;
            if ((double)iv == val)
                snprintf(state->display, sizeof(state->display), "%lld", (long long)iv);
            else
                snprintf(state->display, sizeof(state->display), "%.10g", val);
        }
    }

    size_t len = strlen(state->display);
    if (len > 0 && len + 1 < sizeof(state->display) && state->has_decimal && !strchr(state->display, '.')) {
        state->display[len] = '.';
        state->display[len + 1] = '\0';
    }
}

static void calc_init(CalcState *state)
{
    *state = {};
    state->fresh = true;
    calc_update_display(state);
}

static void calc_input_digit(CalcState *state, uint8_t digit)
{
    if (state->fresh) {
        state->current = 0.0;
        state->fresh = false;
        state->has_decimal = false;
        state->decimal_places = 0;
    }

    if (state->has_decimal) {
        if (state->decimal_places < MAX_DECIMAL_PLACES) {
            state->decimal_places++;
            double factor = k_pow10[state->decimal_places];
            state->current = ((double)(int64_t)(state->current * factor) + digit) / factor;
        }
    } else {
        state->current = state->current * 10.0 + digit;
    }
    calc_update_display(state);
}

static void calc_input_decimal(CalcState *state)
{
    if (state->fresh) {
        state->current = 0.0;
        state->fresh = false;
    }
    state->has_decimal = true;
    calc_update_display(state);
}

static void calc_exec_pending(CalcState *state)
{
    if (state->pending == CalcOp::None)
        return;

    switch (state->pending) {
        case CalcOp::Add:
            state->accumulator += state->current;
            break;
        case CalcOp::Sub:
            state->accumulator -= state->current;
            break;
        case CalcOp::Mul:
            state->accumulator *= state->current;
            break;
        case CalcOp::Div:
            if (state->current != 0.0) {
                state->accumulator = state->accumulator / state->current;
            } else {
                state->error = true;
            }
            break;
        default:
            break;
    }

    state->current = state->accumulator;
    state->pending = CalcOp::None;
    state->fresh = true;
    state->has_decimal = false;
    state->decimal_places = 0;
    calc_update_display(state);
}

static void calc_input_op(CalcState *state, CalcOp op)
{
    if (state->pending != CalcOp::None && !state->fresh)
        calc_exec_pending(state);
    else
        state->accumulator = state->current;

    state->pending = op;
    state->fresh = true;
    state->has_decimal = false;
    state->decimal_places = 0;
}

static void calc_dispatch_action(CalcState *state, const ButtonDef &btn)
{
    if (state->error) {
        // From an error state only a fresh digit or an explicit clear resumes.
        if (btn.action == BtnAction::ClearAll) {
            calc_init(state);
        } else if (btn.action == BtnAction::Digit || btn.action == BtnAction::Decimal ||
                   btn.action == BtnAction::Clear) {
            state->error = false;
            calc_init(state);
            if (btn.action == BtnAction::Digit)
                calc_input_digit(state, btn.value);
            else if (btn.action == BtnAction::Decimal)
                calc_input_decimal(state);
        }
        return;
    }
    switch (btn.action) {
        case BtnAction::Digit:
            calc_input_digit(state, btn.value);
            break;
        case BtnAction::Op:
            calc_input_op(state, static_cast<CalcOp>(btn.value));
            break;
        case BtnAction::Eq:
            calc_exec_pending(state);
            state->accumulator = state->current;
            state->fresh = true;
            break;
        case BtnAction::Clear:
            state->current = 0.0;
            state->fresh = true;
            state->has_decimal = false;
            state->decimal_places = 0;
            calc_update_display(state);
            break;
        case BtnAction::ClearAll:
            calc_init(state);
            break;
        case BtnAction::Decimal:
            calc_input_decimal(state);
            break;
        case BtnAction::Percent:
            state->current /= 100.0;
            calc_update_display(state);
            break;
        case BtnAction::ToggleSign:
            state->current = -state->current;
            state->fresh = false;
            calc_update_display(state);
            break;
    }
}

// Keyboard entry point: map a character to a button action where one exists.
static bool calc_dispatch_key(CalcState *state, char c)
{
    if (c >= '0' && c <= '9') {
        ButtonDef d = {"", true, BtnAction::Digit, (uint8_t)(c - '0')};
        calc_dispatch_action(state, d);
        return true;
    }
    switch (c) {
        case '.':
        case ',':
            calc_dispatch_action(state, {".", true, BtnAction::Decimal, 0});
            return true;
        case '+':
            calc_dispatch_action(state, {"+", false, BtnAction::Op, (uint8_t)CalcOp::Add});
            return true;
        case '-':
            calc_dispatch_action(state, {"-", false, BtnAction::Op, (uint8_t)CalcOp::Sub});
            return true;
        case '*':
        case 'x':
        case 'X':
            calc_dispatch_action(state, {"*", false, BtnAction::Op, (uint8_t)CalcOp::Mul});
            return true;
        case '/':
        case ':':
            calc_dispatch_action(state, {"/", false, BtnAction::Op, (uint8_t)CalcOp::Div});
            return true;
        case '=':
        case '\n':
        case '\r':
            calc_dispatch_action(state, {"=", false, BtnAction::Eq, 0});
            return true;
        case '%':
            calc_dispatch_action(state, {"%", false, BtnAction::Percent, 0});
            return true;
        case '\b':
        case 127: // backspace clears the current entry
            calc_dispatch_action(state, {"C", false, BtnAction::Clear, 0});
            return true;
        case 27: // escape clears everything
            calc_dispatch_action(state, {"AC", false, BtnAction::ClearAll, 0});
            return true;
        default:
            return false;
    }
}

// Grid position of the operator button for a pending op (for the armed highlight).
static bool calc_op_button(CalcOp op, int *row, int *col)
{
    switch (op) {
        case CalcOp::Div:
            *row = 0;
            *col = 3;
            return true;
        case CalcOp::Mul:
            *row = 1;
            *col = 3;
            return true;
        case CalcOp::Sub:
            *row = 2;
            *col = 3;
            return true;
        case CalcOp::Add:
            *row = 3;
            *col = 3;
            return true;
        default:
            return false;
    }
}

struct CalcApp
{
    CalcState state;
    CalcRects rects;
    WidgetButton buttons[ROWS][COLS];
    WidgetHelp help;
};

static void compute_layout(Surface *win, CalcRects *rects)
{
    int pad = gui_app_outer_padding();
    int top_pad = gui_space_1();
    int gap = gui_space_1();
    int display_h = gui_scaled_metric(72);
    int win_w = (int)win->width;
    int win_h = (int)win->height;

    rects->display = gui_rect_make(pad, top_pad, win_w - pad * 2, display_h);

    int grid_y = rects->display.y + rects->display.h + gap;
    int grid_h = win_h - grid_y - pad;
    int grid_w = win_w - pad * 2;

    int btn_w = (grid_w - gap * (COLS - 1)) / COLS;
    int btn_h = (grid_h - gap * (ROWS - 1)) / ROWS;

    if (btn_h < gui_scaled_metric(44))
        btn_h = gui_scaled_metric(44);
    int max_btn_h = gui_scaled_metric(72);
    if (btn_h > max_btn_h)
        btn_h = max_btn_h;

    int actual_grid_h = btn_h * ROWS + gap * (ROWS - 1);
    int grid_y_offset = (grid_h - actual_grid_h) / 2;
    int base_y = grid_y + (grid_y_offset > 0 ? grid_y_offset : 0);

    for (int row = 0; row < ROWS; row++) {
        for (int col = 0; col < COLS; col++) {
            int bx = pad + col * (btn_w + gap);
            int by = base_y + row * (btn_h + gap);
            int bw = (col == COLS - 1) ? (win_w - pad - bx) : btn_w;
            rects->buttons[row][col] = gui_rect_make(bx, by, bw, btn_h);
        }
    }
}

static void render_display(Surface *win, CalcState *state, const CalcRects *rects)
{
    gui_draw_panel(win, rects->display.x, rects->display.y, rects->display.w, rects->display.h, g_gui_style.app_surface,
                   g_gui_style.border);

    const GuiFont *disp_font = gui_font_title();
    int max_w = rects->display.w - gui_space_2() * 2;
    if (max_w < 0)
        max_w = 0;

    // Keep the right end (newest digits) visible when the value overflows.
    char shown[32];
    strncpy(shown, state->display, sizeof(shown) - 1);
    shown[sizeof(shown) - 1] = '\0';
    size_t len = strlen(shown);
    while (len > 1 && gui_measure_text_n(disp_font, shown, len) > max_w) {
        memmove(shown, shown + 1, len);
        len--;
    }

    int disp_text_w = gui_measure_text_n(disp_font, shown, len);
    int disp_x = rects->display.x + rects->display.w - gui_space_2() - disp_text_w;
    if (disp_x < rects->display.x + gui_space_2())
        disp_x = rects->display.x + gui_space_2();
    int disp_y = rects->display.y + (rects->display.h - gui_font_line_height(disp_font)) / 2;

    gui_draw_text_clipped(win, disp_font, disp_x, disp_y, rects->display.w - gui_space_4(), shown, g_gui_style.text,
                          g_gui_style.app_surface);
}

static bool calc_armed_cell(const CalcState &state, int *row, int *col)
{
    *row = -1;
    *col = -1;
    if (state.pending == CalcOp::None || state.error)
        return false;
    return calc_op_button(state.pending, row, col);
}

static void calc_draw_help(Surface *win)
{
    static const char *tips[] = {
        "Type digits and operators on the keyboard",
        "Enter or = evaluates, Backspace clears the entry",
        "Esc clears everything (AC)",
        "% divides by 100, +/- flips the sign",
        "Edit > Copy Result copies the display (Ctrl+C)",
    };
    widget_help_draw(win, (int)win->width, (int)win->height, 0, "Calculator Help", tips, 5);
}

static void render_full_ui(Surface *win, CalcApp *app)
{
    compute_layout(win, &app->rects);

    gui_fill_surface(win, g_gui_style.app_bg);
    render_display(win, &app->state, &app->rects);

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            app->buttons[r][c].rect = app->rects.buttons[r][c];
            widget_button_draw(win, &app->buttons[r][c], k_buttons[r][c].label, k_buttons[r][c].primary, false);
        }
    }

    // Pending operator stays lit: an inner accent ring marks the armed op.
    int armed_r, armed_c;
    if (calc_armed_cell(app->state, &armed_r, &armed_c)) {
        const Rect &r = app->rects.buttons[armed_r][armed_c];
        int rad = gui_corner_radius(r.w, r.h, gui_radius_md());
        if (r.w > 4 && r.h > 4)
            gui_draw_rounded_rect(win, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad > 0 ? rad - 1 : 0,
                                  g_gui_style.accent_soft);
    }

    if (app->help.open)
        calc_draw_help(win);
}

static void calc_copy_result(const CalcState *state)
{
    gui_clipboard_copy(state->display, strlen(state->display));
}

static void calc_menus(App *app)
{
    (void)app;
    MenuModel model;
    gui_menu_model_reset(&model);

    int edit = gui_menu_model_add_menu(&model, "Edit");
    gui_menu_model_add_item(&model, edit, "Copy Result", CALC_MENU_COPY, 0, "Ctrl+C");
    gui_menu_model_add_separator(&model, edit);
    gui_menu_model_add_item(&model, edit, "Clear Entry", CALC_MENU_CLEAR, 0, nullptr);
    gui_menu_model_add_item(&model, edit, "Clear All", CALC_MENU_CLEAR_ALL, 0, "Esc");

    int help = gui_menu_model_add_menu(&model, "Help");
    gui_menu_model_add_item(&model, help, "Calculator Help", CALC_MENU_HELP, 0, nullptr);
    gui_menu_model_add_separator(&model, help);
    gui_menu_model_add_item(&model, help, "About uniOS", MENU_CMD_ABOUT_UNIOS, 0, nullptr);

    gui_menu_publish(&model);
}

static void calc_draw(App *app, Surface *canvas)
{
    CalcApp *calc = (CalcApp *)app_user(app);
    render_full_ui(canvas, calc);
}

static void calc_apply_action(App *app, CalcApp *calc, int r, int c)
{
    int armed_before_r, armed_before_c, armed_after_r, armed_after_c;
    calc_armed_cell(calc->state, &armed_before_r, &armed_before_c);
    calc_dispatch_action(&calc->state, k_buttons[r][c]);
    calc_armed_cell(calc->state, &armed_after_r, &armed_after_c);

    app_invalidate(app, calc->rects.display.x, calc->rects.display.y, calc->rects.display.w, calc->rects.display.h);
    // Only the armed-operator highlight spans multiple buttons; repaint
    // fully when it appears/moves, otherwise the single button suffices.
    if (armed_before_r != armed_after_r || armed_before_c != armed_after_c)
        app_invalidate_all(app);
    else
        app_invalidate(app, calc->rects.buttons[r][c].x, calc->rects.buttons[r][c].y, calc->rects.buttons[r][c].w,
                       calc->rects.buttons[r][c].h);
}

static void calc_event(App *app, const Event *ev)
{
    CalcApp *calc = (CalcApp *)app_user(app);

    switch (ev->type) {
        case EVT_KEY_DOWN: {
            if (ev->key.c == 0)
                break;
            if (calc->help.open) {
                if (widget_help_event(&calc->help, ev))
                    app_invalidate_all(app);
                break;
            }
            if ((uint8_t)ev->key.c == 3) { // Ctrl+C copies the displayed result.
                calc_copy_result(&calc->state);
                break;
            }
            if (calc_dispatch_key(&calc->state, ev->key.c))
                app_invalidate_all(app);
            break;
        }

        case EVT_MOUSE_MOVE:
        case EVT_MOUSE_DOWN:
        case EVT_MOUSE_UP: {
            if (calc->help.open) {
                if (widget_help_event(&calc->help, ev))
                    app_invalidate_all(app);
                break;
            }
            for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < COLS; c++) {
                    int rc = widget_button_event(&calc->buttons[r][c], ev);
                    if (rc & WIDGET_CHANGED) {
                        const Rect &b = calc->rects.buttons[r][c];
                        app_invalidate(app, b.x, b.y, b.w, b.h);
                    }
                    if (rc & WIDGET_CLICKED)
                        calc_apply_action(app, calc, r, c);
                }
            }
            break;
        }

        case EVT_UNFOCUS:
        case EVT_MOUSE_LEAVE: {
            for (int r = 0; r < ROWS; r++) {
                for (int c = 0; c < COLS; c++) {
                    if (widget_button_event(&calc->buttons[r][c], ev) & WIDGET_CHANGED) {
                        const Rect &b = calc->rects.buttons[r][c];
                        app_invalidate(app, b.x, b.y, b.w, b.h);
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

static void calc_menu(App *app, uint32_t cmd)
{
    CalcApp *calc = (CalcApp *)app_user(app);
    switch (cmd) {
        case CALC_MENU_COPY:
            calc_copy_result(&calc->state);
            break;
        case CALC_MENU_CLEAR:
            calc_dispatch_action(&calc->state, {"C", false, BtnAction::Clear, 0});
            app_invalidate_all(app);
            break;
        case CALC_MENU_CLEAR_ALL:
            calc_dispatch_action(&calc->state, {"AC", false, BtnAction::ClearAll, 0});
            app_invalidate_all(app);
            break;
        case CALC_MENU_HELP:
            calc->help.open = true;
            app_invalidate_all(app);
            break;
        default:
            break;
    }
}

extern "C" int main()
{
    static CalcApp calc = {};
    calc_init(&calc.state);

    AppConfig config = {};
    config.title = "Calculator";
    config.width = gui_scaled_metric(320);
    config.height = gui_scaled_metric(420);
    config.min_width = gui_scaled_metric(280);
    config.min_height = gui_scaled_metric(380);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 16;
    config.on_draw = calc_draw;
    config.on_event = calc_event;
    config.on_menu = calc_menu;
    config.on_menus = calc_menus;

    return app_run(&config, &calc);
}
