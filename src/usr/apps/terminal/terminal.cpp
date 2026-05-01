#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <uapi/sysinfo.h>
#include <unistd.h>

#include "../../libc/config_utils.h"
#include "../../libc/syscall.h"
#include "../../libc/wallpaper_defaults.h"
#include "../../libgui/gui.h"

struct Cell
{
    char ch;
    uint32_t fg;
    uint32_t bg;
};

static inline uint32_t term_bg()
{
    return g_gui_style.app_surface;
}
static inline uint32_t term_frame_bg()
{
    return g_gui_style.app_bg;
}
static inline uint32_t term_fg()
{
    return g_gui_style.text;
}
static inline uint32_t term_cursor()
{
    return g_gui_style.accent;
}
static constexpr int TERM_CURSOR_H = 2;
static constexpr const char *WALLPAPER_CONFIG_PATH = "/data/WALLPAPR.CFG";
static constexpr const char *WALLPAPER_BOOTSTRAP_CONFIG_PATH = "/etc/wallpaper.conf";
static constexpr const char *SYSTEM_CONFIG_PATH = "/data/SYSTEM.CFG";
static constexpr const char *SYSTEM_BOOTSTRAP_CONFIG_PATH = "/etc/system.conf";
static constexpr uint32_t TERM_HISTORY_LINES = 2048;
static constexpr uint32_t TERM_HISTORY_LINE_LEN = 1024;
static constexpr uint32_t TERM_MAX_VISIBLE_ROWS = 512;
static constexpr uint8_t KEY_UP_ARROW = 0x80;
static constexpr uint8_t KEY_DOWN_ARROW = 0x81;
static constexpr uint8_t KEY_LEFT_ARROW = 0x82;
static constexpr uint8_t KEY_RIGHT_ARROW = 0x83;
static constexpr uint8_t KEY_HOME = 0x84;
static constexpr uint8_t KEY_END = 0x85;
static constexpr uint8_t KEY_DELETE = 0x86;

static inline const GuiFont *term_font()
{
    const GuiFont *font = gui_font_mono();
    if (font)
        return font;
    return gui_font_default();
}

static inline uint32_t term_cell_w()
{
    int width = gui_font_mono_cell_width(term_font());
    return width > 0 ? (uint32_t)width : 8u;
}

static inline uint32_t term_cell_h()
{
    int height = gui_font_line_height(term_font());
    return height > 0 ? (uint32_t)height : 16u;
}

static inline int term_pad_x()
{
    return gui_space_2();
}

static inline int term_pad_y()
{
    return gui_space_2();
}

static inline int32_t term_content_x()
{
    return term_pad_x();
}

static inline int32_t term_content_y()
{
    return term_pad_y();
}

static int term_ansi_param_at(const char *buf, int wanted, int fallback)
{
    if (!buf || buf[0] == '\0')
        return fallback;
    int index = 0;
    int value = 0;
    bool have = false;
    for (int i = 0;; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            have = true;
        } else if (c == ';' || c == '\0') {
            if (index == wanted)
                return have ? value : fallback;
            index++;
            value = 0;
            have = false;
            if (c == '\0')
                break;
        } else {
            break;
        }
    }
    return fallback;
}

static uint32_t term_ansi_color(int code)
{
    switch (code) {
        case 30:
            return 0xFF1F2328;
        case 31:
            return 0xFFFF453A;
        case 32:
            return 0xFF30D158;
        case 33:
            return 0xFFFFD60A;
        case 34:
            return 0xFF0A84FF;
        case 35:
            return 0xFFBF5AF2;
        case 36:
            return 0xFF5AC8FA;
        case 37:
            return 0xFFE5E5EA;
        case 90:
            return g_gui_style.text_dim;
        case 91:
            return 0xFFFF6961;
        case 92:
            return 0xFF63E6BE;
        case 93:
            return 0xFFFFE066;
        case 94:
            return 0xFF64D2FF;
        case 95:
            return 0xFFD0A8FF;
        case 96:
            return 0xFF70D7FF;
        case 97:
            return 0xFFFFFFFF;
        default:
            return term_fg();
    }
}

static void term_draw_char(Surface *s, int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg)
{
    const GuiFont *font = term_font();
    if (font) {
        gui_draw_mono_cell(s, font, x, y, (int32_t)term_cell_w(), (int32_t)term_cell_h(), c, fg, bg);
    } else {
        gui_draw_char(s, x, y, c, fg, bg);
    }
}

class TerminalEmulator
{
public:
    TerminalEmulator()
        : m_grid(nullptr), m_presented_grid(nullptr), m_history_len(nullptr), m_history_text(nullptr),
          m_history_fg(nullptr), m_ready(false), m_cursor_visible(false)
    {
    }

    ~TerminalEmulator()
    {
        if (m_grid)
            free(m_grid);
        if (m_presented_grid)
            free(m_presented_grid);
        if (m_history_len)
            free(m_history_len);
        if (m_history_text)
            free(m_history_text);
        if (m_history_fg)
            free(m_history_fg);
    }

