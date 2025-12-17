#include "terminal.h"
#include "graphics.h"
#include "timer.h"
#include "heap.h"

Terminal g_terminal;

static const int CHAR_WIDTH = 9;
static const int CHAR_HEIGHT = 10;
static const int MARGIN_LEFT = 50;
static const int MARGIN_TOP = 50;
static const int MARGIN_BOTTOM = 30;

Terminal::Terminal() 
    : width_chars(0), height_chars(0),
      cursor_col(0), cursor_row(0), 
      fg_color(COLOR_WHITE), bg_color(COLOR_BLACK),
      cursor_visible(true), cursor_state(true), last_blink_tick(0),
      text_buffer(nullptr), buffer_size(0),
      capturing(false), capture_buffer(nullptr), capture_len(0), capture_max(0) {
}

Terminal::~Terminal() {
    if (text_buffer) {
        free(text_buffer);
        text_buffer = nullptr;
    }
}

void Terminal::init(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
    
    uint64_t screen_w = gfx_get_width();
    uint64_t screen_h = gfx_get_height();
    
    if (screen_w == 0 || screen_h == 0) return;
    
    width_chars = (screen_w - MARGIN_LEFT * 2) / CHAR_WIDTH;
    height_chars = (screen_h - MARGIN_TOP - MARGIN_BOTTOM) / CHAR_HEIGHT;
    
    // Allocate text buffer
    buffer_size = width_chars * height_chars;
    if (text_buffer) {
        free(text_buffer);
    }
    text_buffer = (Cell*)malloc(buffer_size * sizeof(Cell));
    
    // Initialize all cells to empty
    if (text_buffer) {
        for (int i = 0; i < buffer_size; i++) {
            text_buffer[i].ch = ' ';
            text_buffer[i].fg = fg_color;
            text_buffer[i].bg = bg_color;
        }
    }
    
    clear();
}

Cell* Terminal::get_cell(int col, int row) {
    if (!text_buffer || col < 0 || col >= width_chars || row < 0 || row >= height_chars) {
        return nullptr;
    }
    return &text_buffer[row * width_chars + col];
}

void Terminal::clear() {
    // Clear text buffer
    if (text_buffer) {
        for (int i = 0; i < buffer_size; i++) {
            text_buffer[i].ch = ' ';
            text_buffer[i].fg = fg_color;
            text_buffer[i].bg = bg_color;
        }
    }
    
    // Clear screen
    gfx_clear(bg_color);
    cursor_col = 0;
    cursor_row = 0;
}

void Terminal::set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void Terminal::set_cursor_pos(int col, int row) {
    if (cursor_visible) {
        draw_cursor(false);
    }
    
    cursor_col = col;
    cursor_row = row;
    
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col >= width_chars) cursor_col = width_chars - 1;
    
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row >= height_chars) cursor_row = height_chars - 1;
    
    if (cursor_visible) {
        draw_cursor(true);
    }
}

void Terminal::get_cursor_pos(int* col, int* row) {
    if (col) *col = cursor_col;
    if (row) *row = cursor_row;
}

void Terminal::put_char(char c) {
    // If capturing, route to buffer instead of screen
    if (capturing) {
        if (capture_buffer && capture_len < capture_max) {
            capture_buffer[capture_len++] = c;
        }
        return;
    }
    
    if (cursor_visible) {
        draw_cursor(false);
    }
    
    if (c == '\n') {
        new_line();
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            // Update text buffer
            Cell* cell = get_cell(cursor_col, cursor_row);
            if (cell) {
                cell->ch = ' ';
                cell->fg = fg_color;
                cell->bg = bg_color;
            }
            // Update screen
            int x = MARGIN_LEFT + cursor_col * CHAR_WIDTH;
            int y = MARGIN_TOP + cursor_row * CHAR_HEIGHT;
            gfx_clear_char(x, y, bg_color);
        }
    } else if (c >= 32) {
        // Update text buffer
        Cell* cell = get_cell(cursor_col, cursor_row);
        if (cell) {
            cell->ch = c;
            cell->fg = fg_color;
            cell->bg = bg_color;
        }
        
        // Draw to screen
        int x = MARGIN_LEFT + cursor_col * CHAR_WIDTH;
        int y = MARGIN_TOP + cursor_row * CHAR_HEIGHT;
        gfx_draw_char(x, y, c, fg_color);
        
        cursor_col++;
        if (cursor_col >= width_chars) {
            new_line();
        }
    }
    
    if (cursor_visible) {
        draw_cursor(true);
        cursor_state = true;
        last_blink_tick = timer_get_ticks();
    }
}

void Terminal::write(const char* str) {
    while (*str) {
        put_char(*str++);
    }
}

void Terminal::write_line(const char* str) {
    write(str);
    put_char('\n');
}

void Terminal::new_line() {
    cursor_col = 0;
    cursor_row++;
    
    if (cursor_row >= height_chars) {
        scroll_up();
        cursor_row = height_chars - 1;
    }
}

