#include <kernel/terminal.h>
#include <drivers/video/framebuffer.h>
#include <kernel/time/timer.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>

Terminal g_terminal;

static constexpr int CHAR_WIDTH = 9;
static constexpr int CHAR_HEIGHT = 18;
static constexpr int MARGIN_LEFT = 50;
static constexpr int MARGIN_TOP = 50;
static constexpr int MARGIN_BOTTOM = 30;

Terminal::Terminal() = default;

void Terminal::init(uint32_t fg, uint32_t bg) {
    m_fg_color = fg;
    m_bg_color = bg;
    
    uint64_t screen_w = gfx_get_width();
    uint64_t screen_h = gfx_get_height();
    if (screen_w == 0 || screen_h == 0) return;
    
    m_width_chars = (static_cast<int>(screen_w) - MARGIN_LEFT * 2) / CHAR_WIDTH;
    m_height_chars = (static_cast<int>(screen_h) - MARGIN_TOP - MARGIN_BOTTOM) / CHAR_HEIGHT;
    m_buffer_size = m_width_chars * m_height_chars;
    
    m_text_buffer.reset(static_cast<Cell*>(malloc(m_buffer_size * sizeof(Cell))));
    if (m_text_buffer) {
        for (int i = 0; i < m_buffer_size; i++) {
            m_text_buffer[i] = {' ', m_fg_color, m_bg_color};
        }
    }
    clear();
}

[[nodiscard]] Cell* Terminal::get_cell(int col, int row) {
    if (!m_text_buffer || col < 0 || col >= m_width_chars || row < 0 || row >= m_height_chars) return nullptr;
    return &m_text_buffer[row * m_width_chars + col];
}

void Terminal::clear() {
    if (m_text_buffer) {
        for (int i = 0; i < m_buffer_size; i++) {
            m_text_buffer[i] = {' ', m_fg_color, m_bg_color};
        }
    }
    gfx_clear(m_bg_color);
    m_cursor_col = 0;
    m_cursor_row = 0;
}

void Terminal::set_color(uint32_t fg, uint32_t bg) {
    m_fg_color = fg;
    m_bg_color = bg;
}

void Terminal::set_cursor_pos(int col, int row) {
    if (m_cursor_visible) draw_cursor(false);
    
    m_cursor_col = (col < 0) ? 0 : (col >= m_width_chars) ? m_width_chars - 1 : col;
    m_cursor_row = (row < 0) ? 0 : (row >= m_height_chars) ? m_height_chars - 1 : row;
    
    if (m_cursor_visible) draw_cursor(true);
}

void Terminal::get_cursor_pos(int* col, int* row) {
    if (col) *col = m_cursor_col;
    if (row) *row = m_cursor_row;
}

void Terminal::put_char(char c) {
    if (m_capturing) {
        if (m_capture_buffer && m_capture_len < m_capture_max) m_capture_buffer[m_capture_len++] = c;
        return;
    }
    
    if (m_cursor_visible) draw_cursor(false);
    
    if (c == '\n') {
        new_line();
    } else if (c == '\b') {
        if (m_cursor_col > 0) {
            m_cursor_col--;
            if (Cell* cell = get_cell(m_cursor_col, m_cursor_row)) {
                *cell = {' ', m_fg_color, m_bg_color};
            }
            gfx_clear_char(MARGIN_LEFT + m_cursor_col * CHAR_WIDTH, MARGIN_TOP + m_cursor_row * CHAR_HEIGHT, m_bg_color);
        }
    } else if (c >= 32) {
        if (Cell* cell = get_cell(m_cursor_col, m_cursor_row)) {
            *cell = {c, m_fg_color, m_bg_color};
        }
        gfx_draw_char(MARGIN_LEFT + m_cursor_col * CHAR_WIDTH, MARGIN_TOP + m_cursor_row * CHAR_HEIGHT, c, m_fg_color);
        if (++m_cursor_col >= m_width_chars) new_line();
    }
    
    if (m_cursor_visible) {
        draw_cursor(true);
        m_cursor_state = true;
        m_last_blink_tick = timer_get_ticks();
    }
}

void Terminal::write(const char* str) {
    bool was_visible = m_cursor_visible;
    if (was_visible) {
        draw_cursor(false);
        m_cursor_visible = false;
    }
    
    while (*str) put_char(*str++);
    
    if (was_visible) {
        m_cursor_visible = true;
        draw_cursor(true);
        m_cursor_state = true;
        m_last_blink_tick = timer_get_ticks();
    }
}

void Terminal::write_line(const char* str) {
    write(str);
    put_char('\n');
}

void Terminal::new_line() {
    m_cursor_col = 0;
    if (++m_cursor_row >= m_height_chars) {
        scroll_up();
        m_cursor_row = m_height_chars - 1;
    }
}