    void init(uint32_t width, uint32_t height)
    {
        gui_fonts_init();
        gui_sync_theme_from_registry();
        m_cursor_x = 0;
        m_cursor_y = 0;
        m_fg = term_fg();
        m_bg = term_bg();
        m_ansi_state = 0;
        m_ansi_idx = 0;
        memset(m_ansi_buf, 0, sizeof(m_ansi_buf));
        memset(&m_window, 0, sizeof(m_window));

        m_history_len = (uint16_t *)malloc(sizeof(uint16_t) * TERM_HISTORY_LINES);
        m_history_text = (char *)malloc((size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN);
        m_history_fg = (uint32_t *)malloc((size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        if (!m_history_len || !m_history_text || !m_history_fg)
            return;
        memset(m_history_len, 0, sizeof(uint16_t) * TERM_HISTORY_LINES);
        memset(m_history_text, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN);
        memset(m_history_fg, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        reset_history();

        m_window = gui_register_window_ex("Terminal", width * term_cell_w() + term_pad_x() * 2,
                                          height * term_cell_h() + term_pad_y() * 2, WIN_FLAG_RESIZABLE);
        if (!m_window.buffer)
            return;
        gui_window_set_min_size((int)(term_cell_w() * 48u + (uint32_t)term_pad_x() * 2u),
                                (int)(term_cell_h() * 14u + (uint32_t)term_pad_y() * 2u));

        if (!resize_grid(width, height))
            return;
        draw_chrome();
        m_needs_full_redraw = true;
        m_ready = true;
    }

    bool ready() const
    {
        return m_ready;
    }

    void clear_screen()
    {
        reset_history();
    }

    void put_char(char c)
    {
        if (m_ansi_state == 1) {
            if (c == '[') {
                m_ansi_state = 2;
                m_ansi_idx = 0;
            } else {
                m_ansi_state = 0;
            }
            return;
        } else if (m_ansi_state == 2) {
            if ((c >= '0' && c <= '9') || c == ';') {
                if (m_ansi_idx < 31)
                    m_ansi_buf[m_ansi_idx++] = c;
                return;
            }
            m_ansi_buf[m_ansi_idx] = '\0';
            handle_ansi(c);
            m_ansi_state = 0;
            return;
        }

        if (c == '\x1b') {
            m_ansi_state = 1;
            return;
        }

        if (c == '\n') {
            new_line();
        } else if (c == '\r') {
            m_history_cursor_col = 0;
        } else if (c == '\b' || c == 127) {
            history_backspace();
        } else if (c >= 32) {
            append_history_char(c);
        }
    }

    void write_string(const char *str)
    {
        if (!str)
            return;
        while (*str)
            put_char(*str++);
    }

    void write_bytes(const char *data, size_t len)
    {
        if (!data)
            return;
        for (size_t i = 0; i < len; i++)
            put_char(data[i]);
    }

    void render_all()
    {
        if (!m_ready)
            return;

        rebuild_grid_from_history();
        int32_t dirty_x1 = (int32_t)m_window.width;
        int32_t dirty_y1 = (int32_t)m_window.height;
        int32_t dirty_x2 = 0;
        int32_t dirty_y2 = 0;
        bool has_dirty = false;

        auto mark_dirty_cell = [&](uint32_t x, uint32_t y) {
            int32_t px = term_content_x() + (int32_t)(x * term_cell_w());
            int32_t py = term_content_y() + (int32_t)(y * term_cell_h());
            if (!has_dirty) {
                dirty_x1 = px;
                dirty_y1 = py;
                dirty_x2 = px + (int32_t)term_cell_w();
                dirty_y2 = py + (int32_t)term_cell_h();
                has_dirty = true;
                return;
            }
            if (px < dirty_x1)
                dirty_x1 = px;
            if (py < dirty_y1)
                dirty_y1 = py;
            if (px + (int32_t)term_cell_w() > dirty_x2)
                dirty_x2 = px + (int32_t)term_cell_w();
            if (py + (int32_t)term_cell_h() > dirty_y2)
                dirty_y2 = py + (int32_t)term_cell_h();
        };

        auto redraw_cell = [&](uint32_t x, uint32_t y) {
            if (x >= m_width || y >= m_height)
                return;
            const Cell &cell = m_grid[y * m_width + x];
            int32_t px = term_content_x() + (int32_t)(x * term_cell_w());
            int32_t py = term_content_y() + (int32_t)(y * term_cell_h());
            gui_fill_rect(&m_window, px, py, term_cell_w(), term_cell_h(), cell.bg);
            if (cell.ch != ' ') {
                term_draw_char(&m_window, px, py, cell.ch, cell.fg, cell.bg);
            }
            mark_dirty_cell(x, y);
        };

        if (m_needs_full_redraw || !m_presented_grid) {
            draw_chrome();
            for (uint32_t y = 0; y < m_height; y++) {
                for (uint32_t x = 0; x < m_width; x++) {
                    const Cell &cell = m_grid[y * m_width + x];
                    if (cell.bg != term_bg()) {
                        gui_fill_rect(&m_window, term_content_x() + (int32_t)(x * term_cell_w()),
                                      term_content_y() + (int32_t)(y * term_cell_h()), term_cell_w(), term_cell_h(),
                                      cell.bg);
                    }
                    if (cell.ch != ' ') {
                        term_draw_char(&m_window, term_content_x() + (int32_t)(x * term_cell_w()),
                                       term_content_y() + (int32_t)(y * term_cell_h()), cell.ch, cell.fg, cell.bg);
                    }
                    m_presented_grid[y * m_width + x] = cell;
                }
            }
            dirty_x1 = 0;
            dirty_y1 = 0;
            dirty_x2 = (int32_t)m_window.width;
            dirty_y2 = (int32_t)m_window.height;
            has_dirty = true;
        } else {
            if (m_presented_cursor_visible && m_presented_cursor_x < m_width && m_presented_cursor_y < m_height) {
                redraw_cell(m_presented_cursor_x, m_presented_cursor_y);
            }

            for (uint32_t y = 0; y < m_height; y++) {
                for (uint32_t x = 0; x < m_width; x++) {
                    uint32_t idx = y * m_width + x;
                    const Cell &current = m_grid[idx];
                    Cell &shown = m_presented_grid[idx];
                    if (shown.ch == current.ch && shown.fg == current.fg && shown.bg == current.bg)
                        continue;
                    redraw_cell(x, y);
                    shown = current;
                }
            }
        }

        if (m_cursor_visible && m_cursor_x < m_width && m_cursor_y < m_height) {
            gui_fill_rect(&m_window, term_content_x() + (int32_t)(m_cursor_x * term_cell_w()),
                          term_content_y() + (int32_t)(m_cursor_y * term_cell_h() + term_cell_h() - TERM_CURSOR_H),
                          term_cell_w(), TERM_CURSOR_H, term_cursor());
            mark_dirty_cell(m_cursor_x, m_cursor_y);
        }
        asm volatile("sfence" ::: "memory");
        if (has_dirty) {
            gui_blit_to_screen_rect(&m_window, dirty_x1, dirty_y1, dirty_x2 - dirty_x1, dirty_y2 - dirty_y1);
        }
        m_presented_cursor_visible = m_cursor_visible;
        m_presented_cursor_x = m_cursor_x;
        m_presented_cursor_y = m_cursor_y;
        m_needs_full_redraw = false;
    }

    bool sync_resize()
    {
        if (!m_ready)
            return false;
        int resized = gui_sync_window_size(&m_window);
        if (resized <= 0)
            return false;

        uint32_t content_w =
            (m_window.width > (uint32_t)(term_pad_x() * 2)) ? (m_window.width - (uint32_t)(term_pad_x() * 2)) : 0;
        uint32_t content_h =
            (m_window.height > (uint32_t)(term_pad_y() * 2)) ? (m_window.height - (uint32_t)(term_pad_y() * 2)) : 0;
        uint32_t new_width = content_w / term_cell_w();
        uint32_t new_height = content_h / term_cell_h();
        if (new_width == 0)
            new_width = 1;
        if (new_height == 0)
            new_height = 1;
        return resize_grid(new_width, new_height);
    }

    void theme_changed()
    {
        m_fg = term_fg();
        m_bg = term_bg();
        m_needs_full_redraw = true;
    }

private:
    void draw_chrome()
    {
        gui_fill_surface(&m_window, term_frame_bg());
        gui_draw_panel_inset(&m_window, term_pad_x() / 2, term_pad_y() / 2, (int)m_window.width - term_pad_x(),
                             (int)m_window.height - term_pad_y(), term_bg(), g_gui_style.border,
                             g_gui_style.chrome_bg_alt);
    }

    char *history_line(uint32_t index)
    {
        return m_history_text + ((size_t)index * TERM_HISTORY_LINE_LEN);
    }

    const char *history_line(uint32_t index) const
    {
        return m_history_text + ((size_t)index * TERM_HISTORY_LINE_LEN);
    }

    uint32_t *history_line_fg(uint32_t index)
    {
        return m_history_fg + ((size_t)index * TERM_HISTORY_LINE_LEN);
    }

    void reset_history()
    {
        if (!m_history_len || !m_history_text || !m_history_fg)
            return;
        memset(m_history_len, 0, sizeof(uint16_t) * TERM_HISTORY_LINES);
        memset(m_history_text, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN);
        memset(m_history_fg, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        m_history_count = 1;
        m_history_cursor_col = 0;
        m_cursor_x = 0;
        m_cursor_y = 0;
        m_cursor_visible = true;
        m_needs_full_redraw = true;
        if (m_grid && m_width > 0 && m_height > 0)
            clear_grid();
    }

    void push_history_line()
    {
        if (m_history_count >= TERM_HISTORY_LINES) {
            memmove(m_history_len, m_history_len + 1, sizeof(uint16_t) * (TERM_HISTORY_LINES - 1));
            memmove(m_history_text, m_history_text + TERM_HISTORY_LINE_LEN,
                    (size_t)(TERM_HISTORY_LINES - 1) * TERM_HISTORY_LINE_LEN);
            memmove(m_history_fg, m_history_fg + TERM_HISTORY_LINE_LEN,
                    (size_t)(TERM_HISTORY_LINES - 1) * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
            m_history_count = TERM_HISTORY_LINES - 1;
        }

        char *next_line = history_line(m_history_count);
        memset(next_line, 0, TERM_HISTORY_LINE_LEN);
        memset(history_line_fg(m_history_count), 0, TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        m_history_len[m_history_count] = 0;
        m_history_count++;
        m_history_cursor_col = 0;
    }

    void append_history_char(char c)
    {
        if (m_history_count == 0)
            reset_history();
        uint32_t line_index = m_history_count - 1;
        uint16_t len = m_history_len[line_index];
        uint32_t cursor_col = m_history_cursor_col;
        if (cursor_col >= TERM_HISTORY_LINE_LEN - 1) {
            push_history_line();
            line_index = m_history_count - 1;
            len = 0;
            cursor_col = 0;
        }

        char *line = history_line(line_index);
        uint32_t *fg = history_line_fg(line_index);
        line[cursor_col] = c;
        fg[cursor_col] = m_fg;
        uint16_t new_len = len;
        if (cursor_col >= len) {
            new_len = (uint16_t)(cursor_col + 1);
            line[new_len] = '\0';
        }
        m_history_len[line_index] = new_len;
        m_history_cursor_col = cursor_col + 1;
    }

    void history_backspace()
    {
        if (m_history_count == 0 || m_history_cursor_col == 0)
            return;
        uint32_t line_index = m_history_count - 1;
        uint16_t len = m_history_len[line_index];
        uint32_t cursor_col = m_history_cursor_col - 1;
        char *line = history_line(line_index);
        uint32_t *fg = history_line_fg(line_index);
        if (cursor_col < len) {
            memmove(line + cursor_col, line + cursor_col + 1, (size_t)(len - cursor_col));
            memmove(fg + cursor_col, fg + cursor_col + 1, (size_t)(len - cursor_col) * sizeof(uint32_t));
            len--;
            line[len] = '\0';
            m_history_len[line_index] = len;
        }
        m_history_cursor_col = cursor_col;
    }

    void rebuild_grid_from_history()
    {
        if (!m_grid || m_width == 0 || m_height == 0)
            return;

        struct VisualSlice
        {
            uint32_t line_index;
            uint32_t start_col;
            uint32_t len;
        };

        clear_grid();
        m_cursor_visible = false;

        VisualSlice visible[TERM_MAX_VISIBLE_ROWS];
        uint32_t visible_count = 0;
        uint32_t visible_limit = (m_height < TERM_MAX_VISIBLE_ROWS) ? m_height : TERM_MAX_VISIBLE_ROWS;
        uint32_t cursor_line = (m_history_count > 0) ? (m_history_count - 1) : 0;
        uint32_t cursor_start_col = (m_width > 0) ? ((m_history_cursor_col / m_width) * m_width) : 0;
        int cursor_slice = -1;

        for (int line = (int)m_history_count - 1; line >= 0 && visible_count < visible_limit; line--) {
            uint32_t len = m_history_len[line];
            uint32_t wraps = (len == 0) ? 1 : ((len + m_width - 1) / m_width);
            for (int seg = (int)wraps - 1; seg >= 0 && visible_count < visible_limit; seg--) {
                uint32_t start_col = (uint32_t)seg * m_width;
                uint32_t seg_len = 0;
                if (len > start_col) {
                    seg_len = len - start_col;
                    if (seg_len > m_width)
                        seg_len = m_width;
                }
                visible[visible_count] = {(uint32_t)line, start_col, seg_len};
                if ((uint32_t)line == cursor_line && start_col == cursor_start_col) {
                    cursor_slice = (int)visible_count;
                }
                visible_count++;
            }
        }

        for (uint32_t row = 0; row < visible_count; row++) {
            const VisualSlice &slice = visible[visible_count - 1 - row];
            const char *line = history_line(slice.line_index);
            uint32_t *fg = history_line_fg(slice.line_index);
            for (uint32_t col = 0; col < slice.len; col++) {
                uint32_t color = fg[slice.start_col + col] ? fg[slice.start_col + col] : term_fg();
                set_cell(col, row, line[slice.start_col + col], color, term_bg());
            }
        }

        if (cursor_slice >= 0) {
            m_cursor_x = (m_width > 0) ? (m_history_cursor_col % m_width) : 0;
            m_cursor_y = visible_count - 1 - (uint32_t)cursor_slice;
            if (m_cursor_y < m_height) {
                m_cursor_visible = true;
            }
        }
    }

    bool resize_grid(uint32_t new_width, uint32_t new_height)
    {
        if (new_width == 0 || new_height == 0)
            return false;

        Cell *new_grid = (Cell *)malloc(new_width * new_height * sizeof(Cell));
        Cell *new_presented = (Cell *)malloc(new_width * new_height * sizeof(Cell));
        if (!new_grid || !new_presented) {
            if (new_grid)
                free(new_grid);
            if (new_presented)
                free(new_presented);
            return false;
        }

        for (uint32_t i = 0; i < new_width * new_height; i++) {
            new_grid[i] = {' ', m_fg, m_bg};
            new_presented[i] = {'\0', 0, 0};
        }
        if (m_grid)
            free(m_grid);
        if (m_presented_grid)
            free(m_presented_grid);

        m_grid = new_grid;
        m_presented_grid = new_presented;
        m_width = new_width;
        m_height = new_height;
        m_presented_cursor_visible = false;
        m_needs_full_redraw = true;
        rebuild_grid_from_history();
        return true;
    }

    void clear_grid()
    {
        for (uint32_t i = 0; i < m_width * m_height; i++) {
            m_grid[i] = {' ', m_fg, m_bg};
        }
    }

    void set_cell(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg)
    {
        if (x >= m_width || y >= m_height)
            return;
        m_grid[y * m_width + x] = {c, fg, bg};
    }

    void new_line()
    {
        push_history_line();
    }

    void handle_ansi(char final_char)
    {
        if (final_char == 'J') {
            if (strcmp(m_ansi_buf, "2") == 0) {
                clear_screen();
            }
        } else if (final_char == 'H') {
            reset_history();
        } else if (final_char == 'K') {
            if (m_history_count == 0)
                return;
            uint32_t line_index = m_history_count - 1;
            char *line = history_line(line_index);
            uint32_t *fg = history_line_fg(line_index);
            uint16_t len = m_history_len[line_index];
            int mode = term_ansi_param_at(m_ansi_buf, 0, 0);
            uint32_t start = mode == 2 ? 0 : m_history_cursor_col;
            if (start < len) {
                memset(line + start, 0, (size_t)(len - start));
                memset(fg + start, 0, (size_t)(len - start) * sizeof(uint32_t));
                m_history_len[line_index] = (uint16_t)start;
                if (m_history_cursor_col > start)
                    m_history_cursor_col = start;
                m_needs_full_redraw = true;
            }
        } else if (final_char == 'C') {
            int n = term_ansi_param_at(m_ansi_buf, 0, 1);
            uint32_t line_index = m_history_count > 0 ? m_history_count - 1 : 0;
            uint32_t len = m_history_count > 0 ? m_history_len[line_index] : 0;
            m_history_cursor_col = (m_history_cursor_col + (uint32_t)n > len) ? len : m_history_cursor_col + n;
        } else if (final_char == 'D') {
            int n = term_ansi_param_at(m_ansi_buf, 0, 1);
            m_history_cursor_col = ((uint32_t)n > m_history_cursor_col) ? 0 : m_history_cursor_col - (uint32_t)n;
        } else if (final_char == 'm') {
            if (m_ansi_buf[0] == '\0') {
                m_fg = term_fg();
                m_bg = term_bg();
                return;
            }
            for (int pi = 0;; pi++) {
                int code = term_ansi_param_at(m_ansi_buf, pi, pi == 0 ? 0 : -1);
                if (code < 0)
                    break;
                if (code == 0) {
                    m_fg = term_fg();
                    m_bg = term_bg();
                } else if (code == 39) {
                    m_fg = term_fg();
                } else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
                    m_fg = term_ansi_color(code);
                }
            }
        }
    }

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_cursor_x = 0;
    uint32_t m_cursor_y = 0;
    uint32_t m_fg = 0;
    uint32_t m_bg = 0;
    Cell *m_grid;
    Cell *m_presented_grid;
    uint16_t *m_history_len;
    char *m_history_text;
    uint32_t *m_history_fg;
    uint32_t m_history_count = 0;
    uint32_t m_history_cursor_col = 0;
    Surface m_window;
    bool m_ready;
    bool m_cursor_visible;
    bool m_presented_cursor_visible = false;
    bool m_needs_full_redraw = true;
    uint32_t m_presented_cursor_x = 0;
    uint32_t m_presented_cursor_y = 0;
    int m_ansi_state = 0;
    int m_ansi_idx = 0;
    char m_ansi_buf[32];
};

static void term_printf(TerminalEmulator &term, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term.write_string(buf);
}

static void resolve_path(const char *cwd, const char *path, char *out)
{
    char temp_path[256];

    if (!path || path[0] == '\0') {
        strncpy(out, cwd, 255);
        out[255] = '\0';
        return;
    }

    if (path[0] == '/') {
        strncpy(temp_path, path, sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
    } else {
        strncpy(temp_path, cwd, 255);
        temp_path[255] = '\0';
        if (temp_path[0] == '\0') {
            strncpy(temp_path, "/", sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
        }
        size_t len = strlen(temp_path);
        if (len == 0 || temp_path[len - 1] != '/') {
            strncat(temp_path, "/", sizeof(temp_path) - strlen(temp_path) - 1);
        }
        strncat(temp_path, path, sizeof(temp_path) - strlen(temp_path) - 1);
    }

    char *segments[32];
    int depth = 0;
    char copy[256];
    strncpy(copy, temp_path, 255);
    copy[255] = '\0';

    char *tok = copy;
    if (*tok == '/')
        tok++;

    char *p = tok;
    while (true) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            *p = '\0';

            if (strcmp(tok, "..") == 0) {
                if (depth > 0)
                    depth--;
            } else if (strcmp(tok, ".") != 0 && tok[0] != '\0') {
                if (depth < 32)
                    segments[depth++] = tok;
            }

            if (saved == '\0')
                break;
            tok = p + 1;
            p = tok;
            continue;
        }
        p++;
    }

    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        strncat(out, "/", 255 - strlen(out));
        strncat(out, segments[i], 255 - strlen(out));
    }
    if (out[0] == '\0') {
        strncpy(out, "/", 255);
        out[255] = '\0';
    }
}

static int prompt_len_for(const char *cwd)
{
    return (int)strlen(cwd) + 14;
}

static void print_prompt(TerminalEmulator &term, const char *cwd)
{
    term.write_string("\x1b[90mroot@unios\x1b[0m \x1b[34m");
    term.write_string(cwd);
    term.write_string("\x1b[0m $ ");
}

static void choose_initial_cwd(char *cwd, size_t cwd_size)
{
    if (!cwd || cwd_size == 0)
        return;

    VNodeStat st = {};
    const char *initial = (stat("/data", &st) == 0 && st.is_dir) ? "/data" : "/";
    strncpy(cwd, initial, cwd_size - 1);
    cwd[cwd_size - 1] = '\0';
}

struct TermCommand
{
    const char *name;
    const char *usage;
    const char *description;
};

static const TermCommand k_term_commands[] = {
    {"help", "help", "show command reference"},
    {"clear", "clear", "clear the terminal"},
    {"exit", "exit", "close terminal"},
    {"echo", "echo <text>", "print text"},
    {"pwd", "pwd", "print working directory"},
    {"cd", "cd [dir]", "change directory"},
    {"ls", "ls [dir]", "list files"},
    {"cat", "cat <file>", "print file"},
    {"stat", "stat <file>", "show file metadata"},
    {"touch", "touch <file>", "create a file"},
    {"rm", "rm <file>", "remove a file"},
    {"mkdir", "mkdir <dir>", "create a directory"},
    {"rmdir", "rmdir <dir>", "remove an empty directory"},
    {"mv", "mv <src> <dst>", "rename a path"},
    {"cp", "cp <src> <dst>", "copy a file"},
    {"mem", "mem", "show memory"},
    {"ps", "ps", "list processes"},
    {"date", "date", "show time"},
    {"uptime", "uptime", "show uptime"},
    {"version", "version", "show kernel version"},
    {"uname", "uname", "show system info"},
    {"wallpaper", "wallpaper [file]", "show or set wallpaper"},
};

static void cmd_help(TerminalEmulator &term)
{
    term_printf(term, "uniOS terminal commands (%d)\n", (int)(sizeof(k_term_commands) / sizeof(k_term_commands[0])));
    term.write_string("\x1b[90m  command              description\x1b[0m\n");
    for (uint32_t i = 0; i < sizeof(k_term_commands) / sizeof(k_term_commands[0]); i++)
        term_printf(term, "  %-20s %s\n", k_term_commands[i].usage, k_term_commands[i].description);
}

static void cmd_pwd(TerminalEmulator &term, const char *cwd)
{
    term.write_string(cwd);
    term.put_char('\n');
}

static void cmd_cd(TerminalEmulator &term, char *cwd, const char *arg)
{
    char resolved[256];
    resolve_path(cwd, arg && *arg ? arg : "/", resolved);
    VNodeStat st = {};
    if (stat(resolved, &st) < 0 || !st.is_dir) {
        term_printf(term, "cd: no such directory: %s\n", arg && *arg ? arg : "/");
        return;
    }
    strncpy(cwd, resolved, 255);
    cwd[255] = '\0';
}

static void cmd_ls(TerminalEmulator &term, const char *cwd, const char *arg)
{
    char resolved[256];
    resolve_path(cwd, (arg && *arg) ? arg : cwd, resolved);

    VNodeStat dir_stat = {};
    if (stat(resolved, &dir_stat) < 0) {
        term_printf(term, "ls: cannot access '%s'\n", resolved);
        return;
    }
    if (!dir_stat.is_dir) {
        term_printf(term, "%s\n", resolved);
        return;
    }

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        term_printf(term, "ls: cannot access '%s'\n", resolved);
        return;
    }

    char name[256];
    bool first = true;
    while ((int)syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char full_path[512];
        strncpy(full_path, resolved, sizeof(full_path) - 1);
        full_path[sizeof(full_path) - 1] = '\0';
        size_t len = strlen(full_path);
        if (len == 0 || full_path[len - 1] != '/') {
            strncat(full_path, "/", sizeof(full_path) - strlen(full_path) - 1);
        }
        strncat(full_path, name, sizeof(full_path) - strlen(full_path) - 1);

        VNodeStat st = {};
        if (!first)
            term.write_string("  ");
        if (stat(full_path, &st) == 0 && st.is_dir) {
            term.write_string("\x1b[34m");
            term.write_string(name);
            term.write_string("/\x1b[0m");
        } else {
            term.write_string(name);
        }
        first = false;
    }
    term.put_char('\n');
    close(fd);
}

static void cmd_cat(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        term.write_string("cat: missing file operand\n");
        return;
    }

    char resolved[256];
    resolve_path(cwd, arg, resolved);
    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        term_printf(term, "cat: %s: No such file or directory\n", arg);
        return;
    }

    char buffer[512];
    int bytes_read;
    bool ended_with_newline = false;
    while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        term.write_string(buffer);
        ended_with_newline = buffer[bytes_read - 1] == '\n';
    }
    if (!ended_with_newline)
        term.put_char('\n');
    close(fd);
}

static void cmd_stat(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        term.write_string("stat: missing file operand\n");
        return;
    }

