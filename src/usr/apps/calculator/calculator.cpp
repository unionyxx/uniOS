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

enum class CalcOp : uint8_t
{
    None, Add, Sub, Mul, Div
};

enum class BtnAction : uint8_t
{
    Digit, Op, Eq, Clear, ClearAll, Decimal, Percent, ToggleSign
};

struct CalcState
{
    double accumulator;
    double current;
    CalcOp pending;
    bool fresh;
    bool has_decimal;
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
    {{"AC", false, BtnAction::ClearAll, 0}, {"+/-", false, BtnAction::ToggleSign, 0}, {"%", false, BtnAction::Percent, 0}, {"/", false, BtnAction::Op, (uint8_t)CalcOp::Div}},
    {{"7", true, BtnAction::Digit, 7}, {"8", true, BtnAction::Digit, 8}, {"9", true, BtnAction::Digit, 9}, {"*", false, BtnAction::Op, (uint8_t)CalcOp::Mul}},
    {{"4", true, BtnAction::Digit, 4}, {"5", true, BtnAction::Digit, 5}, {"6", true, BtnAction::Digit, 6}, {"-", false, BtnAction::Op, (uint8_t)CalcOp::Sub}},
    {{"1", true, BtnAction::Digit, 1}, {"2", true, BtnAction::Digit, 2}, {"3", true, BtnAction::Digit, 3}, {"+", false, BtnAction::Op, (uint8_t)CalcOp::Add}},
    {{"0", true, BtnAction::Digit, 0}, {".", true, BtnAction::Decimal, 0}, {"=", false, BtnAction::Eq, 0}, {"C", false, BtnAction::Clear, 0}},
};

struct CalcRects
{
    Rect display;
    Rect buttons[ROWS][COLS];
    bool layout_valid;
};

// Pre-computed FPU lookup table to prevent precision drift and division latency
static constexpr double k_pow10[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15
};

static void calc_update_display(CalcState *state)
{
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
    memset(state, 0, sizeof(*state));
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
    if (state->pending == CalcOp::None) return;
    
    switch (state->pending) {
        case CalcOp::Add: state->accumulator += state->current; break;
        case CalcOp::Sub: state->accumulator -= state->current; break;
        case CalcOp::Mul: state->accumulator *= state->current; break;
        case CalcOp::Div: state->accumulator = (state->current != 0.0) ? (state->accumulator / state->current) : 0.0; break;
        default: break;
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

static void calc_dispatch_action(CalcState *state, const ButtonDef& btn)
{
    switch (btn.action) {
        case BtnAction::Digit:      calc_input_digit(state, btn.value); break;
        case BtnAction::Op:         calc_input_op(state, static_cast<CalcOp>(btn.value)); break;
        case BtnAction::Eq:         calc_exec_pending(state); state->accumulator = state->current; state->fresh = true; break;
        case BtnAction::Clear:      state->current = 0.0; state->fresh = true; state->has_decimal = false; state->decimal_places = 0; calc_update_display(state); break;
        case BtnAction::ClearAll:   calc_init(state); break;
        case BtnAction::Decimal:    calc_input_decimal(state); break;
        case BtnAction::Percent:    state->current /= 100.0; calc_update_display(state); break;
        case BtnAction::ToggleSign: state->current = -state->current; state->fresh = false; calc_update_display(state); break;
    }
}

static inline uint32_t darken_color(uint32_t color, uint8_t factor)
{
    uint32_t r = ((color >> 16) & 0xFFu) * factor / 255u;
    uint32_t g = ((color >> 8) & 0xFFu) * factor / 255u;
    uint32_t b = (color & 0xFFu) * factor / 255u;
    return (color & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

static void compute_layout(Surface *win, CalcRects *rects)
{
    int pad = gui_scaled_metric(16);
    int top_pad = gui_scaled_metric(8);
    int gap = gui_scaled_metric(10);
    int display_h = gui_scaled_metric(72);
    int win_w = (int)win->width;
    int win_h = (int)win->height;

    rects->display = gui_rect_make(pad, top_pad, win_w - pad * 2, display_h);

    int grid_y = rects->display.y + rects->display.h + gap;
    int grid_h = win_h - grid_y - pad;
    int grid_w = win_w - pad * 2;

    int btn_w = (grid_w - gap * (COLS - 1)) / COLS;
    int btn_h = (grid_h - gap * (ROWS - 1)) / ROWS;
    
    if (btn_h < gui_scaled_metric(44)) btn_h = gui_scaled_metric(44);
    int max_btn_h = gui_scaled_metric(72);
    if (btn_h > max_btn_h) btn_h = max_btn_h;

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
    int dr = gui_radius_md();
    gui_fill_rounded_rect(win, rects->display.x, rects->display.y, rects->display.w, rects->display.h, dr, g_gui_style.app_surface);
    gui_draw_rounded_rect(win, rects->display.x, rects->display.y, rects->display.w, rects->display.h, dr, g_gui_style.border);

    const GuiFont *disp_font = gui_font_title();
    int disp_text_w = gui_measure_text(disp_font, state->display);
    int disp_x = rects->display.x + rects->display.w - disp_text_w - gui_space_2();
    int disp_y = rects->display.y + (rects->display.h - gui_font_line_height(disp_font)) / 2;
    
    if (disp_x < rects->display.x + gui_space_2()) disp_x = rects->display.x + gui_space_2();
    
    gui_draw_text_clipped(win, disp_font, disp_x, disp_y, rects->display.w - gui_space_4(), state->display, g_gui_style.text, g_gui_style.app_surface);
    gui_blit_to_screen_rect(win, rects->display.x, rects->display.y, rects->display.w, rects->display.h);
}

static void render_button(Surface *win, const CalcRects *rects, int row, int col, bool hovered, bool pressed)
{
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
    
    const Rect& r = rects->buttons[row][col];
    const ButtonDef& def = k_buttons[row][col];
    
    int rad = gui_corner_radius(r.w, r.h, gui_radius_md());
    uint32_t bg = def.primary ? g_gui_style.accent : g_gui_style.chrome_bg;
    uint32_t fg = def.primary ? 0xFFFFFFFFu : g_gui_style.text;
    uint32_t border = hovered ? g_gui_style.border_hover : g_gui_style.border;

    if (pressed) {
        bg = darken_color(def.primary ? g_gui_style.accent : g_gui_style.chrome_bg, def.primary ? 200 : 220);
        border = g_gui_style.border_focus;
    } else if (hovered) {
        if (def.primary) {
            bg = g_gui_style.accent_soft;
            fg = 0xFFFFFFFFu;
        } else {
            bg = g_gui_style.chrome_bg_alt;
        }
    }

    gui_fill_rounded_rect(win, r.x, r.y, r.w, r.h, rad, bg);
    gui_draw_rounded_rect(win, r.x, r.y, r.w, r.h, rad, border);

    const GuiFont *font = gui_font_title();
    int tw = gui_measure_text(font, def.label);
    int tx = r.x + (r.w - tw) / 2 + (pressed ? 1 : 0);
    int ty = r.y + (r.h - gui_font_line_height(font)) / 2 + (pressed ? 1 : 0);
    
    gui_draw_text_clipped(win, font, tx, ty, r.w - gui_space_2(), def.label, fg, bg);
    gui_blit_to_screen_rect(win, r.x, r.y, r.w, r.h);
}

static void render_full_ui(Surface *win, CalcState *state, CalcRects *rects, int h_row, int h_col, int p_row, int p_col)
{
    if (!rects->layout_valid) compute_layout(win, rects);
    
    gui_fill_surface(win, g_gui_style.app_bg);
    render_display(win, state, rects);
    
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            render_button(win, rects, r, c, (r == h_row && c == h_col), (r == p_row && c == p_col));
        }
    }
}

static void find_button_at(const CalcRects *rects, int x, int y, int *out_row, int *out_col)
{
    *out_row = -1; *out_col = -1;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            const Rect& b = rects->buttons[r][c];
            if (b.w > 0 && b.h > 0 && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
                *out_row = r; *out_col = c;
                return;
            }
        }
    }
}

