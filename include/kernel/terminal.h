#pragma once
#include <stdint.h>
#include <stddef.h>
#include <libk/kstd.h>

struct Cell {
    char ch;
    uint32_t fg;
    uint32_t bg;
};

class Terminal {
public:
    Terminal();
    ~Terminal() = default;
    
    void init(uint32_t fg_color, uint32_t bg_color);
    
    void put_char(char c);
    void write(const char* str);
    void write_line(const char* str);
    
    void clear();
    void set_color(uint32_t fg, uint32_t bg);
    void set_cursor_pos(int col, int row);
    void get_cursor_pos(int* col, int* row);
    
    void set_cursor_visible(bool visible);
    [[nodiscard]] bool is_cursor_visible() const { return m_cursor_visible; }
    void update_cursor();
    
    void clear_chars(int col, int row, int count);
    void write_char_at(int col, int row, char c);
    void write_char_at_color(int col, int row, char c, uint32_t fg, uint32_t bg);
    
    void start_capture(char* buffer, size_t max_len);
    [[nodiscard]] size_t stop_capture();
    [[nodiscard]] bool is_capturing() const { return m_capturing; }

private:
    void scroll_up();
    void new_line();
    void draw_cursor(bool visible);
    void redraw_screen();
    void redraw_row(int row);
    [[nodiscard]] Cell* get_cell(int col, int row);

    int m_width_chars = 0;
    int m_height_chars = 0;
    int m_cursor_col = 0;
    int m_cursor_row = 0;
    
    uint32_t m_fg_color = 0xFFFFFFFF;
    uint32_t m_bg_color = 0;
    
    bool m_cursor_visible = true;
    bool m_cursor_state = true;
    uint64_t m_last_blink_tick = 0;
    
    kstd::unique_ptr<Cell[]> m_text_buffer;
    int m_buffer_size = 0;
    
    bool m_capturing = false;
    char* m_capture_buffer = nullptr;
    size_t m_capture_len = 0;
    size_t m_capture_max = 0;
};

extern Terminal g_terminal;