    char resolved[256];
    resolve_path(cwd, arg, resolved);
    VNodeStat st = {};
    if (stat(resolved, &st) < 0) {
        term_printf(term, "stat: cannot stat '%s'\n", arg);
        return;
    }

    term_printf(term, "File: %s\n", arg);
    term_printf(term, "Size: %llu bytes\n", st.size);
    term_printf(term, "Type: %s\n", st.is_dir ? "Directory" : "Regular File");
    term_printf(term, "Inode: %llu\n", st.inode);
}

static void cmd_touch(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        term.write_string("touch: missing file operand\n");
        return;
    }

    char resolved[256];
    resolve_path(cwd, arg, resolved);
    int fd = open(resolved, O_WRONLY | O_CREAT);
    if (fd < 0) {
        term_printf(term, "touch: failed to create '%s'\n", arg);
        return;
    }
    close(fd);
}

static void cmd_rm(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        term.write_string("rm: missing file operand\n");
        return;
    }

    char resolved[256];
    resolve_path(cwd, arg, resolved);
    if (unlink(resolved) != 0) {
        term_printf(term, "rm: failed to delete '%s'\n", arg);
    }
}

static void cmd_mkdir(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        term.write_string("mkdir: missing directory operand\n");
        return;
    }

    char resolved[256];
    resolve_path(cwd, arg, resolved);
    if (mkdir(resolved) != 0) {
        term_printf(term, "mkdir: failed to create '%s'\n", arg);
    }
}

