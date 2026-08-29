#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <uapi/event.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>
#include <uapi/sysinfo.h>
#include <unistd.h>

#include "../../libapp/app.h"
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

// The terminal input/output field is pinned to the dark theme regardless of
// the active UI theme, so output stays legible and consistent. These mirror
// k_gui_style_dark in libgui/gui.cpp; keep them in sync if that palette moves.
static constexpr uint32_t TERM_DARK_FRAME_BG = 0xFF111214u;      // app_bg
static constexpr uint32_t TERM_DARK_BG = 0xFF15171Au;            // app_surface
static constexpr uint32_t TERM_DARK_FG = 0xFFF2F2F0u;            // text
static constexpr uint32_t TERM_DARK_CURSOR = 0xFF626C78u;        // accent
static constexpr uint32_t TERM_DARK_BORDER = 0xFF333942u;        // border
static constexpr uint32_t TERM_DARK_CHROME_BG_ALT = 0xFF242830u; // chrome_bg_alt
static constexpr uint32_t TERM_DARK_TEXT_DIM = 0xFFC5C8CCu;      // text_dim

static inline uint32_t term_bg()
{
    return TERM_DARK_BG;
}
static inline uint32_t term_frame_bg()
{
    return TERM_DARK_FRAME_BG;
}
static inline uint32_t term_fg()
{
    return TERM_DARK_FG;
}
static inline uint32_t term_cursor()
{
    return TERM_DARK_CURSOR;
}
static inline uint32_t term_border()
{
    return TERM_DARK_BORDER;
}
static inline uint32_t term_chrome_bg_alt()
{
    return TERM_DARK_CHROME_BG_ALT;
}
// Selection highlight: accent blended ~45% over the cell background so the
// tint stays legible in both themes and over ANSI-colored output.
static inline uint32_t term_sel_bg()
{
    uint32_t a = TERM_DARK_CURSOR;
    uint32_t b = TERM_DARK_BG;
    uint32_t r = (((a >> 16) & 0xFFu) * 115u + ((b >> 16) & 0xFFu) * 141u) >> 8;
    uint32_t g = (((a >> 8) & 0xFFu) * 115u + ((b >> 8) & 0xFFu) * 141u) >> 8;
    uint32_t bl = ((a & 0xFFu) * 115u + (b & 0xFFu) * 141u) >> 8;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
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

// Menubar command IDs (dispatched through WindowEntry.menu_command_id).
enum
{
    TERM_MENU_COPY = 1,
    TERM_MENU_PASTE,
    TERM_MENU_SELECT_ALL,
    TERM_MENU_CLEAR,
    TERM_MENU_ZOOM_IN,
    TERM_MENU_ZOOM_OUT,
    TERM_MENU_ZOOM_ACTUAL,
    TERM_MENU_HELP = 0x80,
};

// Zoom is a step offset from the resolution-chosen base mono size. Ctrl+= and
// Ctrl+- collapse to plain '='/'-' at the PS/2 layer (only letters become
// control codes), so zoom is menu-driven to avoid stealing '-'/'=' from the
// shell.
static constexpr int TERM_ZOOM_MIN = -4;
static constexpr int TERM_ZOOM_MAX = 6;
static int g_term_zoom = 0;

static inline const GuiFont *term_font()
{
    const GuiFont *base = gui_font_mono();
    if (!base)
        return gui_font_default();
    if (g_term_zoom == 0)
        return base;
    const GuiFont *zoomed = gui_font_mono_size((int)base->pixel_size + g_term_zoom);
    return zoomed ? zoomed : base;
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
            return TERM_DARK_TEXT_DIM;
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
        : m_scroll_offset(0), m_grid(nullptr), m_presented_grid(nullptr), m_history_len(nullptr),
          m_history_text(nullptr), m_history_fg(nullptr), m_history_start(0), m_ready(false), m_cursor_visible(false)
    {
    }

    uint32_t get_scroll_offset() const
    {
        return m_scroll_offset;
    }
    uint32_t height() const
    {
        return m_height;
    }

    void reset_scroll()
    {
        if (m_scroll_offset != 0) {
            m_scroll_offset = 0;
            m_needs_full_redraw = true;
            sel_clear();
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
            sel_clear();
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
            uint32_t len = history_line_len(i);
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

    void init(uint32_t width, uint32_t height, const Surface &window)
    {
        m_cursor_x = 0;
        m_cursor_y = 0;
        m_scroll_offset = 0;
        m_fg = term_fg();
        m_bg = term_bg();
        m_ansi_state = 0;
        m_ansi_idx = 0;
        memset(m_ansi_buf, 0, sizeof(m_ansi_buf));
        m_window = window;

        m_history_len = (uint16_t *)malloc(sizeof(uint16_t) * TERM_HISTORY_LINES);
        m_history_text = (char *)malloc((size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN);
        m_history_fg = (uint32_t *)malloc((size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        if (!m_history_len || !m_history_text || !m_history_fg)
            return;
        memset(m_history_len, 0, sizeof(uint16_t) * TERM_HISTORY_LINES);
        memset(m_history_text, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN);
        memset(m_history_fg, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        reset_history();

        if (!m_window.buffer)
            return;
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

    void set_focused(bool f)
    {
        m_focused = f;
    }

    bool focused() const
    {
        return m_focused;
    }

    // Cursor blink gate: render_all only draws the caret when this is true.
    void set_blink_on(bool on)
    {
        m_blink_on = on;
    }

    void clear_screen()
    {
        sel_clear();
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
        sel_clear();
        while (*str)
            put_char(*str++);
    }

    void write_bytes(const char *data, size_t len)
    {
        if (!data)
            return;
        sel_clear();
        for (size_t i = 0; i < len; i++)
            put_char(data[i]);
    }

    void render_all()
    {
        if (!m_ready)
            return;

        rebuild_grid_from_history();
        apply_sel_tint();
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

        if (m_cursor_visible && m_blink_on && m_scroll_offset == 0 && m_cursor_x < m_width && m_cursor_y < m_height) {
            gui_fill_rect(&m_window, term_content_x() + (int32_t)(m_cursor_x * term_cell_w()),
                          term_content_y() + (int32_t)(m_cursor_y * term_cell_h() + term_cell_h() - TERM_CURSOR_H),
                          term_cell_w(), TERM_CURSOR_H, term_cursor());
            mark_dirty_cell(m_cursor_x, m_cursor_y);
        }

        uint32_t total_slices = get_total_history_slices();
        uint32_t max_s = max_scroll();
        if (total_slices > m_height && max_s > 0) {
            int sb_w = gui_scrollbar_w();
            int sb_x = (int)m_window.width - term_pad_x() / 2 - sb_w - gui_space_0_5();
            int sb_y = term_content_y();
            int sb_h = (int)(m_height * term_cell_h());

            gui_fill_rect(&m_window, sb_x - 1, sb_y, sb_w + 2, sb_h, term_bg());

            int thumb_h = (sb_h * (int)m_height) / (int)total_slices;
            if (thumb_h < gui_scrollbar_min_thumb())
                thumb_h = gui_scrollbar_min_thumb();

            int scrollable_dist = sb_h - thumb_h;
            int thumb_y = scrollable_dist - (int)((scrollable_dist * m_scroll_offset) / max_s);
            gui_draw_scrollbar(&m_window, sb_x, sb_y, sb_w, sb_h, thumb_y, thumb_h, false);

            if (has_dirty) {
                if (dirty_x2 < (int32_t)m_window.width) {
                    dirty_x2 = (int32_t)m_window.width;
                }
            }
        }

        if (m_help_visible) {
            draw_help_overlay();
            dirty_x1 = 0;
            dirty_y1 = 0;
            dirty_x2 = (int32_t)m_window.width;
            dirty_y2 = (int32_t)m_window.height;
            has_dirty = true;
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

    // Recompute the cell grid from the current window size. The runtime
    // re-syncs the window backing before resize events arrive, so this only
    // reflows the grid.
    bool reflow_grid()
    {
        if (!m_ready && !m_window.buffer)
            return false;

        uint32_t content_w = (m_window.width > (uint32_t)(term_pad_x() * 2 + TERM_SCROLLBAR_RESERVE))
                                 ? (m_window.width - (uint32_t)(term_pad_x() * 2) - TERM_SCROLLBAR_RESERVE)
                                 : 0;
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

    bool sync_resize()
    {
        return reflow_grid();
    }

    // The runtime may swap the window backing while growing a resize, so the
    // cached surface must be re-read from the shared window before reflow.
    void refresh_window(const Surface &window)
    {
        m_window = window;
    }

    // Re-derive the grid from the current window size after the font (and
    // therefore the cell size) changed. The window keeps its pixel size; the
    // column/row count follows the new cell dimensions.
    bool reflow_for_font()
    {
        if (!m_ready)
            return false;
        m_scroll_offset = 0;
        uint32_t content_w = (m_window.width > (uint32_t)(term_pad_x() * 2 + TERM_SCROLLBAR_RESERVE))
                                 ? (m_window.width - (uint32_t)(term_pad_x() * 2) - TERM_SCROLLBAR_RESERVE)
                                 : 0;
        uint32_t content_h =
            (m_window.height > (uint32_t)(term_pad_y() * 2)) ? (m_window.height - (uint32_t)(term_pad_y() * 2)) : 0;
        uint32_t cw = term_cell_w();
        uint32_t ch = term_cell_h();
        uint32_t new_width = cw > 0 ? content_w / cw : 0;
        uint32_t new_height = ch > 0 ? content_h / ch : 0;
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

    // --- Mouse selection ---------------------------------------------------
    // The selection is a contiguous row-major range over the visible grid. It
    // is expressed in viewport cell coordinates, so anything that reflows the
    // viewport (scroll, resize, new output, clear) invalidates it.
    void sel_start(int32_t x, int32_t y)
    {
        uint32_t c, r;
        cell_from_point(x, y, &c, &r);
        m_sel_anchor_col = c;
        m_sel_anchor_row = r;
        m_sel_caret_col = c;
        m_sel_caret_row = r;
        m_sel_active = true;
        m_sel_dragging = true;
    }

    bool sel_extend(int32_t x, int32_t y)
    {
        if (!m_sel_active || !m_sel_dragging)
            return false;
        uint32_t c, r;
        cell_from_point(x, y, &c, &r);
        if (c == m_sel_caret_col && r == m_sel_caret_row)
            return false;
        m_sel_caret_col = c;
        m_sel_caret_row = r;
        return true;
    }

    void sel_finish()
    {
        m_sel_dragging = false;
        if (m_sel_active && m_sel_anchor_col == m_sel_caret_col && m_sel_anchor_row == m_sel_caret_row)
            m_sel_active = false;
    }

    void sel_stop_drag()
    {
        m_sel_dragging = false;
    }

    void sel_clear()
    {
        m_sel_active = false;
        m_sel_dragging = false;
    }

    void sel_all()
    {
        if (m_width == 0 || m_height == 0)
            return;
        m_sel_anchor_col = 0;
        m_sel_anchor_row = 0;
        m_sel_caret_col = m_width - 1;
        m_sel_caret_row = m_height - 1;
        m_sel_active = true;
        m_sel_dragging = false;
    }

    bool sel_has() const
    {
        return m_sel_active && !(m_sel_anchor_col == m_sel_caret_col && m_sel_anchor_row == m_sel_caret_row);
    }

    size_t sel_extract(char *out, size_t out_size) const
    {
        if (!sel_has() || !m_grid || !out || out_size == 0 || m_width == 0)
            return 0;
        uint32_t a = m_sel_anchor_row * m_width + m_sel_anchor_col;
        uint32_t b = m_sel_caret_row * m_width + m_sel_caret_col;
        uint32_t lo = a < b ? a : b;
        uint32_t hi = a < b ? b : a;
        uint32_t start_row = lo / m_width;
        uint32_t start_col = lo % m_width;
        uint32_t end_row = hi / m_width;
        uint32_t end_col = hi % m_width;

        size_t pos = 0;
        for (uint32_t row = start_row; row <= end_row && pos + 1 < out_size; row++) {
            uint32_t c0 = (row == start_row) ? start_col : 0;
            uint32_t c1 = (row == end_row) ? end_col : (m_width - 1);
            int last = -1;
            for (uint32_t c = c0; c <= c1; c++) {
                char ch = m_grid[row * m_width + c].ch;
                if (ch >= 33)
                    last = (int)c;
            }
            if (last >= 0) {
                for (uint32_t c = c0; c <= (uint32_t)last && pos + 1 < out_size; c++) {
                    char ch = m_grid[row * m_width + c].ch;
                    out[pos++] = (ch >= 32) ? ch : ' ';
                }
            }
            if (row < end_row && pos + 1 < out_size)
                out[pos++] = '\n';
        }
        while (pos > 0 && out[pos - 1] == '\n')
            pos--;
        out[pos] = '\0';
        return pos;
    }

    // --- Help overlay ------------------------------------------------------
    void show_help()
    {
        if (!m_help_visible) {
            m_help_visible = true;
            m_needs_full_redraw = true;
        }
    }

    void hide_help()
    {
        if (m_help_visible) {
            m_help_visible = false;
            m_needs_full_redraw = true;
        }
    }

    bool help_visible() const
    {
        return m_help_visible;
    }

    bool help_close_hit(int x, int y) const
    {
        return m_help_visible && x >= m_help_close.x && x < m_help_close.x + m_help_close.w && y >= m_help_close.y &&
               y < m_help_close.y + m_help_close.h;
    }

private:
    void cell_from_point(int32_t x, int32_t y, uint32_t *col, uint32_t *row) const
    {
        int32_t cx = 0, cy = 0;
        if (m_width > 0 && m_height > 0) {
            cx = (x - term_content_x()) / (int32_t)term_cell_w();
            cy = (y - term_content_y()) / (int32_t)term_cell_h();
            if (cx < 0)
                cx = 0;
            if (cy < 0)
                cy = 0;
            if (cx >= (int32_t)m_width)
                cx = (int32_t)m_width - 1;
            if (cy >= (int32_t)m_height)
                cy = (int32_t)m_height - 1;
        }
        *col = (uint32_t)cx;
        *row = (uint32_t)cy;
    }

    bool cell_selected(uint32_t col, uint32_t row) const
    {
        if (!sel_has())
            return false;
        uint32_t a = m_sel_anchor_row * m_width + m_sel_anchor_col;
        uint32_t b = m_sel_caret_row * m_width + m_sel_caret_col;
        uint32_t lo = a < b ? a : b;
        uint32_t hi = a < b ? b : a;
        uint32_t p = row * m_width + col;
        return p >= lo && p <= hi;
    }

    void apply_sel_tint()
    {
        if (!sel_has() || !m_grid)
            return;
        uint32_t sel_bg = term_sel_bg();
        for (uint32_t y = 0; y < m_height; y++) {
            for (uint32_t x = 0; x < m_width; x++) {
                if (cell_selected(x, y))
                    m_grid[y * m_width + x].bg = sel_bg;
            }
        }
    }

    void draw_help_overlay()
    {
        static const char *tips[] = {
            "Drag with the mouse to select output, then copy it",   "Ctrl+X copies the selection (Ctrl+C sends SIGINT)",
            "Ctrl+V pastes the clipboard into the shell",           "Right-click pastes the clipboard",
            "Page Up / Page Down and the wheel scroll the history", "Edit > Clear Screen wipes the scrollback",
            "View > Zoom In / Zoom Out changes the text size",
        };
        int win_w = (int)m_window.width;
        int win_h = (int)m_window.height;
        GuiDialogLayout layout = gui_dialog_layout(win_w, win_h, 0, tips, (int)(sizeof(tips) / sizeof(tips[0])), false);
        gui_draw_dialog(&m_window, win_w, win_h, 0, &layout, "Terminal Help", tips,
                        (int)(sizeof(tips) / sizeof(tips[0])), nullptr, "Close", false, false, nullptr, false, false);
        m_help_close = layout.confirm;
    }

    void draw_chrome()
    {
        gui_fill_surface(&m_window, term_frame_bg());
        gui_draw_panel_inset(&m_window, term_pad_x() / 2, term_pad_y() / 2, (int)m_window.width - term_pad_x(),
                             (int)m_window.height - term_pad_y(), term_bg(), term_border(), term_chrome_bg_alt());
    }

    char *history_line(uint32_t index)
    {
        uint32_t phys_idx = (m_history_start + index) % TERM_HISTORY_LINES;
        return m_history_text + ((size_t)phys_idx * TERM_HISTORY_LINE_LEN);
    }

    const char *history_line(uint32_t index) const
    {
        uint32_t phys_idx = (m_history_start + index) % TERM_HISTORY_LINES;
        return m_history_text + ((size_t)phys_idx * TERM_HISTORY_LINE_LEN);
    }

    uint32_t *history_line_fg(uint32_t index)
    {
        uint32_t phys_idx = (m_history_start + index) % TERM_HISTORY_LINES;
        return m_history_fg + ((size_t)phys_idx * TERM_HISTORY_LINE_LEN);
    }

    uint16_t &history_line_len(uint32_t index)
    {
        uint32_t phys_idx = (m_history_start + index) % TERM_HISTORY_LINES;
        return m_history_len[phys_idx];
    }

    const uint16_t &history_line_len(uint32_t index) const
    {
        uint32_t phys_idx = (m_history_start + index) % TERM_HISTORY_LINES;
        return m_history_len[phys_idx];
    }

    void reset_history()
    {
        if (!m_history_len || !m_history_text || !m_history_fg)
            return;
        memset(m_history_len, 0, sizeof(uint16_t) * TERM_HISTORY_LINES);
        memset(m_history_text, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN);
        memset(m_history_fg, 0, (size_t)TERM_HISTORY_LINES * TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        m_history_count = 1;
        m_history_start = 0;
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
            m_history_start = (m_history_start + 1) % TERM_HISTORY_LINES;
            m_history_count = TERM_HISTORY_LINES - 1;
        }

        uint32_t new_logical_idx = m_history_count;
        uint32_t phys_idx = (m_history_start + new_logical_idx) % TERM_HISTORY_LINES;

        char *next_line = m_history_text + ((size_t)phys_idx * TERM_HISTORY_LINE_LEN);
        memset(next_line, 0, TERM_HISTORY_LINE_LEN);
        memset(m_history_fg + ((size_t)phys_idx * TERM_HISTORY_LINE_LEN), 0, TERM_HISTORY_LINE_LEN * sizeof(uint32_t));
        m_history_len[phys_idx] = 0;

        m_history_count++;
        m_history_cursor_col = 0;
        // Do NOT force m_scroll_offset to 0 here: a user reading scrollback
        // while output streams should keep their place. The offset is counted
        // from the bottom, so following live output (offset 0) still works.
        // No full redraw here: the grid diff in render_all() repaints the
        // shifted cells. Forcing a full redraw on every newline made bursty
        // command output redraw the whole window per line.
    }

    void append_history_char(char c)
    {
        if (m_history_count == 0)
            reset_history();
        uint32_t line_index = m_history_count - 1;
        uint16_t len = history_line_len(line_index);
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
        history_line_len(line_index) = new_len;
        m_history_cursor_col = cursor_col + 1;
    }

    void history_backspace()
    {
        if (m_history_count == 0 || m_history_cursor_col == 0)
            return;
        uint32_t line_index = m_history_count - 1;
        uint16_t len = history_line_len(line_index);
        uint32_t cursor_col = m_history_cursor_col - 1;
        char *line = history_line(line_index);
        uint32_t *fg = history_line_fg(line_index);
        if (cursor_col < len) {
            memmove(line + cursor_col, line + cursor_col + 1, (size_t)(len - cursor_col));
            memmove(fg + cursor_col, fg + cursor_col + 1, (size_t)(len - cursor_col) * sizeof(uint32_t));
            len--;
            line[len] = '\0';
            history_line_len(line_index) = len;
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
            uint32_t len = history_line_len((uint32_t)line);
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

        Cell *new_grid = (Cell *)malloc((size_t)new_width * new_height * sizeof(Cell));
        Cell *new_presented = (Cell *)malloc((size_t)new_width * new_height * sizeof(Cell));
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
        sel_clear();
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
            // Cursor home: move to the start of the current line. This must
            // NOT clear the screen (programs send CSI H frequently just to
            // reposition the cursor).
            m_history_cursor_col = 0;
        } else if (final_char == 'K') {
            if (m_history_count == 0)
                return;
            uint32_t line_index = m_history_count - 1;
            char *line = history_line(line_index);
            uint32_t *fg = history_line_fg(line_index);
            uint16_t len = history_line_len(line_index);
            int mode = term_ansi_param_at(m_ansi_buf, 0, 0);
            if (mode == 1) {
                // Erase from the start of the line up to the cursor.
                uint32_t count = m_history_cursor_col;
                if (count > len)
                    count = len;
                if (count > 0) {
                    memmove(line, line + count, (size_t)(len - count));
                    memmove(fg, fg + count, (size_t)(len - count) * sizeof(uint32_t));
                    len = (uint16_t)(len - count);
                    line[len] = '\0';
                    history_line_len(line_index) = len;
                    m_history_cursor_col = 0;
                    m_needs_full_redraw = true;
                }
            } else {
                uint32_t start = mode == 2 ? 0 : m_history_cursor_col;
                if (start < len) {
                    memset(line + start, 0, (size_t)(len - start));
                    memset(fg + start, 0, (size_t)(len - start) * sizeof(uint32_t));
                    history_line_len(line_index) = (uint16_t)start;
                    if (m_history_cursor_col > start)
                        m_history_cursor_col = start;
                    m_needs_full_redraw = true;
                }
            }
        } else if (final_char == 'C') {
            int n = term_ansi_param_at(m_ansi_buf, 0, 1);
            uint32_t line_index = m_history_count > 0 ? m_history_count - 1 : 0;
            uint32_t len = m_history_count > 0 ? history_line_len(line_index) : 0;
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
    uint32_t m_history_start = 0;
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
    bool m_focused = true;
    bool m_blink_on = true;
    bool m_sel_active = false;
    bool m_sel_dragging = false;
    uint32_t m_sel_anchor_col = 0;
    uint32_t m_sel_anchor_row = 0;
    uint32_t m_sel_caret_col = 0;
    uint32_t m_sel_caret_row = 0;
    bool m_help_visible = false;
    Rect m_help_close = {0, 0, 0, 0};
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

static bool term_paste_to(int fd)
{
    char text[MENU_CLIPBOARD_CAP + 1];
    size_t len = 0;
    if (!gui_clipboard_paste(text, sizeof(text), &len) || len == 0)
        return false;
    size_t written = 0;
    while (written < len) {
        int n = write(fd, text + written, len - written);
        if (n < 0)
            return false;
        written += (size_t)n;
    }
    return true;
}

static bool term_copy_selection(TerminalEmulator &term)
{
    char text[MENU_CLIPBOARD_CAP + 1];
    size_t len = term.sel_extract(text, sizeof(text));
    if (len == 0)
        return false;
    return gui_clipboard_copy(text, len);
}

// Clamp, persist, and report whether the zoom level actually changed.
static bool term_apply_zoom(int new_zoom)
{
    if (new_zoom < TERM_ZOOM_MIN)
        new_zoom = TERM_ZOOM_MIN;
    if (new_zoom > TERM_ZOOM_MAX)
        new_zoom = TERM_ZOOM_MAX;
    if (new_zoom == g_term_zoom)
        return false;
    g_term_zoom = new_zoom;
    app_setting_save_int("terminal_zoom", g_term_zoom);
    return true;
}

static void term_publish_menus(TerminalEmulator &term, bool clipboard_nonempty)
{
    MenuModel model;
    gui_menu_model_reset(&model);

    int edit = gui_menu_model_add_menu(&model, "Edit");
    gui_menu_model_add_item(&model, edit, "Copy", TERM_MENU_COPY, term.sel_has() ? 0 : MENU_FLAG_DISABLED, "Ctrl+X");
    gui_menu_model_add_item(&model, edit, "Paste", TERM_MENU_PASTE, clipboard_nonempty ? 0 : MENU_FLAG_DISABLED,
                            "Ctrl+V");
    gui_menu_model_add_item(&model, edit, "Select All", TERM_MENU_SELECT_ALL, 0, nullptr);
    gui_menu_model_add_separator(&model, edit);
    gui_menu_model_add_item(&model, edit, "Clear Screen", TERM_MENU_CLEAR, 0, nullptr);

    int view = gui_menu_model_add_menu(&model, "View");
    gui_menu_model_add_item(&model, view, "Zoom In", TERM_MENU_ZOOM_IN,
                            g_term_zoom >= TERM_ZOOM_MAX ? MENU_FLAG_DISABLED : 0, nullptr);
    gui_menu_model_add_item(&model, view, "Zoom Out", TERM_MENU_ZOOM_OUT,
                            g_term_zoom <= TERM_ZOOM_MIN ? MENU_FLAG_DISABLED : 0, nullptr);
    gui_menu_model_add_item(&model, view, "Actual Size", TERM_MENU_ZOOM_ACTUAL,
                            g_term_zoom == 0 ? MENU_FLAG_DISABLED : 0, nullptr);

    app_menus_add_help(&model, TERM_MENU_HELP);

    gui_menu_publish(&model);
}

static void term_handle_menu_command(TerminalEmulator &term, uint32_t cmd, int shell_fd, bool *shell_alive,
                                     bool *needs_render)
{
    switch (cmd) {
        case TERM_MENU_COPY:
            term_copy_selection(term);
            break;
        case TERM_MENU_PASTE:
            if (*shell_alive)
                term_paste_to(shell_fd);
            break;
        case TERM_MENU_SELECT_ALL:
            term.sel_all();
            *needs_render = true;
            break;
        case TERM_MENU_CLEAR:
            term.clear_screen();
            *needs_render = true;
            break;
        case TERM_MENU_ZOOM_IN:
            if (term_apply_zoom(g_term_zoom + 1)) {
                term.reflow_for_font();
                *needs_render = true;
            }
            break;
        case TERM_MENU_ZOOM_OUT:
            if (term_apply_zoom(g_term_zoom - 1)) {
                term.reflow_for_font();
                *needs_render = true;
            }
            break;
        case TERM_MENU_ZOOM_ACTUAL:
            if (term_apply_zoom(0)) {
                term.reflow_for_font();
                *needs_render = true;
            }
            break;
        case TERM_MENU_HELP:
            term.show_help();
            *needs_render = true;
            break;
        default:
            break;
    }
}

// The terminal owns an extra event source (the shell pipe via epoll) and an
// incremental renderer, so it runs the libapp runtime in manual mode:
// app_create/app_pump handle window lifecycle, resize, theme and menubar
// plumbing, while the shell loop below keeps its own pacing.
struct TermRun
{
    TerminalEmulator *term;
    bool needs_render;
    bool shell_alive;
    int pipe_to_shell;
    uint64_t last_activity_ticks;
    bool menu_focus_dirty;
};

static void term_event(App *app, const Event *ev)
{
    TermRun *run = (TermRun *)app_user(app);
    TerminalEmulator &term = *run->term;

    switch (ev->type) {
        case EVT_WINDOW_RESIZE:
            term.refresh_window(*app_window(app));
            if (term.sync_resize())
                run->needs_render = true;
            break;

        case EVT_FOCUS:
            term.set_focused(true);
            run->last_activity_ticks = get_ticks();
            run->menu_focus_dirty = true;
            run->needs_render = true;
            break;

        case EVT_UNFOCUS:
            term.set_focused(false);
            run->needs_render = true;
            break;

        case EVT_MOUSE_SCROLL: {
            int delta = ev->mouse.scroll_y * 3;
            term.scroll_history(delta);
            run->needs_render = true;
            break;
        }

        case EVT_MOUSE_LEAVE:
            term.sel_stop_drag();
            break;

        case EVT_MOUSE_DOWN:
            if (term.help_visible()) {
                if (ev->mouse.button == 1) {
                    term.hide_help();
                    run->needs_render = true;
                }
                break;
            }
            if (ev->mouse.button == 1) {
                term.sel_start(ev->mouse.x, ev->mouse.y);
                run->needs_render = true;
            } else if (ev->mouse.button == 2) {
                if (run->shell_alive)
                    term_paste_to(run->pipe_to_shell);
                run->last_activity_ticks = get_ticks();
                run->needs_render = true;
            }
            break;

        case EVT_MOUSE_MOVE:
            if (term.sel_extend(ev->mouse.x, ev->mouse.y))
                run->needs_render = true;
            break;

        case EVT_MOUSE_UP:
            if (ev->mouse.button == 1) {
                term.sel_finish();
                run->needs_render = true;
            }
            break;

        case EVT_KEY_DOWN: {
            if (ev->key.c == 0)
                break;
            char c = (char)ev->key.c;
            if (term.help_visible()) {
                if ((uint8_t)c == 27 || c == '\n' || c == '\r') {
                    term.hide_help();
                    run->needs_render = true;
                }
                break;
            }
            if ((uint8_t)c == KEY_PAGEUP) {
                term.scroll_history((int)term.height() / 2);
                run->needs_render = true;
                break;
            }
            if ((uint8_t)c == KEY_PAGEDOWN) {
                term.scroll_history(-(int)term.height() / 2);
                run->needs_render = true;
                break;
            }
            if ((uint8_t)c == 24) { // Ctrl+X copies the selection (Ctrl+C is SIGINT).
                term_copy_selection(term);
                break;
            }
            if ((uint8_t)c == 22) { // Ctrl+V pastes the clipboard into the shell.
                if (run->shell_alive)
                    term_paste_to(run->pipe_to_shell);
                run->last_activity_ticks = get_ticks();
                run->needs_render = true;
                break;
            }

            if (term.get_scroll_offset() > 0) {
                term.reset_scroll();
                run->needs_render = true;
            }

            if (run->shell_alive) {
                if (write(run->pipe_to_shell, &c, 1) < 0)
                    run->shell_alive = false;
            }
            run->last_activity_ticks = get_ticks();
            break;
        }

        default:
            break;
    }
}

static void term_menu(App *app, uint32_t cmd)
{
    TermRun *run = (TermRun *)app_user(app);
    term_handle_menu_command(*run->term, cmd, run->pipe_to_shell, &run->shell_alive, &run->needs_render);
}

static void term_settings(App *app)
{
    TermRun *run = (TermRun *)app_user(app);
    run->term->theme_changed();
    run->needs_render = true;
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    g_term_zoom = app_setting_load_int("terminal_zoom", 0);
    if (g_term_zoom < TERM_ZOOM_MIN)
        g_term_zoom = TERM_ZOOM_MIN;
    if (g_term_zoom > TERM_ZOOM_MAX)
        g_term_zoom = TERM_ZOOM_MAX;

    gui_fonts_init();

    AppConfig config = {};
    config.title = "Terminal";
    config.width = (int)(80 * term_cell_w() + (uint32_t)term_pad_x() * 2u + TERM_SCROLLBAR_RESERVE);
    config.height = (int)(25 * term_cell_h() + (uint32_t)term_pad_y() * 2u);
    config.min_width = (int)(term_cell_w() * 48u + (uint32_t)term_pad_x() * 2u + TERM_SCROLLBAR_RESERVE);
    config.min_height = (int)(term_cell_h() * 14u + (uint32_t)term_pad_y() * 2u);
    config.flags = WIN_FLAG_RESIZABLE;
    config.on_event = term_event;
    config.on_menu = term_menu;
    config.on_settings = term_settings;

    static TermRun run = {};
    App *app = app_create(&config, &run);
    if (!app)
        return 1;

    static TerminalEmulator term;
    term.init(80, 25, *app_window(app));
    if (!term.ready()) {
        app_destroy(app);
        return 1;
    }
    run.term = &term;
    run.shell_alive = true;
    run.last_activity_ticks = get_ticks();
    run.menu_focus_dirty = true;

    term.render_all();

    bool last_menu_sel = term.sel_has();
    bool last_menu_clip = false;
    int last_menu_zoom = g_term_zoom;

    int pipe_to_shell[2];
    int pipe_from_shell[2];
    if (pipe(pipe_to_shell) < 0 || pipe(pipe_from_shell) < 0) {
        term.write_string("terminal: failed to create pipes\n");
        term.render_all();
        app_destroy(app);
        return 1;
    }

    int shell_pid = fork();
    if (shell_pid < 0) {
        term.write_string("terminal: failed to fork child process\n");
        term.render_all();
        app_destroy(app);
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
    run.pipe_to_shell = pipe_to_shell[1];

    int epfd = epoll_create(1);
    if (epfd < 0) {
        term.write_string("terminal: failed to create epoll instance\n");
        term.render_all();
        app_destroy(app);
        return 1;
    }

    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = pipe_from_shell[0];
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, pipe_from_shell[0], &ev) < 0) {
        term.write_string("terminal: failed to configure epoll\n");
        term.render_all();
        app_destroy(app);
        return 1;
    }

    bool last_blink_on = true;
    while (true) {
        bool saw_event = false;
        run.needs_render = false;

        // 1. Drain GUI events, theme sync and menubar commands through the
        //    runtime; the callbacks above set run.needs_render.
        if (!app_pump(app))
            break;

        // 2. Poll/Read shell output if alive. Drain everything the pipe
        //    holds before rendering so bursty output costs one redraw, not
        //    one redraw per chunk (the visible "word-by-word" effect).
        if (run.shell_alive) {
            int timeout = (run.needs_render) ? 0 : 16;
            struct epoll_event events[1];
            int n = epoll_wait(epfd, events, 1, timeout);
            char read_buf[4096];
            for (int drain = 0; run.shell_alive && n > 0 && drain < 16; drain++) {
                int bytes_read = read(pipe_from_shell[0], read_buf, sizeof(read_buf));
                if (bytes_read > 0) {
                    term.write_bytes(read_buf, (size_t)bytes_read);
                    run.needs_render = true;
                    saw_event = true;
                    run.last_activity_ticks = get_ticks();
                    // read() blocks on an empty pipe: only keep draining
                    // while epoll says more data is already pending.
                    n = epoll_wait(epfd, events, 1, 0);
                    continue;
                }
                if (bytes_read == 0) {
                    run.shell_alive = false;
                    term.write_string("\r\n[Process completed]\r\n");
                    run.needs_render = true;
                    saw_event = true;
                }
                break;
            }
        }

        // Republish the menu model when Copy/Paste availability or focus
        // changes so the menubar's enabled-state stays honest.
        Registry *registry = gui_registry();
        bool clip_nonempty = registry && registry->clipboard_len > 0;
        bool sel_has_now = term.sel_has();
        if (term.focused() && (run.menu_focus_dirty || sel_has_now != last_menu_sel ||
                               clip_nonempty != last_menu_clip || g_term_zoom != last_menu_zoom)) {
            term_publish_menus(term, clip_nonempty);
            last_menu_sel = sel_has_now;
            last_menu_clip = clip_nonempty;
            last_menu_zoom = g_term_zoom;
            run.menu_focus_dirty = false;
        }

        // 3. Cursor blink: solid for a moment after activity, then a steady
        //    530 ms phase while focused; hidden when the window is unfocused.
        uint64_t now_ticks = get_ticks();
        bool blink_on;
        if (!term.focused()) {
            blink_on = false;
        } else if (now_ticks - run.last_activity_ticks < 530) {
            blink_on = true;
        } else {
            blink_on = (((now_ticks - run.last_activity_ticks) / 530) % 2) == 0;
        }
        if (blink_on != last_blink_on) {
            last_blink_on = blink_on;
            term.set_blink_on(blink_on);
            run.needs_render = true;
        }

        if (run.needs_render) {
            term.render_all();
        } else if (!run.shell_alive && !saw_event) {
            sleep_ms(16);
        }
    }

    app_destroy(app);
    return 0;
}
