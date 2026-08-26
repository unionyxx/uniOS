#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>

#include "../../libc/unistd.h"
#include "../../libgui/gui.h"

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
    bool layout_valid;
    bool help_visible;
    Rect help_close;
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
    rects->layout_valid = true;
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
    gui_blit_to_screen_rect(win, rects->display.x, rects->display.y, rects->display.w, rects->display.h);
}

static void render_button(Surface *win, const CalcRects *rects, int row, int col, bool hovered, bool pressed,
                          bool armed)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS)
        return;

    const Rect &r = rects->buttons[row][col];
    const ButtonDef &def = k_buttons[row][col];

    gui_app_draw_button_ex(win, r.x, r.y, r.w, r.h, def.label, def.primary, armed, hovered, pressed);

    if (armed && !pressed) {
        // Pending operator stays lit: an inner accent ring marks the armed op.
        int rad = gui_corner_radius(r.w, r.h, gui_radius_md());
        if (r.w > 4 && r.h > 4)
            gui_draw_rounded_rect(win, r.x + 1, r.y + 1, r.w - 2, r.h - 2, rad > 0 ? rad - 1 : 0,
                                  g_gui_style.accent_soft);
    }

    gui_blit_to_screen_rect(win, r.x, r.y, r.w, r.h);
}

static void calc_draw_help(Surface *win, CalcRects *rects);

static void render_full_ui(Surface *win, CalcState *state, CalcRects *rects, int h_row, int h_col, int p_row, int p_col)
{
    if (!rects->layout_valid)
        compute_layout(win, rects);

    gui_fill_surface(win, g_gui_style.app_bg);
    render_display(win, state, rects);

    int armed_r = -1, armed_c = -1;
    if (state->pending != CalcOp::None && !state->error)
        calc_op_button(state->pending, &armed_r, &armed_c);

    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            render_button(win, rects, r, c, (r == h_row && c == h_col), (r == p_row && c == p_col),
                          (r == armed_r && c == armed_c));
        }
    }
    if (rects->help_visible)
        calc_draw_help(win, rects);
    // Commit the whole window: the background fill is content too, and a
    // partial commit would leave stale padding after theme switches.
    gui_blit_to_screen_rect(win, 0, 0, (int)win->width, (int)win->height);
}

static void find_button_at(const CalcRects *rects, int x, int y, int *out_row, int *out_col)
{
    *out_row = -1;
    *out_col = -1;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const Rect &b = rects->buttons[r][c];
            if (b.w > 0 && b.h > 0 && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
                *out_row = r;
                *out_col = c;
                return;
            }
        }
    }
}

static void calc_copy_result(const CalcState *state)
{
    gui_clipboard_copy(state->display, strlen(state->display));
}