static const char *term_parse_token(const char *s, char *out, size_t out_size)
{
    while (s && *s == ' ')
        s++;
    if (!s)
        s = "";
    size_t i = 0;
    while (*s && *s != ' ' && i + 1 < out_size)
        out[i++] = *s++;
    out[i] = '\0';
    while (*s == ' ')
        s++;
    return s;
}

static void cmd_rmdir(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        term.write_string("rmdir: missing directory operand\n");
        return;
    }
    char resolved[256];
    resolve_path(cwd, arg, resolved);
    if (rmdir(resolved) != 0)
        term_printf(term, "rmdir: failed to remove '%s'\n", arg);
}

static void cmd_mv(TerminalEmulator &term, const char *cwd, const char *arg)
{
    char src[256], dst[256];
    const char *p = term_parse_token(arg, src, sizeof(src));
    term_parse_token(p, dst, sizeof(dst));
    if (!src[0] || !dst[0]) {
        term.write_string("Usage: mv <src> <dst>\n");
        return;
    }
    char rsrc[256], rdst[256];
    resolve_path(cwd, src, rsrc);
    resolve_path(cwd, dst, rdst);
    if (rename(rsrc, rdst) != 0)
        term_printf(term, "mv: failed to move '%s'\n", src);
}

static void cmd_cp(TerminalEmulator &term, const char *cwd, const char *arg)
{
    char src[256], dst[256];
    const char *p = term_parse_token(arg, src, sizeof(src));
    term_parse_token(p, dst, sizeof(dst));
    if (!src[0] || !dst[0]) {
        term.write_string("Usage: cp <src> <dst>\n");
        return;
    }

    char rsrc[256], rdst[256];
    resolve_path(cwd, src, rsrc);
    resolve_path(cwd, dst, rdst);
    int in = open(rsrc, O_RDONLY);
    if (in < 0) {
        term_printf(term, "cp: cannot open '%s'\n", src);
        return;
    }
    int out = open(rdst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        term_printf(term, "cp: cannot create '%s'\n", dst);
        return;
    }
    char buf[1024];
    int n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, (size_t)n);
    close(in);
    close(out);
}

