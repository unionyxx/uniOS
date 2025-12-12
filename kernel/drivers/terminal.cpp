#include "terminal.h"
#include "graphics.h"
#include "timer.h"

Terminal g_terminal;

static const int CHAR_WIDTH = 9;
static const int CHAR_HEIGHT = 10;
static const int MARGIN_LEFT = 50;
static const int MARGIN_TOP = 50;
static const int MARGIN_BOTTOM = 30;

Terminal::Terminal() 
    : cursor_col(0), cursor_row(0), 
      fg_color(COLOR_WHITE), bg_color(COLOR_BLACK),
      cursor_visible(true), cursor_state(true), last_blink_tick(0),
      capturing(false), capture_buffer(nullptr), capture_len(0), capture_max(0) {
}

void Terminal::init(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
    
    uint64_t screen_w = gfx_get_width();
    uint64_t screen_h = gfx_get_height();
    
    if (screen_w == 0 || screen_h == 0) return;
    
    width_chars = (screen_w - MARGIN_LEFT * 2) / CHAR_WIDTH;
    height_chars = (screen_h - MARGIN_TOP - MARGIN_BOTTOM) / CHAR_HEIGHT;
    
    clear();
}

void Terminal::clear() {
    gfx_clear(bg_color);
    cursor_col = 0;
    cursor_row = 0;
    
    // Draw header if needed, or just leave blank
    // gfx_draw_string(MARGIN_LEFT, MARGIN_TOP - 20, "uniOS Terminal", fg_color);
}

void Terminal::set_color(uint32_t fg, uint32_t bg) {
    fg_color = fg;
    bg_color = bg;
}

void Terminal::set_cursor_pos(int col, int row) {
    // Only manage cursor drawing if cursor is visible
    if (cursor_visible) {
        draw_cursor(false);  // Erase old cursor
    }
    
    cursor_col = col;
    cursor_row = row;
    
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col >= width_chars) cursor_col = width_chars - 1;
    
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row >= height_chars) cursor_row = height_chars - 1;
    
    // Only draw new cursor if visible
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
    
    // Only hide cursor if it's visible
    if (cursor_visible) {
        draw_cursor(false);
    }
    
    if (c == '\n') {
        new_line();
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            int x = MARGIN_LEFT + cursor_col * CHAR_WIDTH;
            int y = MARGIN_TOP + cursor_row * CHAR_HEIGHT;
            gfx_clear_char(x, y, bg_color);
        }
    } else if (c >= 32) {
        int x = MARGIN_LEFT + cursor_col * CHAR_WIDTH;
        int y = MARGIN_TOP + cursor_row * CHAR_HEIGHT;
        
        gfx_draw_char(x, y, c, fg_color);
        
        cursor_col++;
        if (cursor_col >= width_chars) {
            new_line();
        }
    }
    
    // Only show cursor if it's visible
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
    // Scroll the entire screen area used by terminal
    // We scroll by one line height
    gfx_scroll_up(CHAR_HEIGHT, bg_color);
}

void Terminal::draw_cursor(bool visible) {
    if (!cursor_visible) return;
    
    int x = MARGIN_LEFT + cursor_col * CHAR_WIDTH;
    int y = MARGIN_TOP + cursor_row * CHAR_HEIGHT;
    
    // Draw cursor as a solid line at bottom of character cell
    int cursor_height = 2;
    int cursor_y = y + CHAR_HEIGHT - cursor_height;
    
    if (visible) {
        // Draw bright white cursor line
        gfx_fill_rect(x, cursor_y, CHAR_WIDTH, cursor_height, 0xFFFFFFFF);
    } else {
        // Clear cursor line with background color
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
    // Blink every 30 ticks (~300ms at 100Hz) for more visible blinking
    if (now - last_blink_tick > 30) {
        last_blink_tick = now;
        cursor_state = !cursor_state;
        draw_cursor(cursor_state);
    }
}

void Terminal::clear_chars(int col, int row, int count) {
    // Clear characters AND cursor area without affecting cursor state
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    
    // Clear entire character cells including cursor area
    // Use fill_rect to clear the full cell height
    gfx_fill_rect(x, y, count * CHAR_WIDTH, CHAR_HEIGHT, bg_color);
}

void Terminal::write_char_at(int col, int row, char c) {
    // Write character without affecting cursor state
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_draw_char(x, y, c, fg_color);
}

void Terminal::write_char_at_color(int col, int row, char c, uint32_t fg, uint32_t bg) {
    // Write character with specific colors (for selection highlighting)
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_fill_rect(x, y, CHAR_WIDTH, CHAR_HEIGHT, bg);  // Clear with bg color
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
    
    // Null-terminate if there's room
    if (capture_buffer && capture_len < capture_max) {
        capture_buffer[capture_len] = '\0';
    }
    
    capture_buffer = nullptr;
    capture_len = 0;
    capture_max = 0;
    
    return len;
}