static void calc_publish_menus()
{
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

static void calc_handle_menu_command(CalcState *state, uint32_t cmd, CalcRects *rects, bool *redraw)
{
    switch (cmd) {
        case CALC_MENU_COPY:
            calc_copy_result(state);
            break;
        case CALC_MENU_CLEAR:
            calc_dispatch_action(state, {"C", false, BtnAction::Clear, 0});
            *redraw = true;
            break;
        case CALC_MENU_CLEAR_ALL:
            calc_dispatch_action(state, {"AC", false, BtnAction::ClearAll, 0});
            *redraw = true;
            break;
        case CALC_MENU_HELP:
            rects->help_visible = true;
            *redraw = true;
            break;
        default:
            break;
    }
}

static void calc_draw_help(Surface *win, CalcRects *rects)
{
    static const char *tips[] = {
        "Type digits and operators on the keyboard",
        "Enter or = evaluates, Backspace clears the entry",
        "Esc clears everything (AC)",
        "% divides by 100, +/- flips the sign",
        "Edit > Copy Result copies the display (Ctrl+C)",
    };
    int win_w = (int)win->width;
    int win_h = (int)win->height;
    GuiDialogLayout layout = gui_dialog_layout(win_w, win_h, 0, tips, (int)(sizeof(tips) / sizeof(tips[0])), false);
    gui_draw_dialog(win, win_w, win_h, 0, &layout, "Calculator Help", tips, (int)(sizeof(tips) / sizeof(tips[0])),
                    nullptr, "Close", false, false, nullptr, false, false);
    rects->help_close = layout.confirm;
}

extern "C" int main()
{
    Surface win = gui_register_window_ex("Calculator", (uint32_t)gui_scaled_metric(320),
                                         (uint32_t)gui_scaled_metric(420), WIN_FLAG_RESIZABLE);
    if (!win.buffer)
        return 1;

    gui_window_set_min_size(gui_scaled_metric(280), gui_scaled_metric(380));
    gui_sync_theme_from_registry();
    gui_request_focus();

    CalcState state;
    calc_init(&state);

    CalcRects rects = {};
    render_full_ui(&win, &state, &rects, -1, -1, -1, -1);
    calc_publish_menus();

    int hover_r = -1, hover_c = -1;
    int press_r = -1, press_c = -1;

    auto armed_cell = [&](int *ar, int *ac) {
        *ar = -1;
        *ac = -1;
        if (state.pending != CalcOp::None && !state.error)
            calc_op_button(state.pending, ar, ac);
    };
    auto redraw_button = [&](int r, int c) {
        int ar, ac;
        armed_cell(&ar, &ac);
        render_button(&win, &rects, r, c, (r == hover_r && c == hover_c), (r == press_r && c == press_c),
                      (r == ar && c == ac));
    };
    auto apply_action = [&](int r, int c) {
        int armed_before_r, armed_before_c, armed_after_r, armed_after_c;
        armed_cell(&armed_before_r, &armed_before_c);
        calc_dispatch_action(&state, k_buttons[r][c]);
        armed_cell(&armed_after_r, &armed_after_c);
        render_display(&win, &state, &rects);
        // Only the armed-operator highlight spans multiple buttons; repaint
        // fully when it appears/moves, otherwise the single button suffices.
        if (armed_before_r != armed_after_r || armed_before_c != armed_after_c)
            render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
    };

    Registry *registry = gui_registry();
    uint32_t last_settings_gen = registry ? registry->settings_generation : 0;
    uint64_t next_frame_ticks = get_ticks() + 16;

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            switch (ev.type) {
                case EVT_WINDOW_CLOSE:
                    return 0;

                case EVT_WINDOW_RESIZE:
                    if (gui_sync_window_size(&win) > 0) {
                        rects.layout_valid = false;
                        hover_r = hover_c = -1;
                        press_r = press_c = -1;
                        render_full_ui(&win, &state, &rects, -1, -1, -1, -1);
                    }
                    break;

                case EVT_FOCUS:
                    gui_sync_theme_from_registry();
                    calc_publish_menus();
                    render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
                    break;

                case EVT_UNFOCUS:
                case EVT_MOUSE_LEAVE:
                    if (hover_r >= 0 || press_r >= 0) {
                        int old_hr = hover_r, old_hc = hover_c;
                        int old_pr = press_r, old_pc = press_c;
                        hover_r = hover_c = -1;
                        press_r = press_c = -1;
                        if (old_pr >= 0)
                            redraw_button(old_pr, old_pc);
                        if (old_hr >= 0 && (old_hr != old_pr || old_hc != old_pc))
                            redraw_button(old_hr, old_hc);
                    }
                    break;

                case EVT_KEY_DOWN:
                    if (ev.key.c == 0)
                        break;
                    if (rects.help_visible) {
                        if ((uint8_t)ev.key.c == 27 || ev.key.c == '\n' || ev.key.c == '\r') {
                            rects.help_visible = false;
                            render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
                        }
                        break;
                    }
                    if ((uint8_t)ev.key.c == 3) { // Ctrl+C copies the displayed result.
                        calc_copy_result(&state);
                        break;
                    }
                    if (calc_dispatch_key(&state, ev.key.c)) {
                        render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
                    }
                    break;

                case EVT_MOUSE_MOVE: {
                    int r, c;
                    find_button_at(&rects, ev.mouse.x, ev.mouse.y, &r, &c);
                    if (r != hover_r || c != hover_c) {
                        int old_r = hover_r, old_c = hover_c;
                        hover_r = r;
                        hover_c = c;
                        if (old_r >= 0)
                            redraw_button(old_r, old_c);
                        if (hover_r >= 0 && (hover_r != old_r || hover_c != old_c))
                            redraw_button(hover_r, hover_c);
                        // Moving off a pressed button cancels the press visually.
                        if (press_r >= 0 && (press_r != hover_r || press_c != hover_c)) {
                            int pr = press_r, pc = press_c;
                            press_r = press_c = -1;
                            redraw_button(pr, pc);
                        }
                    }
                    break;
                }

                case EVT_MOUSE_DOWN: {
                    if (ev.mouse.button != 1)
                        break;
                    if (rects.help_visible) {
                        rects.help_visible = false;
                        render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
                        break;
                    }
                    int r, c;
                    find_button_at(&rects, ev.mouse.x, ev.mouse.y, &r, &c);
                    if (r >= 0) {
                        press_r = r;
                        press_c = c;
                        redraw_button(r, c);
                    }
                    break;
                }

                case EVT_MOUSE_UP:
                    if (ev.mouse.button != 1)
                        break;
                    if (press_r >= 0) {
                        int r = press_r, c = press_c;
                        press_r = press_c = -1;
                        // Release-to-apply: the action fires only when the
                        // pointer is still over the pressed button, so
                        // dragging away cancels.
                        int up_r, up_c;
                        find_button_at(&rects, ev.mouse.x, ev.mouse.y, &up_r, &up_c);
                        if (up_r == r && up_c == c)
                            apply_action(r, c);
                        redraw_button(r, c);
                    }
                    break;

                default:
                    break;
            }
        }

        registry = gui_registry();
        if (registry && registry->settings_generation != last_settings_gen) {
            last_settings_gen = registry->settings_generation;
            if (gui_sync_theme_from_registry()) {
                render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
            }
        }

        uint32_t menu_cmd = 0;
        if (gui_menu_take_command(&menu_cmd)) {
            bool redraw = false;
            calc_handle_menu_command(&state, menu_cmd, &rects, &redraw);
            if (redraw)
                render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
        }

        sleep_until_ticks(next_frame_ticks);
        uint64_t frame_now = get_ticks();
        next_frame_ticks += 16;
        if (frame_now > next_frame_ticks)
            next_frame_ticks = frame_now + 16;
    }
}