static void cmd_mem(TerminalEmulator &term)
{
    MemInfo info = {};
    if (get_meminfo(&info) != 0) {
        term.write_string("mem: failed to get memory info\n");
        return;
    }
    term_printf(term, "Total: %llu KB\n", info.total_kb);
    term_printf(term, "Used:  %llu KB\n", info.used_kb);
    term_printf(term, "Free:  %llu KB\n", info.free_kb);
}

static void cmd_ps(TerminalEmulator &term)
{
    ProcessInfo procs[32];
    int count = get_procs(procs, 32);
    if (count < 0) {
        term.write_string("ps: failed to get process list\n");
        return;
    }

    term.write_string("\x1b[90mPID  PPID  STATE   PRI  UID  NAME\x1b[0m\n");
    for (int i = 0; i < count; i++) {
        const char *state_str = "READY";
        switch ((int)procs[i].state) {
            case ProcessState_Running:
                state_str = "RUN";
                break;
            case ProcessState_Blocked:
                state_str = "BLOCK";
                break;
            case ProcessState_Sleeping:
                state_str = "SLEEP";
                break;
            case ProcessState_Zombie:
                state_str = "ZOMBIE";
                break;
            case ProcessState_Waiting:
                state_str = "WAIT";
                break;
            default:
                break;
        }
        term_printf(term, "%-4d %-5d %-7s %-4d %-4d %s\n", (int)procs[i].pid, (int)procs[i].parent_pid, state_str,
                    (int)procs[i].priority, (int)procs[i].uid, procs[i].name);
    }
}

