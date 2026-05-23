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
static constexpr uint8_t KEY_PAGEUP = 0x87;
static constexpr uint8_t KEY_PAGEDOWN = 0x88;
static constexpr uint32_t TERM_SCROLLBAR_RESERVE = 12;

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
        : m_scroll_offset(0), m_grid(nullptr), m_presented_grid(nullptr), m_history_len(nullptr), m_history_text(nullptr),
          m_history_fg(nullptr), m_ready(false), m_cursor_visible(false)
    {
    }

    uint32_t get_scroll_offset() const { return m_scroll_offset; }
    uint32_t height() const { return m_height; }

    void reset_scroll()
    {
        if (m_scroll_offset != 0) {
            m_scroll_offset = 0;
            m_needs_full_redraw = true;
        }
    }

    void scroll_history(int delta)
    {
        uint32_t max_s = max_scroll();
        int new_offset = (int)m_scroll_offset + delta;
        if (new_offset < 0) {
            new_offset = 0;
        } else if (new_offset > (int)max_s) {
            new_offset = (int)max_s;
        }
        if (m_scroll_offset != (uint32_t)new_offset) {
            m_scroll_offset = (uint32_t)new_offset;
            m_needs_full_redraw = true;
        }
    }

    uint32_t max_scroll() const
    {
        uint32_t total = get_total_history_slices();
        return (total > m_height) ? (total - m_height) : 0;
    }

    uint32_t get_total_history_slices() const
    {
        uint32_t total = 0;
        for (uint32_t i = 0; i < m_history_count; i++) {
            uint32_t len = m_history_len[i];
            uint32_t wraps = (len == 0) ? 1 : ((len + m_width - 1) / m_width);
            total += wraps;
        }
        return total;
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
        m_scroll_offset = 0;
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

        m_window = gui_register_window_ex("Terminal", width * term_cell_w() + term_pad_x() * 2 + TERM_SCROLLBAR_RESERVE,
                                          height * term_cell_h() + term_pad_y() * 2, WIN_FLAG_RESIZABLE);
        if (!m_window.buffer)
            return;
        gui_window_set_min_size((int)(term_cell_w() * 48u + (uint32_t)term_pad_x() * 2u + TERM_SCROLLBAR_RESERVE),
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

        uint32_t total_slices = get_total_history_slices();
        uint32_t max_s = max_scroll();
        if (total_slices > m_height && max_s > 0) {
            int sb_x = (int)m_window.width - term_pad_x() / 2 - 10;
            int sb_y = term_content_y();
            int sb_w = 6;
            int sb_h = (int)(m_height * term_cell_h());

            gui_fill_rect(&m_window, sb_x - 1, sb_y, sb_w + 2, sb_h, term_bg());
            gui_fill_rounded_rect(&m_window, sb_x, sb_y, sb_w, sb_h, sb_w / 2, g_gui_style.app_surface_alt);

            int thumb_h = (sb_h * (int)m_height) / (int)total_slices;
            if (thumb_h < 16)
                thumb_h = 16;

            int scrollable_dist = sb_h - thumb_h;
            int thumb_y = sb_y + scrollable_dist - (int)((scrollable_dist * m_scroll_offset) / max_s);
            gui_fill_rounded_rect(&m_window, sb_x, thumb_y, sb_w, thumb_h, sb_w / 2, g_gui_style.text_dim);

            if (has_dirty) {
                if (dirty_x2 < (int32_t)m_window.width) {
                    dirty_x2 = (int32_t)m_window.width;
                }
            }
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
            (m_window.width > (uint32_t)(term_pad_x() * 2 + TERM_SCROLLBAR_RESERVE)) ? (m_window.width - (uint32_t)(term_pad_x() * 2) - TERM_SCROLLBAR_RESERVE) : 0;
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
        m_scroll_offset = 0;
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
        m_scroll_offset = 0;
        m_needs_full_redraw = true;
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

        uint32_t skipped = 0;
        for (int line = (int)m_history_count - 1; line >= 0 && visible_count < visible_limit; line--) {
            uint32_t len = m_history_len[line];
            uint32_t wraps = (len == 0) ? 1 : ((len + m_width - 1) / m_width);
            for (int seg = (int)wraps - 1; seg >= 0 && visible_count < visible_limit; seg--) {
                if (skipped < m_scroll_offset) {
                    skipped++;
                    continue;
                }
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

    uint32_t m_scroll_offset = 0;
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

static inline int epoll_create(int size)
{
    return (int)syscall1(SYS_EPOLL_CREATE, (uint64_t)size);
}

static inline int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    return (int)syscall6(SYS_EPOLL_CTL, (uint64_t)epfd, (uint64_t)op, (uint64_t)fd, (uint64_t)event, 0, 0);
}

static inline int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    return (int)syscall6(SYS_EPOLL_WAIT, (uint64_t)epfd, (uint64_t)events, (uint64_t)maxevents, (uint64_t)timeout, 0, 0);
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    TerminalEmulator term;
    term.init(80, 25);
    if (!term.ready())
        return 1;

    gui_request_focus();
    term.render_all();
    Registry *registry = gui_registry();
    uint32_t last_settings_generation = registry ? registry->settings_generation : 0;

    int pipe_to_shell[2];
    int pipe_from_shell[2];
    if (pipe(pipe_to_shell) < 0 || pipe(pipe_from_shell) < 0) {
        term.write_string("terminal: failed to create pipes\n");
        term.render_all();
        return 1;
    }

    int shell_pid = fork();
    if (shell_pid < 0) {
        term.write_string("terminal: failed to fork child process\n");
        term.render_all();
        return 1;
    }

    if (shell_pid == 0) {
        // Child: shell process
        dup2(pipe_to_shell[0], 0);
        dup2(pipe_from_shell[1], 1);
        dup2(pipe_from_shell[1], 2);

        close(pipe_to_shell[0]);
        close(pipe_to_shell[1]);
        close(pipe_from_shell[0]);
        close(pipe_from_shell[1]);

        exec("/bin/shell.elf");
        printf("terminal: failed to execute /bin/shell.elf\n");
        exit(127);
    }

    // Parent: terminal GUI emulator
    close(pipe_to_shell[0]);
    close(pipe_from_shell[1]);

    int epfd = epoll_create(1);
    if (epfd < 0) {
        term.write_string("terminal: failed to create epoll instance\n");
        term.render_all();
        return 1;
    }

    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = pipe_from_shell[0];
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_from_shell[0], &ev) < 0) {
        term.write_string("terminal: failed to configure epoll\n");
        term.render_all();
        return 1;
    }

    bool shell_alive = true;
    while (true) {
        bool saw_event = false;
        Event gui_ev = {};
        bool needs_render = false;

        // 1. Poll GUI events
        while (poll_event(&gui_ev) > 0) {
            saw_event = true;
            if (gui_ev.type == EVT_WINDOW_CLOSE) {
                return 0;
            }
            if (gui_ev.type == EVT_WINDOW_RESIZE) {
                if (term.sync_resize()) {
                    needs_render = true;
                }
                continue;
            }
            if (gui_ev.type == EVT_MOUSE_SCROLL) {
                int delta = gui_ev.mouse.scroll_y * 3;
                term.scroll_history(delta);
                needs_render = true;
                continue;
            }
            if (gui_ev.type == EVT_KEY_DOWN && gui_ev.key.c != 0) {
                char c = (char)gui_ev.key.c;
                if ((uint8_t)c == KEY_PAGEUP) {
                    term.scroll_history((int)term.height() / 2);
                    needs_render = true;
                    continue;
                }
                if ((uint8_t)c == KEY_PAGEDOWN) {
                    term.scroll_history(-(int)term.height() / 2);
                    needs_render = true;
                    continue;
                }

                if (term.get_scroll_offset() > 0) {
                    term.reset_scroll();
                    needs_render = true;
                }

                if (shell_alive) {
                    write(pipe_to_shell[1], &c, 1);
                }
                continue;
            }
        }

        // 2. Poll shell output if alive
        if (shell_alive) {
            struct epoll_event events[1];
            int n = epoll_wait(epfd, events, 1, 0);
            if (n > 0) {
                char read_buf[1024];
                int bytes_read = read(pipe_from_shell[0], read_buf, sizeof(read_buf));
                if (bytes_read > 0) {
                    term.write_bytes(read_buf, (size_t)bytes_read);
                    needs_render = true;
                    saw_event = true;
                } else if (bytes_read == 0) {
                    shell_alive = false;
                    term.write_string("\r\n[Process completed]\r\n");
                    needs_render = true;
                    saw_event = true;
                }
            }
        }

        // 3. Theme change sync
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