extern "C" int main()
{
    Surface win = gui_register_window_ex("Calculator", (uint32_t)gui_scaled_metric(320), (uint32_t)gui_scaled_metric(420), WIN_FLAG_RESIZABLE);
    if (!win.buffer) return 1;

    gui_window_set_min_size(gui_scaled_metric(280), gui_scaled_metric(380));
    gui_sync_theme_from_registry();
    gui_request_focus();

    CalcState state;
    calc_init(&state);
    
    CalcRects rects = {};
    render_full_ui(&win, &state, &rects, -1, -1, -1, -1);

    int hover_r = -1, hover_c = -1;
    int press_r = -1, press_c = -1;

    Registry *registry = gui_registry();
    uint32_t last_settings_gen = registry ? registry->settings_generation : 0;

    while (true) {
        Event ev = {};
        while (poll_event(&ev) > 0) {
            switch (ev.type) {
                case EVT_WINDOW_CLOSE:
                    return 0;
                    
                case EVT_WINDOW_RESIZE:
                    if (gui_sync_window_size(&win) > 0) {
                        rects.layout_valid = false;
                        render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
                    }
                    break;
                    
                case EVT_MOUSE_MOVE: {
                    int r, c;
                    find_button_at(&rects, ev.mouse.x, ev.mouse.y, &r, &c);
                    if (r != hover_r || c != hover_c) {
                        int old_r = hover_r, old_c = hover_c;
                        hover_r = r; hover_c = c;
                        
                        // Partial Invalidation: Only re-rasterize delta state
                        if (old_r >= 0) render_button(&win, &rects, old_r, old_c, false, (old_r == press_r && old_c == press_c));
                        if (hover_r >= 0) render_button(&win, &rects, hover_r, hover_c, true, (hover_r == press_r && hover_c == press_c));
                    }
                    break;
                }
                
                case EVT_MOUSE_DOWN:
                    if (ev.mouse.button == 1 && hover_r >= 0) {
                        press_r = hover_r; press_c = hover_c;
                        calc_dispatch_action(&state, k_buttons[press_r][press_c]);
                        render_display(&win, &state, &rects);
                        render_button(&win, &rects, press_r, press_c, true, true);
                    }
                    break;
                    
                case EVT_MOUSE_UP:
                    if (ev.mouse.button == 1 && press_r >= 0) {
                        int r = press_r, c = press_c;
                        press_r = -1; press_c = -1;
                        render_button(&win, &rects, r, c, (r == hover_r && c == hover_c), false);
                    }
                    break;
                    
                default: break;
            }
        }

        registry = gui_registry();
        if (registry && registry->settings_generation != last_settings_gen) {
            last_settings_gen = registry->settings_generation;
            if (gui_sync_theme_from_registry()) {
                render_full_ui(&win, &state, &rects, hover_r, hover_c, press_r, press_c);
            }
        }

        sleep_ms(16);
    }
}