static void cmd_date(TerminalEmulator &term)
{
    SysTime t = {};
    if (get_time(&t) != 0) {
        term.write_string("date: failed to get time\n");
        return;
    }
    term_printf(term, "%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
}

static void cmd_uptime(TerminalEmulator &term)
{
    uint64_t up = get_uptime();
    uint64_t s = up % 60;
    uint64_t m = (up / 60) % 60;
    uint64_t h = up / 3600;
    term_printf(term, "up %02llu:%02llu:%02llu\n", h, m, s);
}

static void publish_wallpaper_request(const char *path)
{
    Registry *registry = gui_registry();
    if (!registry || !path || !*path)
        return;
    strncpy(registry->wallpaper_requested, path, sizeof(registry->wallpaper_requested) - 1);
    registry->wallpaper_requested[sizeof(registry->wallpaper_requested) - 1] = '\0';
    registry->wallpaper_generation = registry->wallpaper_generation + 1u;
    registry->wallpaper_reload_requested = true;
    asm volatile("sfence" ::: "memory");
}

static uint32_t terminal_theme_mode()
{
    Registry *registry = gui_registry();
    if (registry)
        return registry->theme_mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK;

    char config[512];
    char value[64];
    const char *candidates[] = {SYSTEM_CONFIG_PATH, SYSTEM_BOOTSTRAP_CONFIG_PATH};
    if (cfg_read_text_from_candidates(candidates, sizeof(candidates) / sizeof(candidates[0]), config, sizeof(config)) &&
        cfg_line_value(config, "theme", value, sizeof(value)) && strcmp(value, "light") == 0) {
        return GUI_THEME_LIGHT;
    }
    return GUI_THEME_DARK;
}

static void cmd_wallpaper(TerminalEmulator &term, const char *cwd, const char *arg)
{
    if (!arg || !*arg) {
        Registry *registry = gui_registry();
        if (registry && registry->wallpaper_active[0]) {
            const char *status = "solid";
            if (registry->wallpaper_status == WALLPAPER_STATUS_DEFAULT)
                status = "default";
            else if (registry->wallpaper_status == WALLPAPER_STATUS_CUSTOM)
                status = "custom";
            term_printf(term, "wallpaper: %s (%s)\n", registry->wallpaper_active, status);
            return;
        }

        char configured[256] = {};
        const char *candidates[] = {WALLPAPER_CONFIG_PATH, WALLPAPER_BOOTSTRAP_CONFIG_PATH};
        cfg_read_first_line_from_candidates(candidates, sizeof(candidates) / sizeof(candidates[0]), configured,
                                            sizeof(configured));
        if (configured[0])
            term_printf(term, "wallpaper: %s (configured)\n",
                        wallpaper_resolve_path_for_theme(configured, terminal_theme_mode()));
        else
            term_printf(term, "wallpaper: %s (default)\n", wallpaper_default_path_for_theme(terminal_theme_mode()));
        return;
    }

    char resolved[256];
    resolve_path(cwd, arg, resolved);
    Surface image = {};
    if (!gui_load_uowp(resolved, wallpaper_uowp_variant_for_theme(terminal_theme_mode()), 0, 0, &image)) {
        term_printf(term, "wallpaper: unsupported or unreadable UOWP: %s\n", arg);
        return;
    }
    gui_destroy_surface(&image);

    publish_wallpaper_request(resolved);

    char config_buf[320];
    snprintf(config_buf, sizeof(config_buf), "%s\n", resolved);
    if (!cfg_write_text_file(WALLPAPER_CONFIG_PATH, config_buf)) {
        term_printf(term, "wallpaper applied for this session: %s\n", resolved);
        return;
    }

    term_printf(term, "wallpaper set to %s\n", resolved);
}

static void execute_command(TerminalEmulator &term, char *cwd, char *line)
{
    while (*line == ' ')
        line++;
    if (*line == '\0')
        return;

    char *space = strchr(line, ' ');
    char *arg = nullptr;
    if (space) {
        *space = '\0';
        arg = space + 1;
        while (arg && *arg == ' ')
            arg++;
    }

    if (strcmp(line, "help") == 0) {
        cmd_help(term);
    } else if (strcmp(line, "clear") == 0) {
        term.clear_screen();
    } else if (strcmp(line, "exit") == 0) {
        exit(0);
    } else if (strcmp(line, "echo") == 0) {
        term.write_string(arg ? arg : "");
        term.put_char('\n');
    } else if (strcmp(line, "pwd") == 0) {
        cmd_pwd(term, cwd);
    } else if (strcmp(line, "cd") == 0) {
        cmd_cd(term, cwd, arg);
    } else if (strcmp(line, "ls") == 0) {
        cmd_ls(term, cwd, arg);
    } else if (strcmp(line, "cat") == 0) {
        cmd_cat(term, cwd, arg);
    } else if (strcmp(line, "stat") == 0) {
        cmd_stat(term, cwd, arg);
    } else if (strcmp(line, "touch") == 0) {
        cmd_touch(term, cwd, arg);
    } else if (strcmp(line, "rm") == 0) {
        cmd_rm(term, cwd, arg);
    } else if (strcmp(line, "mkdir") == 0) {
        cmd_mkdir(term, cwd, arg);
    } else if (strcmp(line, "rmdir") == 0) {
        cmd_rmdir(term, cwd, arg);
    } else if (strcmp(line, "mv") == 0) {
        cmd_mv(term, cwd, arg);
    } else if (strcmp(line, "cp") == 0) {
        cmd_cp(term, cwd, arg);
    } else if (strcmp(line, "mem") == 0) {
        cmd_mem(term);
    } else if (strcmp(line, "ps") == 0) {
        cmd_ps(term);
    } else if (strcmp(line, "date") == 0) {
        cmd_date(term);
    } else if (strcmp(line, "uptime") == 0) {
        cmd_uptime(term);
    } else if (strcmp(line, "wallpaper") == 0) {
        cmd_wallpaper(term, cwd, arg);
    } else if (strcmp(line, "version") == 0) {
        term_printf(term, "uniOS @ %s\n", GIT_COMMIT);
    } else if (strcmp(line, "uname") == 0) {
        term_printf(term, "uniOS %s x86_64\n", GIT_COMMIT);
    } else {
        term_printf(term, "Unknown command: %s\n", line);
    }
}

static void redraw_terminal_input(TerminalEmulator &term, const char *cwd, const char *line, int line_len,
                                  int cursor_pos)
{
    term.write_string("\r\x1b[K");
    print_prompt(term, cwd);
    term.write_string(line);
    int left = line_len - cursor_pos;
    if (left > 0)
        term_printf(term, "\x1b[%dD", left);
}

static void terminal_insert_text(TerminalEmulator &term, const char *cwd, char *line, int &line_len, int &cursor_pos,
                                 const char *text)
{
    int add = (int)strlen(text);
    if (add <= 0 || line_len + add >= 255)
        return;
    memmove(line + cursor_pos + add, line + cursor_pos, (size_t)(line_len - cursor_pos + 1));
    memcpy(line + cursor_pos, text, (size_t)add);
    line_len += add;
    cursor_pos += add;
    redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
}

static bool term_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}