void Terminal::scroll_up() {
    if (!m_text_buffer || m_width_chars <= 0 || m_height_chars <= 1) return;
    
    int rows_to_move = m_height_chars - 1;
    kstring::memmove(m_text_buffer.get(), m_text_buffer.get() + m_width_chars, rows_to_move * m_width_chars * sizeof(Cell));
    
    int last_row_idx = (m_height_chars - 1) * m_width_chars;
    for (int col = 0; col < m_width_chars; col++) {
        m_text_buffer[last_row_idx + col] = {' ', m_fg_color, m_bg_color};
    }
    
    gfx_scroll_up(CHAR_HEIGHT, m_bg_color);
}

void Terminal::redraw_screen() {
    if (!m_text_buffer || m_width_chars <= 0 || m_height_chars <= 0) return;
    
    for (int row = 0; row < m_height_chars; row++) {
        int y = MARGIN_TOP + row * CHAR_HEIGHT;
        gfx_fill_rect(MARGIN_LEFT, y, m_width_chars * CHAR_WIDTH, CHAR_HEIGHT, m_bg_color);
        for (int col = 0; col < m_width_chars; col++) {
            Cell* cell = &m_text_buffer[row * m_width_chars + col];
            if (cell->ch != ' ') gfx_draw_char(MARGIN_LEFT + col * CHAR_WIDTH, y, cell->ch, cell->fg);
        }
    }
}

void Terminal::redraw_row(int row) {
    if (!m_text_buffer || row < 0 || row >= m_height_chars) return;
    
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_fill_rect(MARGIN_LEFT, y, m_width_chars * CHAR_WIDTH, CHAR_HEIGHT, m_bg_color);
    for (int col = 0; col < m_width_chars; col++) {
        Cell* cell = &m_text_buffer[row * m_width_chars + col];
        if (cell->ch != ' ') gfx_draw_char(MARGIN_LEFT + col * CHAR_WIDTH, y, cell->ch, cell->fg);
    }
}

void Terminal::draw_cursor(bool visible) {
    constexpr int cursor_height = 2;
    int x = MARGIN_LEFT + m_cursor_col * CHAR_WIDTH;
    int y = MARGIN_TOP + m_cursor_row * CHAR_HEIGHT + CHAR_HEIGHT - cursor_height;
    gfx_fill_rect(x, y, CHAR_WIDTH, cursor_height, visible ? COLOR_WHITE : m_bg_color);
}

void Terminal::set_cursor_visible(bool visible) {
    if (m_cursor_visible == visible) return;
    if (visible) {
        m_cursor_visible = true;
        m_cursor_state = true;
        m_last_blink_tick = timer_get_ticks();
        draw_cursor(true);
    } else {
        draw_cursor(false);
        m_cursor_visible = false;
    }
}

void Terminal::update_cursor() {
    if (!m_cursor_visible) return;
    uint64_t now = timer_get_ticks();
    if (now - m_last_blink_tick > 500) {
        m_last_blink_tick = now;
        m_cursor_state = !m_cursor_state;
        draw_cursor(m_cursor_state);
    }
}

void Terminal::clear_chars(int col, int row, int count) {
    for (int i = 0; i < count; i++) {
        if (Cell* cell = get_cell(col + i, row)) *cell = {' ', m_fg_color, m_bg_color};
    }
    gfx_fill_rect(MARGIN_LEFT + col * CHAR_WIDTH, MARGIN_TOP + row * CHAR_HEIGHT, count * CHAR_WIDTH, CHAR_HEIGHT, m_bg_color);
}

void Terminal::write_char_at(int col, int row, char c) {
    if (Cell* cell = get_cell(col, row)) *cell = {c, m_fg_color, m_bg_color};
    gfx_draw_char(MARGIN_LEFT + col * CHAR_WIDTH, MARGIN_TOP + row * CHAR_HEIGHT, c, m_fg_color);
}

void Terminal::write_char_at_color(int col, int row, char c, uint32_t fg, uint32_t bg) {
    if (Cell* cell = get_cell(col, row)) *cell = {c, fg, bg};
    int x = MARGIN_LEFT + col * CHAR_WIDTH;
    int y = MARGIN_TOP + row * CHAR_HEIGHT;
    gfx_fill_rect(x, y, CHAR_WIDTH, CHAR_HEIGHT, bg);
    gfx_draw_char(x, y, c, fg);
}

void Terminal::start_capture(char* buffer, size_t max_len) {
    m_capture_buffer = buffer;
    m_capture_max = max_len;
    m_capture_len = 0;
    m_capturing = true;
}

size_t Terminal::stop_capture() {
    m_capturing = false;
    size_t len = m_capture_len;
    if (m_capture_buffer && m_capture_len < m_capture_max) m_capture_buffer[m_capture_len] = '\0';
    m_capture_buffer = nullptr;
    m_capture_len = 0;
    m_capture_max = 0;
    return len;
}
