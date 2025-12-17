#pragma once
#include <stdint.h>
#include <stddef.h>

// Cell structure for text buffer (character + colors)
struct Cell {
    char ch;
    uint32_t fg;
    uint32_t bg;
};

class Terminal {
public:
    Terminal();
    ~Terminal();
    
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
    
    // Output capture for piping
    void start_capture(char* buffer, size_t max_len);
    size_t stop_capture();  // Returns bytes captured
    bool is_capturing() const { return capturing; }

private:
    void scroll_up();
    void new_line();
    void draw_cursor(bool visible);
    void redraw_screen();           // Redraw entire screen from text buffer
    void redraw_row(int row);       // Redraw single row from text buffer
    Cell* get_cell(int col, int row); // Get cell pointer

    int width_chars;
    int height_chars;
    int cursor_col;
    int cursor_row;
    
    uint32_t fg_color;
    uint32_t bg_color;
    
    bool cursor_visible;
    bool cursor_state; // For blinking
    uint64_t last_blink_tick;
    
    // Text buffer for fast scrolling
    Cell* text_buffer;
    int buffer_size;
    
    // Capture mode for piping
    bool capturing;
    char* capture_buffer;
    size_t capture_len;
    size_t capture_max;
};

// Global terminal instance
extern Terminal g_terminal;