struct TermCompletion
{
    char text[256];
    bool is_dir;
};

static int collect_term_command_matches(const char *prefix, TermCompletion *matches, int max_matches)
{
    int count = 0;
    for (uint32_t i = 0; i < sizeof(k_term_commands) / sizeof(k_term_commands[0]) && count < max_matches; i++) {
        if (!term_starts_with(k_term_commands[i].name, prefix))
            continue;
        strncpy(matches[count].text, k_term_commands[i].name, sizeof(matches[count].text) - 1);
        matches[count].text[sizeof(matches[count].text) - 1] = '\0';
        matches[count].is_dir = false;
        count++;
    }
    return count;
}

static void split_term_completion_path(const char *token, char *dir_token, size_t dir_size, char *base,
                                       size_t base_size)
{
    const char *slash = strrchr(token, '/');
    if (!slash) {
        strncpy(dir_token, ".", dir_size - 1);
        dir_token[dir_size - 1] = '\0';
        strncpy(base, token, base_size - 1);
        base[base_size - 1] = '\0';
        return;
    }
    size_t dir_len = (size_t)(slash - token);
    if (dir_len == 0)
        dir_len = 1;
    if (dir_len >= dir_size)
        dir_len = dir_size - 1;
    strncpy(dir_token, token, dir_len);
    dir_token[dir_len] = '\0';
    strncpy(base, slash + 1, base_size - 1);
    base[base_size - 1] = '\0';
}

static int collect_term_file_matches(const char *cwd, const char *token, TermCompletion *matches, int max_matches)
{
    char dir_token[256], base[128], resolved[256];
    split_term_completion_path(token, dir_token, sizeof(dir_token), base, sizeof(base));
    resolve_path(cwd, strcmp(dir_token, ".") == 0 ? cwd : dir_token, resolved);
    int fd = open(resolved, O_RDONLY);
    if (fd < 0)
        return 0;

    int count = 0;
    char name[256];
    while (count < max_matches && (int)syscall3(SYS_GETDENTS, (uint64_t)fd, (uint64_t)name, 0) == 0) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (!term_starts_with(name, base))
            continue;
        char full[512];
        strncpy(full, resolved, sizeof(full) - 1);
        full[sizeof(full) - 1] = '\0';
        size_t len = strlen(full);
        if (len == 0 || full[len - 1] != '/')
            strncat(full, "/", sizeof(full) - strlen(full) - 1);
        strncat(full, name, sizeof(full) - strlen(full) - 1);
        VNodeStat st = {};
        strncpy(matches[count].text, name, sizeof(matches[count].text) - 1);
        matches[count].text[sizeof(matches[count].text) - 1] = '\0';
        matches[count].is_dir = stat(full, &st) == 0 && st.is_dir;
        count++;
    }
    close(fd);
    return count;
}

static int term_common_prefix_len(const TermCompletion *matches, int count)
{
    if (count <= 0)
        return 0;
    int prefix_len = (int)strlen(matches[0].text);
    for (int i = 1; i < count; i++) {
        int j = 0;
        while (j < prefix_len && matches[i].text[j] && matches[i].text[j] == matches[0].text[j])
            j++;
        prefix_len = j;
    }
    return prefix_len;
}