void Terminal::scroll_up() {
    // Safety: ensure buffer exists and dimensions are valid
    if (!text_buffer || width_chars <= 0 || height_chars <= 1 || buffer_size <= 0) return;
    
    // FAST: Shift text buffer up by one row in RAM (~2KB for 80x25)
    // This is the key optimization - no framebuffer pixel copying!
    int rows_to_move = height_chars - 1;
    
    // Move rows 1..N-1 to rows 0..N-2
    for (int row = 0; row < rows_to_move; row++) {
        int dst_idx = row * width_chars;
        int src_idx = (row + 1) * width_chars;
        if (dst_idx >= 0 && src_idx + width_chars <= buffer_size) {
            for (int col = 0; col < width_chars; col++) {
                text_buffer[dst_idx + col] = text_buffer[src_idx + col];
            }
        }
    }
    
    // Clear the last row in buffer
    int last_row_idx = (height_chars - 1) * width_chars;
    if (last_row_idx >= 0 && last_row_idx + width_chars <= buffer_size) {
        for (int col = 0; col < width_chars; col++) {
            text_buffer[last_row_idx + col].ch = ' ';
            text_buffer[last_row_idx + col].fg = fg_color;
            text_buffer[last_row_idx + col].bg = bg_color;
        }
    }
    
    // Redraw entire screen from text buffer
    redraw_screen();
}

void Terminal::redraw_screen() {
    // Safety: ensure buffer exists and dimensions are valid
    if (!text_buffer || width_chars <= 0 || height_chars <= 0 || buffer_size <= 0) return;
    
    // Redraw each row - clear row background then draw characters
    // This avoids the black flash from gfx_clear()
    for (int row = 0; row < height_chars; row++) {
        int y = MARGIN_TOP + row * CHAR_HEIGHT;
        
        // Clear this row's background (no full-screen clear = no flicker)
        gfx_fill_rect(MARGIN_LEFT, y, width_chars * CHAR_WIDTH, CHAR_HEIGHT, bg_color);
        
        // Draw characters for this row
        for (int col = 0; col < width_chars; col++) {
            int idx = row * width_chars + col;
            if (idx >= 0 && idx < buffer_size) {
                Cell* cell = &text_buffer[idx];
                if (cell->ch != ' ') {
                    int x = MARGIN_LEFT + col * CHAR_WIDTH;
                    gfx_draw_char(x, y, cell->ch, cell->fg);
                }
            }
        }
    }
}

void Terminal::redraw_row(int row) {
    if (!text_buffer || row < 0 || row >= height_chars) return;
    
    // Clear the row area
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_fill_rect(MARGIN_LEFT, y, width_chars * CHAR_WIDTH, CHAR_HEIGHT, bg_color);
    
    // Draw characters for this row
    for (int col = 0; col < width_chars; col++) {
        Cell* cell = &text_buffer[row * width_chars + col];
        if (cell->ch != ' ') {
            int x = MARGIN_LEFT + col * CHAR_WIDTH;
            gfx_draw_char(x, y, cell->ch, cell->fg);
        }
    }
}

void Terminal::draw_cursor(bool visible) {
    if (!cursor_visible) return;
    
    int x = MARGIN_LEFT + cursor_col * CHAR_WIDTH;
    int y = MARGIN_TOP + cursor_row * CHAR_HEIGHT;
    
    int cursor_height = 2;
    int cursor_y = y + CHAR_HEIGHT - cursor_height;
    
    if (visible) {
        gfx_fill_rect(x, cursor_y, CHAR_WIDTH, cursor_height, 0xFFFFFFFF);
    } else {
        gfx_fill_rect(x, cursor_y, CHAR_WIDTH, cursor_height, bg_color);
    }
}

void Terminal::set_cursor_visible(bool visible) {
    cursor_visible = visible;
    if (visible) {
        cursor_state = true;
        last_blink_tick = timer_get_ticks();
        draw_cursor(true);
    } else {
        draw_cursor(false);
    }
}

void Terminal::update_cursor() {
    if (!cursor_visible) return;
    
    uint64_t now = timer_get_ticks();
    if (now - last_blink_tick > 30) {
        last_blink_tick = now;
        cursor_state = !cursor_state;
        draw_cursor(cursor_state);
    }
}

void Terminal::clear_chars(int col, int row, int count) {
    // Clear in text buffer
    for (int i = 0; i < count; i++) {
        Cell* cell = get_cell(col + i, row);
        if (cell) {
            cell->ch = ' ';
            cell->fg = fg_color;
            cell->bg = bg_color;
        }
    }
    
    // Clear on screen
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_fill_rect(x, y, count * CHAR_WIDTH, CHAR_HEIGHT, bg_color);
}

void Terminal::write_char_at(int col, int row, char c) {
    // Update text buffer
    Cell* cell = get_cell(col, row);
    if (cell) {
        cell->ch = c;
        cell->fg = fg_color;
        cell->bg = bg_color;
    }
    
    // Draw to screen
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_draw_char(x, y, c, fg_color);
}

void Terminal::write_char_at_color(int col, int row, char c, uint32_t fg, uint32_t bg) {
    // Update text buffer
    Cell* cell = get_cell(col, row);
    if (cell) {
        cell->ch = c;
        cell->fg = fg;
        cell->bg = bg;
    }
    
    // Draw to screen
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_fill_rect(x, y, CHAR_WIDTH, CHAR_HEIGHT, bg);
    gfx_draw_char(x, y, c, fg);
}

void Terminal::start_capture(char* buffer, size_t max_len) {
    capture_buffer = buffer;
    capture_max = max_len;
    capture_len = 0;
    capturing = true;
}

size_t Terminal::stop_capture() {
    capturing = false;
    size_t len = capture_len;
    
    if (capture_buffer && capture_len < capture_max) {
        capture_buffer[capture_len] = '\0';
    }
    
    capture_buffer = nullptr;
    capture_len = 0;
    capture_max = 0;
    
    return len;
}
