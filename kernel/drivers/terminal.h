#pragma once
#include <stdint.h>

class Terminal {
public:
    Terminal();
    
    // Initialize with screen dimensions and colors
    void init(uint32_t fg_color, uint32_t bg_color);
    
    // Output
    void put_char(char c);
    void write(const char* str);
    void write_line(const char* str);
    
    // Control
    void clear();
    void set_color(uint32_t fg, uint32_t bg);
    void set_cursor_pos(int col, int row);
    void get_cursor_pos(int* col, int* row);
    
    // Cursor blinking
    void set_cursor_visible(bool visible);
    void update_cursor(); // Call periodically
    
    // Direct character operations (no cursor logic)
    void clear_chars(int col, int row, int count);
    void write_char_at(int col, int row, char c);
    void write_char_at_color(int col, int row, char c, uint32_t fg, uint32_t bg);

private:
    void scroll_up();
    void new_line();
    void draw_cursor(bool visible);

    int width_chars;
    int height_chars;
    int cursor_col;
    int cursor_row;
    
    uint32_t fg_color;
    uint32_t bg_color;
    
    bool cursor_visible;
    bool cursor_state; // For blinking
    uint64_t last_blink_tick;
};

// Global terminal instance
extern Terminal g_terminal;