static void terminal_complete(TerminalEmulator &term, const char *cwd, char *line, int &line_len, int &cursor_pos)
{
    int token_start = cursor_pos;
    while (token_start > 0 && line[token_start - 1] != ' ')
        token_start--;
    bool is_command = token_start == 0;
    char token[128];
    int token_len = cursor_pos - token_start;
    if (token_len >= (int)sizeof(token))
        token_len = (int)sizeof(token) - 1;
    strncpy(token, line + token_start, (size_t)token_len);
    token[token_len] = '\0';

    TermCompletion matches[48];
    int count = is_command ? collect_term_command_matches(token, matches, 48)
                           : collect_term_file_matches(cwd, token, matches, 48);
    if (count == 0)
        return;

    int base_len = token_len;
    if (!is_command) {
        const char *slash = strrchr(token, '/');
        if (slash)
            base_len = (int)strlen(slash + 1);
    }

    if (count == 1) {
        terminal_insert_text(term, cwd, line, line_len, cursor_pos, matches[0].text + base_len);
        if (matches[0].is_dir)
            terminal_insert_text(term, cwd, line, line_len, cursor_pos, "/");
        else if (is_command)
            terminal_insert_text(term, cwd, line, line_len, cursor_pos, " ");
        return;
    }

    int common_len = term_common_prefix_len(matches, count);
    if (common_len > base_len) {
        char suffix[128];
        int suffix_len = common_len - base_len;
        if (suffix_len >= (int)sizeof(suffix))
            suffix_len = (int)sizeof(suffix) - 1;
        strncpy(suffix, matches[0].text + base_len, (size_t)suffix_len);
        suffix[suffix_len] = '\0';
        terminal_insert_text(term, cwd, line, line_len, cursor_pos, suffix);
        return;
    }

    term.put_char('\n');
    for (int i = 0; i < count; i++) {
        if (matches[i].is_dir)
            term_printf(term, "\x1b[34m%s/\x1b[0m  ", matches[i].text);
        else
            term_printf(term, "%s  ", matches[i].text);
    }
    term.put_char('\n');
    redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TerminalEmulator term;
    term.init(80, 25);
    if (!term.ready())
        return 1;

    char cwd[256];
    choose_initial_cwd(cwd, sizeof(cwd));

    gui_request_focus();
    print_prompt(term, cwd);
    term.render_all();
    Registry *registry = gui_registry();
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    char line[256];
    int line_len = 0;
    int cursor_pos = 0;
    memset(line, 0, sizeof(line));
    char input_history[32][256];
    int input_history_count = 0;
    int input_history_cursor = 0;
    memset(input_history, 0, sizeof(input_history));

    while (true) {
        bool saw_event = false;
        Event ev = {};
        bool needs_render = false;
        while (poll_event(&ev) > 0) {
            saw_event = true;
            if (ev.type == EVT_WINDOW_CLOSE)
                return 0;
            if (ev.type == EVT_WINDOW_RESIZE) {
                if (term.sync_resize()) {
                    needs_render = true;
                }
                continue;
            }
            if (ev.type != EVT_KEY_DOWN || ev.key.c == 0)
                continue;

            uint8_t c = (uint8_t)ev.key.c;
            if (c == '\n' || c == '\r') {
                line[line_len] = '\0';
                term.put_char('\n');
                if (line_len > 0) {
                    int slot = input_history_count % 32;
                    if (input_history_count == 0 || strcmp(input_history[(input_history_count - 1) % 32], line) != 0) {
                        strncpy(input_history[slot], line, sizeof(input_history[slot]) - 1);
                        input_history[slot][sizeof(input_history[slot]) - 1] = '\0';
                        input_history_count++;
                    }
                    input_history_cursor = input_history_count;
                }
                execute_command(term, cwd, line);
                line_len = 0;
                cursor_pos = 0;
                line[0] = '\0';
                print_prompt(term, cwd);
                needs_render = true;
                continue;
            }

            if (c == '\t') {
                terminal_complete(term, cwd, line, line_len, cursor_pos);
                needs_render = true;
                continue;
            }

            if (c == '\b' || c == 127) {
                if (cursor_pos > 0) {
                    memmove(line + cursor_pos - 1, line + cursor_pos, (size_t)(line_len - cursor_pos + 1));
                    line_len--;
                    cursor_pos--;
                    redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                    needs_render = true;
                }
                continue;
            }

            if (c == KEY_DELETE) {
                if (cursor_pos < line_len) {
                    memmove(line + cursor_pos, line + cursor_pos + 1, (size_t)(line_len - cursor_pos));
                    line_len--;
                    redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                    needs_render = true;
                }
                continue;
            }

            if (c == KEY_LEFT_ARROW) {
                if (cursor_pos > 0) {
                    cursor_pos--;
                    term.write_string("\x1b[1D");
                    needs_render = true;
                }
                continue;
            }

            if (c == KEY_RIGHT_ARROW) {
                if (cursor_pos < line_len) {
                    cursor_pos++;
                    term.write_string("\x1b[1C");
                    needs_render = true;
                }
                continue;
            }

            if (c == KEY_HOME || c == 1) {
                cursor_pos = 0;
                redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                needs_render = true;
                continue;
            }

            if (c == KEY_END || c == 5) {
                cursor_pos = line_len;
                redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                needs_render = true;
                continue;
            }

            if (c == KEY_UP_ARROW) {
                if (input_history_count > 0) {
                    int first = input_history_count > 32 ? input_history_count - 32 : 0;
                    if (input_history_cursor > first)
                        input_history_cursor--;
                    strncpy(line, input_history[input_history_cursor % 32], sizeof(line) - 1);
                    line[sizeof(line) - 1] = '\0';
                    line_len = (int)strlen(line);
                    cursor_pos = line_len;
                    redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                    needs_render = true;
                }
                continue;
            }

            if (c == KEY_DOWN_ARROW) {
                if (input_history_cursor < input_history_count - 1) {
                    input_history_cursor++;
                    strncpy(line, input_history[input_history_cursor % 32], sizeof(line) - 1);
                    line[sizeof(line) - 1] = '\0';
                } else {
                    input_history_cursor = input_history_count;
                    line[0] = '\0';
                }
                line_len = (int)strlen(line);
                cursor_pos = line_len;
                redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                needs_render = true;
                continue;
            }

            if (c == 12) {
                term.clear_screen();
                redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                needs_render = true;
                continue;
            }

            if (c == 21) {
                line[0] = '\0';
                line_len = 0;
                cursor_pos = 0;
                redraw_terminal_input(term, cwd, line, line_len, cursor_pos);
                needs_render = true;
                continue;
            }

            if (c >= 32 && c <= 126 && line_len < (int)sizeof(line) - 1) {
                char text[2] = {(char)c, '\0'};
                terminal_insert_text(term, cwd, line, line_len, cursor_pos, text);
                needs_render = true;
            }
        }

        registry = gui_registry();
        if (registry && registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            if (gui_sync_theme_from_registry()) {
                term.theme_changed();
            }
            needs_render = true;
        }

        if (needs_render) {
            term.render_all();
        } else if (!saw_event) {
            sleep_ms(35);
        }
    }

    return 0;
}
