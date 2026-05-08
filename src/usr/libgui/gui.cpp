#include "gui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/syscalls.h>
#include <unistd.h>

#include "../libc/log.h"
#include "../libc/syscall.h"

static Registry *g_registry = NULL;
extern "C" {
WindowEntry *g_my_window = NULL;
}
static int g_window_shm_id = WIN_SHM_INVALID;
static uint32_t g_window_buffer_w = 0;
static uint32_t g_window_buffer_h = 0;
static constexpr int GUI_RETIRED_WINDOW_BUFFER_SLOTS = 4;
struct RetiredWindowBuffer
{
    int shm_id;
    uint32_t generation;
};
static RetiredWindowBuffer g_retired_window_buffers[GUI_RETIRED_WINDOW_BUFFER_SLOTS] = {};
static int g_ui_scale_pct = 0;
static constexpr int k_system_menu_gap_px = 6;
static constexpr int k_system_menu_item_h_px = 24;
static constexpr int k_system_menu_item_count = 9;
static constexpr int k_system_menu_extra_h_px = 6;

#include <drivers/video/font.h>

extern "C" {
Theme g_current_theme = {};
GuiStylePalette g_gui_style = {};
GuiChromePalette g_gui_chrome = {};
}

static const Theme k_gui_theme_dark = {0xFF111214, 0xFFF2F2F0, 0xFF6E7784, 0xFF2F343A, 0xFF1A1C20, 0xFF15171A};

static const Theme k_gui_theme_light = {0xFFF4F5F7, 0xFF15181D, 0xFF4B6EAE, 0xFFD6D9E0, 0xFFF6F7F9, 0xFFF6F7F9};

static const GuiStylePalette k_gui_style_dark = {0xFF111214, 0xFF15171A, 0xFF1A1D21, 0xFF1D2025, 0xFF242830, 0xFF2B3037,
                                                 0xFF333942, 0xFF555E6A, 0xFF484F59, 0xFF626C78, 0xFF2A2F36, 0xFFF2F2F0,
                                                 0xFFC5C8CC, 0xFF8E949C, 0xFF55B36A, 0xFFE2AF45, 0xFFFF625E};

static const GuiStylePalette k_gui_style_light = {
    0xFFF4F5F7, 0xFFFFFFFF, 0xFFF8F9FB, 0xFFF0F2F5, 0xFFE9EDF3, 0xFFD7DCE4, 0xFFD4D9E2, 0xFF4B6EAE, 0xFFB9C4D5,
    0xFF4B6EAE, 0xFFDDE6F4, 0xFF15181D, 0xFF5D6470, 0xFF808792, 0xFF267A46, 0xFF9A6A00, 0xFFD43D35};

static const GuiChromePalette k_gui_chrome_dark = {0xFF101113, 0xFF17191D, 0xFF1E2126, 0xFF181A1F, 0xFF23272D,
                                                   0xFFF2F2F0, 0xFF9A9FA7, 0x22000000, 0xFF333942, 0xFFFF5F57,
                                                   0xFFF0C04D, 0xFF45C16B, 0xFFEDEDEB, 0xFFF2F2F0};

static const GuiChromePalette k_gui_chrome_light = {0xFFF4F5F7, 0xFFE6E9EF, 0xFFF7F8FA, 0xFFF0F2F5, 0xFFE9EDF3,
                                                    0xFF15181D, 0xFF6E7580, 0x1A000000, 0xFFC9CED8, 0xFFFF6159,
                                                    0xFFF1BC4C, 0xFF36BD64, 0xFF6D7584, 0xFF15181D};

static GuiThemeMode g_applied_theme_mode = GUI_THEME_DARK;
static bool g_theme_tables_init = false;

static uint32_t g_font_mask[256][8];
static bool g_font_mask_init = false;

static void init_font_masks()
{
    if (g_font_mask_init)
        return;
    for (int i = 0; i < 256; i++) {
        for (int col = 0; col < 8; col++) {
            g_font_mask[i][col] = (i & (1 << (7 - col))) ? 0xFFFFFFFF : 0;
        }
    }
    g_font_mask_init = true;
}

static bool theme_tables_initialized()
{
    return g_theme_tables_init;
}

static bool gui_clip_rect_to_bounds(int32_t *x, int32_t *y, int32_t *w, int32_t *h, int32_t max_w, int32_t max_h)
{
    if (!x || !y || !w || !h || *w <= 0 || *h <= 0 || max_w <= 0 || max_h <= 0)
        return false;

    int64_t left = *x;
    int64_t top = *y;
    int64_t right = left + (int64_t)*w;
    int64_t bottom = top + (int64_t)*h;

    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > max_w)
        right = max_w;
    if (bottom > max_h)
        bottom = max_h;
    if (right <= left || bottom <= top)
        return false;

    *x = (int32_t)left;
    *y = (int32_t)top;
    *w = (int32_t)(right - left);
    *h = (int32_t)(bottom - top);
    return true;
}

static uint32_t next_window_buffer_generation()
{
    if (!g_my_window)
        return 1;
    uint32_t next = (g_my_window->buffer_generation == 0xFFFFFFFFu) ? 1u : g_my_window->buffer_generation + 1u;
    return next;
}

static bool gui_surface_layout(uint32_t width, uint32_t height, uint32_t *pitch_out, size_t *bytes_out)
{
    if (pitch_out)
        *pitch_out = 0;
    if (bytes_out)
        *bytes_out = 0;
    if (width == 0 || height == 0)
        return false;

    uint64_t pitch64 = (uint64_t)width * 4u;
    uint64_t bytes64 = pitch64 * (uint64_t)height;
    if (pitch64 > 0xFFFFFFFFu || bytes64 == 0 || bytes64 > (uint64_t)SIZE_MAX)
        return false;

    if (pitch_out)
        *pitch_out = (uint32_t)pitch64;
    if (bytes_out)
        *bytes_out = (size_t)bytes64;
    return true;
}

static void gui_window_register_cleanup(int shm_id, bool mapped)
{
    if (mapped)
        syscall1(SYS_SHM_UNMAP, (uint64_t)shm_id);
    if (gui_shm_id_is_valid(shm_id))
        syscall1(SYS_SHM_FREE, (uint64_t)shm_id);
}

static bool gui_reserve_window_slot(int slot)
{
    if (!g_registry || slot < 0 || slot >= MAX_WINDOWS)
        return false;
    return __sync_bool_compare_and_swap(&g_registry->windows[slot].shm_id, WIN_SHM_INVALID, WIN_SHM_RESERVED);
}

static void gui_publish_window_count_for_slot(int slot)
{
    if (!g_registry || slot < 0)
        return;

    uint32_t needed = static_cast<uint32_t>(slot) + 1u;
    while (true) {
        uint32_t cur = g_registry->window_count;
        if (cur >= needed)
            return;
        if (__sync_bool_compare_and_swap(&g_registry->window_count, cur, needed))
            return;
        syscall1(SYS_YIELD, 0);
    }
}

static uint32_t gui_resize_capacity(uint32_t current, uint32_t target)
{
    if (target <= current)
        return current;

    uint32_t slack = current / 4u;
    if (slack < 64u)
        slack = 64u;

    uint64_t grown = (uint64_t)current + (uint64_t)slack;
    if (grown < target)
        grown = target;
    grown = (grown + 63u) & ~63u;
    if (grown > 0xFFFFFFFFu)
        return target;
    return static_cast<uint32_t>(grown);
}

static void gui_init_retired_window_buffers()
{
    for (int i = 0; i < GUI_RETIRED_WINDOW_BUFFER_SLOTS; i++) {
        g_retired_window_buffers[i].shm_id = WIN_SHM_INVALID;
        g_retired_window_buffers[i].generation = 0;
    }
}

static bool gui_generation_reached(uint32_t acked, uint32_t generation)
{
    if (generation == 0 || acked == 0)
        return false;
    return static_cast<int32_t>(acked - generation) >= 0;
}

static void gui_release_retired_window_buffer()
{
    if (!g_my_window)
        return;

    uint32_t acked = g_my_window->buffer_ack_generation;
    for (int i = 0; i < GUI_RETIRED_WINDOW_BUFFER_SLOTS; i++) {
        RetiredWindowBuffer &retired = g_retired_window_buffers[i];
        if (!gui_shm_id_is_valid(retired.shm_id) || retired.generation == 0)
            continue;
        if (!gui_generation_reached(acked, retired.generation))
            continue;

        syscall1(SYS_SHM_FREE, static_cast<uint64_t>(retired.shm_id));
        retired.shm_id = WIN_SHM_INVALID;
        retired.generation = 0;
    }
}

static int gui_find_retired_window_buffer_slot()
{
    gui_release_retired_window_buffer();
    for (int i = 0; i < GUI_RETIRED_WINDOW_BUFFER_SLOTS; i++) {
        if (!gui_shm_id_is_valid(g_retired_window_buffers[i].shm_id))
            return i;
    }
    return -1;
}

static bool gui_resize_window_backing(Surface *s, uint32_t target_w, uint32_t target_h)
{
    if (!s || !g_my_window || !gui_shm_id_is_valid(g_window_shm_id) || target_w == 0 || target_h == 0)
        return false;
    if (target_w <= g_window_buffer_w && target_h <= g_window_buffer_h)
        return true;

    // Previously published backing stores may still be visible to the compositor.
    // Keep a small retire queue so rapid interactive resizes can grow again before
    // the first compositor acknowledgement arrives.
    int retired_slot = gui_find_retired_window_buffer_slot();
    if (retired_slot < 0)
        return false;

    uint32_t alloc_w = gui_resize_capacity(g_window_buffer_w, target_w);
    uint32_t alloc_h = gui_resize_capacity(g_window_buffer_h, target_h);
    uint64_t shm_bytes = (uint64_t)alloc_w * (uint64_t)alloc_h * 4u;
    if (shm_bytes == 0 || shm_bytes > 0x1000000ULL) {
        alloc_w = target_w;
        alloc_h = target_h;
        shm_bytes = (uint64_t)alloc_w * (uint64_t)alloc_h * 4u;
        if (shm_bytes == 0 || shm_bytes > 0x1000000ULL)
            return false;
    }

    int new_shm_id = static_cast<int>(syscall1(SYS_SHM_GET, shm_bytes));
    if (new_shm_id < 0)
        return false;

    uint64_t mapped = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(new_shm_id));
    if (mapped == 0 || mapped == static_cast<uint64_t>(-1)) {
        syscall1(SYS_SHM_FREE, static_cast<uint64_t>(new_shm_id));
        return false;
    }

    uint32_t *new_buffer = reinterpret_cast<uint32_t *>(mapped);
    const bool transparent = (g_my_window->flags & WIN_FLAG_TRANSPARENT) != 0;
    if (transparent) {
        memset(new_buffer, 0, static_cast<size_t>(shm_bytes));
    } else {
        const uint32_t fill = theme_tables_initialized() ? g_gui_style.app_bg : 0xFF15171Au;
        const uint64_t pixels = (uint64_t)alloc_w * (uint64_t)alloc_h;
        for (uint64_t i = 0; i < pixels; i++)
            new_buffer[i] = fill;
    }

    uint32_t copy_w = (s->width < alloc_w) ? s->width : alloc_w;
    uint32_t copy_h = (s->height < alloc_h) ? s->height : alloc_h;
    uint32_t old_stride = s->pitch / 4;
    for (uint32_t row = 0; row < copy_h; row++) {
        memcpy(&new_buffer[static_cast<size_t>(row) * alloc_w], &s->buffer[static_cast<size_t>(row) * old_stride],
               static_cast<size_t>(copy_w) * 4u);
    }

    int old_shm_id = g_window_shm_id;
    uint32_t generation = next_window_buffer_generation();
    s->buffer = new_buffer;
    s->pitch = alloc_w * 4u;
    if (s->width > alloc_w)
        s->width = alloc_w;
    if (s->height > alloc_h)
        s->height = alloc_h;
    g_window_shm_id = new_shm_id;
    g_window_buffer_w = alloc_w;
    g_window_buffer_h = alloc_h;

    g_my_window->buffer_w = static_cast<int>(alloc_w);
    g_my_window->buffer_h = static_cast<int>(alloc_h);
    g_my_window->shm_id = new_shm_id;
    g_my_window->buffer_generation = generation;
    // Allocation alone is not a resize commit. The client publishes
    // buffer_resize_serial only after it has redrawn and committed damage for
    // the current resize_serial.
    asm volatile("sfence" ::: "memory");

    syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(old_shm_id));
    g_retired_window_buffers[retired_slot].shm_id = old_shm_id;
    g_retired_window_buffers[retired_slot].generation = generation;
    return true;
}

static void copy_theme_tables(GuiThemeMode mode)
{
    const Theme *theme = &k_gui_theme_dark;
    const GuiStylePalette *style = &k_gui_style_dark;
    const GuiChromePalette *chrome = &k_gui_chrome_dark;
    if (mode == GUI_THEME_LIGHT) {
        theme = &k_gui_theme_light;
        style = &k_gui_style_light;
        chrome = &k_gui_chrome_light;
    }

    g_current_theme = *theme;
    g_gui_style = *style;
    g_gui_chrome = *chrome;
    asm volatile("sfence" ::: "memory");
    g_applied_theme_mode = mode;
    g_theme_tables_init = true;
}

static int clamp_metric(int value, int min_value, int max_value)
{
    if (value < min_value)
        return min_value;
    if (value > max_value)
        return max_value;
    return value;
}

static int detect_framebuffer_scale_pct()
{
    uint32_t info[4] = {};
    if (fb_info(info) != 0)
        return 100;

    uint32_t width = info[0];
    uint32_t height = info[1];
    uint32_t short_edge = width < height ? width : height;

    // Pixel resolution is only a weak density hint. A 2560x1440 desktop monitor should remain 1:1 unless the font
    // set or a future DPI source asks for larger UI metrics.
    if (short_edge >= 2160)
        return 125;
    if (short_edge >= 1800)
        return 112;
    if (short_edge <= 720)
        return 96;
    return 100;
}

static int resolve_ui_scale_pct()
{
    if (g_ui_scale_pct != 0)
        return g_ui_scale_pct;

    gui_fonts_init();
    const GuiFont *font = gui_font_default();
    int pixel_size = (font && font->pixel_size > 0) ? static_cast<int>(font->pixel_size) : 12;
    int font_scale_pct = 100 + (pixel_size - 12) * 4;
    int framebuffer_scale_pct = detect_framebuffer_scale_pct();
    int resolved = font_scale_pct > framebuffer_scale_pct ? font_scale_pct : framebuffer_scale_pct;
    g_ui_scale_pct = clamp_metric(resolved, 96, 125);
    return g_ui_scale_pct;
}

static int scaled_metric_floor(int base_px)
{
    int scale_pct = resolve_ui_scale_pct();
    return (base_px * scale_pct + 50) / 100;
}

static int popup_menu_outer_pad_x()
{
    return gui_scaled_metric(6);
}

static int popup_menu_outer_pad_y()
{
    return gui_scaled_metric(6);
}

static int popup_menu_row_gap()
{
    return gui_scaled_metric(1);
}

static int popup_menu_row_pad_x()
{
    return gui_scaled_metric(13);
}

static int popup_menu_separator_inset()
{
    return gui_scaled_metric(12);
}

static int popup_menu_separator_h()
{
    return popup_menu_row_gap() * 2 + 1;
}

static int popup_menu_content_width(const GuiMenuItem *items, int count, int min_width)
{
    int width = min_width - popup_menu_outer_pad_x() * 2;
    if (width < 0)
        width = 0;
    width -= popup_menu_row_pad_x() * 2;
    if (width < 0)
        width = 0;

    for (int i = 0; i < count; i++) {
        if (!items[i].label || items[i].separator)
            continue;
        int item_w = gui_measure_text(gui_font_default(), items[i].label);
        if (item_w > width)
            width = item_w;
    }
    return width;
}

static int popup_menu_inner_width(const GuiMenuItem *items, int count, int min_width)
{
    return popup_menu_content_width(items, count, min_width) + popup_menu_row_pad_x() * 2 +
           popup_menu_outer_pad_x() * 2;
}

static int popup_menu_item_y_offset(const GuiMenuItem *items, int count, int index)
{
    int y = popup_menu_outer_pad_y();
    int gap = popup_menu_row_gap();
    int item_h = gui_popup_menu_item_h();
    for (int i = 0; i < count && i < index; i++) {
        y += items[i].separator ? popup_menu_separator_h() : item_h;
        if (i + 1 < count)
            y += gap;
    }
    return y;
}

extern "C" {

Surface gui_init_framebuffer(void)
{
    init_font_masks();
    gui_fonts_init();
    copy_theme_tables(g_applied_theme_mode);
    Surface s = {0, 0, 0, 0, false};
    uint32_t info[4];
    if (fb_info(info) == 0) {
        s.width = info[0];
        s.height = info[1];
        s.pitch = info[2];
        s.buffer = reinterpret_cast<uint32_t *>(fb_mmap());
        s.owns_buffer = false;
    }
    return s;
}

Surface gui_create_surface(uint32_t width, uint32_t height)
{
    Surface s = {0, 0, 0, 0, false};
    size_t size = 0;
    if (!gui_surface_layout(width, height, &s.pitch, &size))
        return s;

    s.width = width;
    s.height = height;
    s.buffer = static_cast<uint32_t *>(malloc(size));
    s.owns_buffer = (s.buffer != nullptr);
    if (s.buffer)
        memset(s.buffer, 0, size);
    return s;
}

void gui_destroy_surface(Surface *s)
{
    if (!s)
        return;
    if (s->owns_buffer && s->buffer)
        free(s->buffer);
    s->buffer = nullptr;
    s->width = 0;
    s->height = 0;
    s->pitch = 0;
    s->owns_buffer = false;
    s->display_handle = 0;
}

void gui_draw_pixel(Surface *s, int32_t x, int32_t y, uint32_t color)
{
    if (!s || !s->buffer || x < 0 || y < 0 || x >= static_cast<int32_t>(s->width) ||
        y >= static_cast<int32_t>(s->height))
        return;
    s->buffer[y * (s->pitch / 4) + x] = color;
}

void gui_fill_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!s || !s->buffer || s->pitch == 0 || w <= 0 || h <= 0)
        return;
    if (!gui_clip_rect_to_bounds(&x, &y, &w, &h, static_cast<int32_t>(s->width), static_cast<int32_t>(s->height)))
        return;

    uint32_t pitch_u32 = s->pitch / 4;
    uint32_t *first_row = &s->buffer[static_cast<size_t>(y) * pitch_u32 + static_cast<size_t>(x)];

    int32_t i = 0;
    for (; i + 7 < w; i += 8) {
        first_row[i + 0] = color;
        first_row[i + 1] = color;
        first_row[i + 2] = color;
        first_row[i + 3] = color;
        first_row[i + 4] = color;
        first_row[i + 5] = color;
        first_row[i + 6] = color;
        first_row[i + 7] = color;
    }
    for (; i < w; i++)
        first_row[i] = color;

    if (h > 1) {
        size_t row_bytes = static_cast<size_t>(w) * sizeof(uint32_t);
        for (int32_t py = 1; py < h; py++) {
            memcpy(&s->buffer[static_cast<size_t>(y + py) * pitch_u32 + static_cast<size_t>(x)], first_row, row_bytes);
        }
    }
}

void gui_draw_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    gui_fill_rect(s, x, y, w, 1, color);
    gui_fill_rect(s, x, y + h - 1, w, 1, color);
    gui_fill_rect(s, x, y, 1, h, color);
    gui_fill_rect(s, x + w - 1, y, 1, h, color);
}

static inline uint8_t scale_alpha_u8(uint8_t alpha, uint8_t coverage)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(alpha) * static_cast<uint32_t>(coverage) + 127u) / 255u);
}

static inline uint32_t blend_pixel(uint32_t dst, uint32_t src, uint8_t coverage)
{
    uint32_t src_a = (src >> 24) & 0xFFu;
    if (coverage < 255)
        src_a = (src_a * coverage + 127u) / 255u;
    if (src_a == 0)
        return dst;
    if (src_a == 255)
        return 0xFF000000u | (src & 0x00FFFFFFu);

    uint32_t dst_a = (dst >> 24) & 0xFFu;
    if (dst_a == 0)
        return (src_a << 24) | (src & 0x00FFFFFFu);

    if (dst_a == 255) {
        uint32_t inv_a = 255u - src_a;
        uint32_t s_rb = src & 0x00FF00FFu;
        uint32_t d_rb = dst & 0x00FF00FFu;

        uint32_t rb = (s_rb * src_a + d_rb * inv_a + 0x00800080u);
        rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
        rb &= 0x00FF00FFu;

        uint32_t s_g = (src >> 8) & 0xFFu;
        uint32_t d_g = (dst >> 8) & 0xFFu;
        uint32_t g = (s_g * src_a + d_g * inv_a + 127u) / 255u;

        return 0xFF000000u | rb | (g << 8);
    }

    // Full ARGB blend
    uint32_t inv_a = 255u - src_a;
    uint32_t out_a = src_a + (dst_a * inv_a + 127u) / 255u;
    if (out_a == 0)
        return 0;

    auto blend_ch = [&](uint32_t s, uint32_t d, uint32_t sa, uint32_t da) {
        return (s * sa * 255u + d * da * inv_a + (out_a * 127u)) / (out_a * 255u);
    };

    uint32_t r = blend_ch((src >> 16) & 0xFFu, (dst >> 16) & 0xFFu, src_a, dst_a);
    uint32_t g = blend_ch((src >> 8) & 0xFFu, (dst >> 8) & 0xFFu, src_a, dst_a);
    uint32_t b = blend_ch(src & 0xFFu, dst & 0xFFu, src_a, dst_a);
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

static inline void paint_pixel_coverage(uint32_t *dst, uint32_t color, uint8_t coverage, uint8_t base_alpha)
{
    if (!dst || coverage == 0)
        return;
    if (coverage == 255 && base_alpha == 255)
        *dst = color;
    else
        *dst = blend_pixel(*dst, color, coverage);
}

static void paint_solid_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!s || !s->buffer || w <= 0 || h <= 0)
        return;

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (base_alpha == 0)
        return;
    if (base_alpha == 255) {
        gui_fill_rect(s, x, y, w, h, color);
        return;
    }

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x >= static_cast<int32_t>(s->width) || y >= static_cast<int32_t>(s->height))
        return;
    if (x + w > static_cast<int32_t>(s->width))
        w = static_cast<int32_t>(s->width) - x;
    if (y + h > static_cast<int32_t>(s->height))
        h = static_cast<int32_t>(s->height) - y;
    if (w <= 0 || h <= 0)
        return;

    uint32_t pitch = s->pitch / 4;
    for (int32_t py = 0; py < h; py++) {
        uint32_t *dst = &s->buffer[static_cast<size_t>(y + py) * pitch + x];
        for (int32_t px = 0; px < w; px++)
            dst[px] = blend_pixel(dst[px], color, 255);
    }
}

static inline float libgui_fabsf(float x)
{
    return x < 0.0f ? -x : x;
}
static inline float libgui_maxf(float a, float b)
{
    return a > b ? a : b;
}

static inline float libgui_sqrt(float n)
{
    if (n <= 0.0f)
        return 0.0f;
    float x = n;
    float y = 1.0f;
    float e = 0.000001f;
    for (int i = 0; i < 15; i++) {
        x = (x + y) * 0.5f;
        y = n / x;
        if (libgui_fabsf(x - y) <= e)
            break;
    }
    return x;
}

static constexpr int k_round_aa_samples = 8;
static constexpr int k_round_aa_total = k_round_aa_samples * k_round_aa_samples;
static const float k_round_aa_offsets[k_round_aa_samples] = {0.0625f, 0.1875f, 0.3125f, 0.4375f,
                                                             0.5625f, 0.6875f, 0.8125f, 0.9375f};
static constexpr int k_round_mask_cache_entries = 16;
static constexpr uint32_t GUI_ROUNDED_EDGE_TOP = 1u;
static constexpr uint32_t GUI_ROUNDED_EDGE_BOTTOM = 2u;
static constexpr uint32_t GUI_ROUNDED_EDGE_ALL = GUI_ROUNDED_EDGE_TOP | GUI_ROUNDED_EDGE_BOTTOM;

struct RoundedCornerMaskCacheEntry
{
    int radius = 0;
    int scale_pct = 0;
    uint32_t age = 0;
    uint8_t *fill = nullptr;
};

static RoundedCornerMaskCacheEntry g_round_mask_cache[k_round_mask_cache_entries];
static uint32_t g_round_mask_cache_age = 1;

static inline uint8_t rounded_hits_to_alpha(int hits)
{
    return hits <= 0 ? 0
                     : (hits >= k_round_aa_total
                            ? 255
                            : static_cast<uint8_t>((hits * 255 + k_round_aa_total / 2) / k_round_aa_total));
}

static uint8_t *build_rounded_corner_fill_mask(int radius)
{
    if (radius <= 0)
        return nullptr;
    size_t count = static_cast<size_t>(radius) * static_cast<size_t>(radius);
    uint8_t *mask = static_cast<uint8_t *>(malloc(count));
    if (!mask)
        return nullptr;

    float rr = static_cast<float>(radius) * static_cast<float>(radius);
    for (int row = 0; row < radius; row++) {
        for (int col = 0; col < radius; col++) {
            int hits = 0;
            for (int sy = 0; sy < k_round_aa_samples; sy++) {
                float sample_y = static_cast<float>(row) + k_round_aa_offsets[sy];
                for (int sx = 0; sx < k_round_aa_samples; sx++) {
                    float sample_x = static_cast<float>(col) + k_round_aa_offsets[sx];
                    float dx = sample_x - static_cast<float>(radius);
                    float dy = sample_y - static_cast<float>(radius);
                    if (dx * dx + dy * dy <= rr)
                        hits++;
                }
            }
            mask[static_cast<size_t>(row) * static_cast<size_t>(radius) + static_cast<size_t>(col)] =
                rounded_hits_to_alpha(hits);
        }
    }
    return mask;
}

static const RoundedCornerMaskCacheEntry *get_rounded_corner_mask_entry(int radius)
{
    if (radius <= 0)
        return nullptr;
    int scale_pct = gui_ui_scale_pct();
    RoundedCornerMaskCacheEntry *victim = &g_round_mask_cache[0];

    for (int i = 0; i < k_round_mask_cache_entries; i++) {
        RoundedCornerMaskCacheEntry &entry = g_round_mask_cache[i];
        if (entry.fill && entry.radius == radius && entry.scale_pct == scale_pct) {
            entry.age = ++g_round_mask_cache_age;
            return &entry;
        }
        if (!entry.fill) {
            victim = &entry;
            break;
        }
        if (entry.age < victim->age)
            victim = &entry;
    }

    uint8_t *mask = build_rounded_corner_fill_mask(radius);
    if (!mask)
        return nullptr;
    if (victim->fill)
        free(victim->fill);
    victim->radius = radius;
    victim->scale_pct = scale_pct;
    victim->age = ++g_round_mask_cache_age;
    victim->fill = mask;
    return victim;
}

static inline uint8_t rounded_corner_mask_alpha(const RoundedCornerMaskCacheEntry *entry, int local_x, int local_y)
{
    if (!entry || !entry->fill || local_x < 0 || local_y < 0 || local_x >= entry->radius || local_y >= entry->radius)
        return 0;
    return entry
        ->fill[static_cast<size_t>(local_y) * static_cast<size_t>(entry->radius) + static_cast<size_t>(local_x)];
}

uint8_t gui_rounded_rect_coverage_local(int32_t col, int32_t row, int32_t w, int32_t h, int32_t r,
                                        uint32_t rounded_edges)
{
    if (w <= 0 || h <= 0 || col < 0 || row < 0 || col >= w || row >= h)
        return 0;
    if ((rounded_edges & GUI_ROUNDED_EDGE_ALL) == 0)
        return 255;
    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r <= 0)
        return 255;

    bool top_band = (rounded_edges & GUI_ROUNDED_EDGE_TOP) && row < r;
    bool bottom_band = (rounded_edges & GUI_ROUNDED_EDGE_BOTTOM) && row >= h - r;
    if (!top_band && !bottom_band)
        return 255;
    if (col >= r && col < w - r)
        return 255;

    const RoundedCornerMaskCacheEntry *entry = get_rounded_corner_mask_entry(r);
    if (!entry) {
        float center_x = (col < r) ? static_cast<float>(r) : static_cast<float>(w - r);
        float center_y = top_band ? static_cast<float>(r) : static_cast<float>(h - r);
        float rr = static_cast<float>(r) * static_cast<float>(r);
        int hits = 0;
        for (int sy = 0; sy < k_round_aa_samples; sy++) {
            float sample_y = static_cast<float>(row) + k_round_aa_offsets[sy];
            for (int sx = 0; sx < k_round_aa_samples; sx++) {
                float sample_x = static_cast<float>(col) + k_round_aa_offsets[sx];
                float dx = sample_x - center_x;
                float dy = sample_y - center_y;
                if (dx * dx + dy * dy <= rr)
                    hits++;
            }
        }
        return rounded_hits_to_alpha(hits);
    }

    int local_x = (col < r) ? col : (w - 1 - col);
    int local_y = top_band ? row : (h - 1 - row);
    return rounded_corner_mask_alpha(entry, local_x, local_y);
}

static inline uint8_t rounded_rect_stroke_coverage_local(int32_t col, int32_t row, int32_t w, int32_t h, int32_t r)
{
    uint8_t outer = gui_rounded_rect_coverage_local(col, row, w, h, r, GUI_ROUNDED_EDGE_ALL);
    if (outer == 0)
        return 0;
    if (w <= 2 || h <= 2)
        return outer;

    int32_t inner_w = w - 2;
    int32_t inner_h = h - 2;
    int32_t inner_r = r > 0 ? r - 1 : 0;
    uint8_t inner = gui_rounded_rect_coverage_local(col - 1, row - 1, inner_w, inner_h, inner_r, GUI_ROUNDED_EDGE_ALL);
    return inner >= outer ? 0 : static_cast<uint8_t>(outer - inner);
}

static inline uint8_t circle_fill_coverage(int32_t px, int32_t py, int32_t cx, int32_t cy, int32_t r)
{
    if (r <= 0)
        return 0;
    float rr = static_cast<float>(r) * static_cast<float>(r);
    float center_x = static_cast<float>(cx);
    float center_y = static_cast<float>(cy);
    float edge_dist =
        libgui_sqrt((px + 0.5f - center_x) * (px + 0.5f - center_x) + (py + 0.5f - center_y) * (py + 0.5f - center_y));
    if (edge_dist <= static_cast<float>(r) - 0.75f)
        return 255;
    if (edge_dist >= static_cast<float>(r) + 0.75f)
        return 0;

    int hits = 0;
    for (int sy = 0; sy < k_round_aa_samples; sy++) {
        float sample_y = static_cast<float>(py) + k_round_aa_offsets[sy] - center_y;
        for (int sx = 0; sx < k_round_aa_samples; sx++) {
            float sample_x = static_cast<float>(px) + k_round_aa_offsets[sx] - center_x;
            if (sample_x * sample_x + sample_y * sample_y <= rr)
                hits++;
        }
    }
    return rounded_hits_to_alpha(hits);
}

void gui_fill_rounded_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (!s || !s->buffer || w <= 0 || h <= 0)
        return;
    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r == 0) {
        gui_fill_rect(s, x, y, w, h, color);
        return;
    }

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (base_alpha == 0)
        return;
    uint32_t pitch = s->pitch / 4;

    paint_solid_rect(s, x + r, y, w - r * 2, h, color);
    paint_solid_rect(s, x, y + r, r, h - r * 2, color);
    paint_solid_rect(s, x + w - r, y + r, r, h - r * 2, color);

    const RoundedCornerMaskCacheEntry *entry = get_rounded_corner_mask_entry(r);
    for (int corner = 0; corner < 4; corner++) {
        bool right = (corner & 1) != 0;
        bool bottom = (corner & 2) != 0;
        int32_t cx0 = right ? x + w - r : x;
        int32_t cy0 = bottom ? y + h - r : y;
        int32_t start_y = cy0 < 0 ? 0 : cy0;
        int32_t end_y = cy0 + r;
        if (end_y > static_cast<int32_t>(s->height))
            end_y = static_cast<int32_t>(s->height);
        int32_t start_x = cx0 < 0 ? 0 : cx0;
        int32_t end_x = cx0 + r;
        if (end_x > static_cast<int32_t>(s->width))
            end_x = static_cast<int32_t>(s->width);

        for (int32_t py = start_y; py < end_y; py++) {
            uint32_t *dst_row = &s->buffer[static_cast<size_t>(py) * pitch];
            for (int32_t px = start_x; px < end_x; px++) {
                uint8_t coverage = 0;
                if (entry) {
                    int32_t local_x = right ? (x + w - 1 - px) : (px - x);
                    int32_t local_y = bottom ? (y + h - 1 - py) : (py - y);
                    coverage = rounded_corner_mask_alpha(entry, local_x, local_y);
                } else {
                    coverage = gui_rounded_rect_coverage_local(px - x, py - y, w, h, r, GUI_ROUNDED_EDGE_ALL);
                }
                paint_pixel_coverage(&dst_row[px], color, coverage, base_alpha);
            }
        }
    }
}

void gui_draw_rounded_rect(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (!s || !s->buffer || w <= 0 || h <= 0)
        return;
    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;
    if (r == 0 || w <= 2 || h <= 2) {
        gui_draw_rect(s, x, y, w, h, color);
        return;
    }

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (base_alpha == 0)
        return;
    uint32_t pitch = s->pitch / 4;

    paint_solid_rect(s, x + r, y, w - r * 2, 1, color);
    paint_solid_rect(s, x + r, y + h - 1, w - r * 2, 1, color);
    paint_solid_rect(s, x, y + r, 1, h - r * 2, color);
    paint_solid_rect(s, x + w - 1, y + r, 1, h - r * 2, color);

    for (int corner = 0; corner < 4; corner++) {
        bool right = (corner & 1) != 0;
        bool bottom = (corner & 2) != 0;
        int32_t cx0 = right ? x + w - r : x;
        int32_t cy0 = bottom ? y + h - r : y;
        int32_t start_y = cy0 < 0 ? 0 : cy0;
        int32_t end_y = cy0 + r;
        if (end_y > static_cast<int32_t>(s->height))
            end_y = static_cast<int32_t>(s->height);
        int32_t start_x = cx0 < 0 ? 0 : cx0;
        int32_t end_x = cx0 + r;
        if (end_x > static_cast<int32_t>(s->width))
            end_x = static_cast<int32_t>(s->width);

        for (int32_t py = start_y; py < end_y; py++) {
            int32_t row = py - y;
            uint32_t *dst_row = &s->buffer[static_cast<size_t>(py) * pitch];
            for (int32_t px = start_x; px < end_x; px++) {
                uint8_t coverage = rounded_rect_stroke_coverage_local(px - x, row, w, h, r);
                paint_pixel_coverage(&dst_row[px], color, coverage, base_alpha);
            }
        }
    }
}

void gui_fill_circle(Surface *s, int32_t x, int32_t y, int32_t r, uint32_t color)
{
    if (!s || !s->buffer || r <= 0)
        return;
    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (base_alpha == 0)
        return;
    uint32_t pitch = s->pitch / 4;

    int32_t start_y = y - r;
    if (start_y < 0)
        start_y = 0;
    int32_t end_y = y + r + 1;
    if (end_y > static_cast<int32_t>(s->height))
        end_y = static_cast<int32_t>(s->height);
    int32_t start_x = x - r;
    if (start_x < 0)
        start_x = 0;
    int32_t end_x = x + r + 1;
    if (end_x > static_cast<int32_t>(s->width))
        end_x = static_cast<int32_t>(s->width);

    for (int32_t py = start_y; py < end_y; py++) {
        uint32_t *dst_row = &s->buffer[static_cast<size_t>(py) * pitch];
        for (int32_t px = start_x; px < end_x; px++) {
            uint8_t coverage = circle_fill_coverage(px, py, x, y, r);
            if (coverage == 0)
                continue;
            uint32_t *dst = &dst_row[px];
            if (coverage == 255 && base_alpha == 255)
                *dst = color;
            else
                *dst = blend_pixel(*dst, color, coverage);
        }
    }
}

void gui_draw_circle_stroke(Surface *s, int32_t x, int32_t y, int32_t r, int32_t thickness, uint32_t color)
{
    if (!s || !s->buffer || r <= 0)
        return;

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (base_alpha == 0)
        return;

    if (thickness <= 0)
        thickness = 1;
    if (thickness >= r) {
        gui_fill_circle(s, x, y, r, color);
        return;
    }

    int32_t inner_r = r - thickness;
    uint32_t pitch = s->pitch / 4;

    int32_t start_y = y - r;
    if (start_y < 0)
        start_y = 0;
    int32_t end_y = y + r + 1;
    if (end_y > static_cast<int32_t>(s->height))
        end_y = static_cast<int32_t>(s->height);
    int32_t start_x = x - r;
    if (start_x < 0)
        start_x = 0;
    int32_t end_x = x + r + 1;
    if (end_x > static_cast<int32_t>(s->width))
        end_x = static_cast<int32_t>(s->width);

    for (int32_t py = start_y; py < end_y; py++) {
        uint32_t *dst_row = &s->buffer[static_cast<size_t>(py) * pitch];
        for (int32_t px = start_x; px < end_x; px++) {
            uint8_t outer = circle_fill_coverage(px, py, x, y, r);
            if (outer == 0)
                continue;

            uint8_t inner = (inner_r > 0) ? circle_fill_coverage(px, py, x, y, inner_r) : 0;
            uint8_t coverage = inner >= outer ? 0 : static_cast<uint8_t>(outer - inner);
            if (coverage == 0)
                continue;

            uint32_t *dst = &dst_row[px];
            if (coverage == 255 && base_alpha == 255)
                *dst = color;
            else
                *dst = blend_pixel(*dst, color, coverage);
        }
    }
}

void gui_draw_char(Surface *s, int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg)
{
    if (gui_fonts_init()) {
        char text[2] = {c, '\0'};
        gui_draw_text(s, gui_font_default(), x, y, text, fg, bg);
        return;
    }
    if (!s || !s->buffer || x < 0 || y < 0 || x + 8 > static_cast<int32_t>(s->width) ||
        y + 16 > static_cast<int32_t>(s->height))
        return;
    init_font_masks();

    const uint8_t *glyph = font8x16[static_cast<uint8_t>(c)];
    uint32_t pitch_u32 = s->pitch / 4;
    uint32_t *row_ptr = s->buffer + (static_cast<size_t>(y) * pitch_u32) + static_cast<size_t>(x);

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        const uint32_t *mask = g_font_mask[bits];
        row_ptr[0] = (mask[0] & fg) | (~mask[0] & bg);
        row_ptr[1] = (mask[1] & fg) | (~mask[1] & bg);
        row_ptr[2] = (mask[2] & fg) | (~mask[2] & bg);
        row_ptr[3] = (mask[3] & fg) | (~mask[3] & bg);
        row_ptr[4] = (mask[4] & fg) | (~mask[4] & bg);
        row_ptr[5] = (mask[5] & fg) | (~mask[5] & bg);
        row_ptr[6] = (mask[6] & fg) | (~mask[6] & bg);
        row_ptr[7] = (mask[7] & fg) | (~mask[7] & bg);
        row_ptr += pitch_u32;
    }
}

void gui_draw_string(Surface *s, int32_t x, int32_t y, const char *str, uint32_t fg, uint32_t bg)
{
    if (!str)
        return;
    if (gui_fonts_init()) {
        gui_draw_text(s, gui_font_default(), x, y, str, fg, bg);
        return;
    }
    int32_t cur_x = x;
    while (*str) {
        if (*str == '\n') {
            cur_x = x;
            y += 16;
        } else {
            gui_draw_char(s, cur_x, y, *str, fg, bg);
            cur_x += 8;
        }
        str++;
    }
}

int gui_measure_text_n(const GuiFont *font, const char *str, size_t len)
{
    if (!str || len == 0)
        return 0;

    int width = 0;
    size_t i = 0;
    for (; i < len && str[i] && str[i] != '\n'; i++) {
        uint8_t ch = static_cast<uint8_t>(str[i]);
        if (!font) {
            width += 8;
            continue;
        }
        if (ch < 128u) {
            width += font->ascii_advance[ch] > 0 ? font->ascii_advance[ch] : gui_font_max_advance(font);
        } else {
            const GuiGlyph *glyph = nullptr;
            if (font->glyphs && font->glyph_count > 0) {
                if (font->fallback_index < font->glyph_count)
                    glyph = &font->glyphs[font->fallback_index];
                else
                    glyph = &font->glyphs[0];
            }
            width += glyph ? glyph->advance_x : gui_font_max_advance(font);
        }
    }
    return width;
}

static constexpr size_t k_gui_clip_text_limit = 255;

static size_t gui_bounded_clip_text_len(const char *str, size_t limit)
{
    if (!str)
        return 0;

    size_t len = 0;
    while (len < limit && str[len] && str[len] != '\n')
        len++;
    return len;
}

size_t gui_truncate_text(const GuiFont *font, const char *str, int max_width, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';
    if (!str || max_width <= 0)
        return 0;

    size_t len = gui_bounded_clip_text_len(str, k_gui_clip_text_limit);
    int full_width = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t ch = static_cast<uint8_t>(str[i]);
        if (!font) {
            full_width += 8;
        } else if (ch < 128u) {
            full_width += font->ascii_advance[ch] > 0 ? font->ascii_advance[ch] : gui_font_max_advance(font);
        } else {
            const GuiGlyph *glyph = nullptr;
            if (font->glyphs && font->glyph_count > 0) {
                if (font->fallback_index < font->glyph_count)
                    glyph = &font->glyphs[font->fallback_index];
                else
                    glyph = &font->glyphs[0];
            }
            full_width += glyph ? glyph->advance_x : gui_font_max_advance(font);
        }
    }

    if (full_width <= max_width) {
        size_t copy_len = len;
        if (copy_len >= out_size)
            copy_len = out_size - 1;
        memcpy(out, str, copy_len);
        out[copy_len] = '\0';
        return copy_len;
    }

    static const char *ellipsis = "...";
    int ellipsis_w = gui_measure_text(font, ellipsis);
    if (ellipsis_w > max_width)
        return 0;

    int target_width = max_width - ellipsis_w;
    size_t clipped = 0;
    int clipped_width = 0;
    while (clipped < len && str[clipped]) {
        uint8_t ch = static_cast<uint8_t>(str[clipped]);
        int advance = 8;
        if (font) {
            if (ch < 128u) {
                advance = font->ascii_advance[ch] > 0 ? font->ascii_advance[ch] : gui_font_max_advance(font);
            } else {
                const GuiGlyph *glyph = nullptr;
                if (font->glyphs && font->glyph_count > 0) {
                    if (font->fallback_index < font->glyph_count)
                        glyph = &font->glyphs[font->fallback_index];
                    else
                        glyph = &font->glyphs[0];
                }
                advance = glyph ? glyph->advance_x : gui_font_max_advance(font);
            }
        }
        if (clipped_width + advance > target_width)
            break;
        clipped_width += advance;
        clipped++;
    }

    if (clipped >= out_size)
        clipped = out_size - 1;
    memcpy(out, str, clipped);
    size_t dot_count = 3;
    if (clipped + dot_count >= out_size)
        dot_count = out_size - clipped - 1;
    memcpy(out + clipped, ellipsis, dot_count);
    out[clipped + dot_count] = '\0';
    return clipped + dot_count;
}

void gui_draw_text_clipped(Surface *s, const GuiFont *font, int32_t x, int32_t y, int32_t max_width, const char *str,
                           uint32_t fg, uint32_t bg)
{
    if (!s || !s->buffer || !str || max_width <= 0)
        return;

    char safe_text[256];
    size_t safe_len = gui_bounded_clip_text_len(str, sizeof(safe_text) - 1u);
    for (size_t i = 0; i < safe_len; i++)
        safe_text[i] = str[i];
    safe_text[safe_len] = '\0';

    if (gui_measure_text(font, safe_text) <= max_width) {
        if (font)
            gui_draw_text(s, font, x, y, safe_text, fg, bg);
        else
            gui_draw_string(s, x, y, safe_text, fg, bg);
        return;
    }

    char clipped[256];
    size_t len = gui_truncate_text(font, safe_text, max_width, clipped, sizeof(clipped));
    if (len == 0)
        return;
    if (font)
        gui_draw_text(s, font, x, y, clipped, fg, bg);
    else
        gui_draw_string(s, x, y, clipped, fg, bg);
}

void gui_blit(Surface *dest, Surface *src, int32_t dest_x, int32_t dest_y)
{
    if (!dest || !dest->buffer || !src || !src->buffer)
        return;
    gui_blit_rect(dest, src, dest_x, dest_y, 0, 0, static_cast<int32_t>(src->width), static_cast<int32_t>(src->height));
}

void gui_blit_alpha(Surface *dest, Surface *src, int32_t dx, int32_t dy)
{
    if (!dest || !dest->buffer || !src || !src->buffer || dest->pitch == 0 || src->pitch == 0)
        return;

    int32_t sx = 0;
    int32_t sy = 0;
    int32_t w = static_cast<int32_t>(src->width);
    int32_t h = static_cast<int32_t>(src->height);

    if (dx < 0) {
        int64_t skip = -(int64_t)dx;
        if (skip > 0x7FFFFFFF)
            return;
        sx = static_cast<int32_t>(skip);
        w -= sx;
        dx = 0;
    }
    if (dy < 0) {
        int64_t skip = -(int64_t)dy;
        if (skip > 0x7FFFFFFF)
            return;
        sy = static_cast<int32_t>(skip);
        h -= sy;
        dy = 0;
    }
    if (w <= 0 || h <= 0)
        return;
    if (dx >= static_cast<int32_t>(dest->width) || dy >= static_cast<int32_t>(dest->height))
        return;
    if (sx >= static_cast<int32_t>(src->width) || sy >= static_cast<int32_t>(src->height))
        return;
    if (static_cast<int64_t>(sx) + w > static_cast<int32_t>(src->width))
        w = static_cast<int32_t>(src->width) - sx;
    if (static_cast<int64_t>(sy) + h > static_cast<int32_t>(src->height))
        h = static_cast<int32_t>(src->height) - sy;
    if (static_cast<int64_t>(dx) + w > static_cast<int32_t>(dest->width))
        w = static_cast<int32_t>(dest->width) - dx;
    if (static_cast<int64_t>(dy) + h > static_cast<int32_t>(dest->height))
        h = static_cast<int32_t>(dest->height) - dy;
    if (w <= 0 || h <= 0)
        return;

    uint32_t dp = dest->pitch / 4;
    uint32_t sp = src->pitch / 4;

    for (int32_t y = 0; y < h; y++) {
        uint32_t *drow = &dest->buffer[static_cast<size_t>(dy + y) * dp + static_cast<size_t>(dx)];
        uint32_t *srow = &src->buffer[static_cast<size_t>(sy + y) * sp + static_cast<size_t>(sx)];
        for (int32_t x = 0; x < w; x++) {
            uint32_t pixel = srow[x];
            uint8_t alpha = static_cast<uint8_t>(pixel >> 24);
            if (alpha == 0)
                continue;
            if (alpha == 255)
                drow[x] = pixel;
            else
                drow[x] = blend_pixel(drow[x], pixel, 255);
        }
    }
}

void gui_blit_rect(Surface *dest, Surface *src, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    if (!dest || !dest->buffer || !src || !src->buffer || dest->pitch == 0 || src->pitch == 0 || w <= 0 || h <= 0)
        return;

    int64_t dst_x = dx;
    int64_t dst_y = dy;
    int64_t src_x = sx;
    int64_t src_y = sy;
    int64_t copy_w = w;
    int64_t copy_h = h;

    if (src_x < 0) {
        dst_x -= src_x;
        copy_w += src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        dst_y -= src_y;
        copy_h += src_y;
        src_y = 0;
    }
    if (dst_x < 0) {
        src_x -= dst_x;
        copy_w += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y -= dst_y;
        copy_h += dst_y;
        dst_y = 0;
    }
    if (copy_w <= 0 || copy_h <= 0)
        return;
    if (src_x >= src->width || src_y >= src->height || dst_x >= dest->width || dst_y >= dest->height)
        return;
    if (src_x + copy_w > src->width)
        copy_w = static_cast<int64_t>(src->width) - src_x;
    if (src_y + copy_h > src->height)
        copy_h = static_cast<int64_t>(src->height) - src_y;
    if (dst_x + copy_w > dest->width)
        copy_w = static_cast<int64_t>(dest->width) - dst_x;
    if (dst_y + copy_h > dest->height)
        copy_h = static_cast<int64_t>(dest->height) - dst_y;
    if (copy_w <= 0 || copy_h <= 0)
        return;

    dx = static_cast<int32_t>(dst_x);
    dy = static_cast<int32_t>(dst_y);
    sx = static_cast<int32_t>(src_x);
    sy = static_cast<int32_t>(src_y);
    w = static_cast<int32_t>(copy_w);
    h = static_cast<int32_t>(copy_h);

    uint32_t dp_u32 = dest->pitch / 4;
    uint32_t sp_u32 = src->pitch / 4;
    bool same_buffer = dest->buffer == src->buffer;
    bool overlap = false;
    if (same_buffer) {
        overlap = !(static_cast<int64_t>(dx) + w <= sx || static_cast<int64_t>(sx) + w <= dx ||
                    static_cast<int64_t>(dy) + h <= sy || static_cast<int64_t>(sy) + h <= dy);
    }

    size_t row_bytes = static_cast<size_t>(w) * sizeof(uint32_t);

    if (!overlap && dx == 0 && sx == 0 && static_cast<uint32_t>(w) == sp_u32 && static_cast<uint32_t>(w) == dp_u32) {
        memcpy(&dest->buffer[static_cast<size_t>(dy) * dp_u32], &src->buffer[static_cast<size_t>(sy) * sp_u32],
               row_bytes * static_cast<size_t>(h));
        return;
    }

    if (!overlap && dx == 0 && sx == 0 && static_cast<uint32_t>(w) == src->width &&
        static_cast<uint32_t>(w) == dest->width) {
        memcpy(&dest->buffer[static_cast<size_t>(dy) * dp_u32], &src->buffer[static_cast<size_t>(sy) * sp_u32],
               row_bytes * static_cast<size_t>(h));
        return;
    }

    int32_t start_y = 0;
    int32_t end_y = h;
    int32_t step_y = 1;
    if (same_buffer && overlap && dy > sy) {
        start_y = h - 1;
        end_y = -1;
        step_y = -1;
    }

    for (int32_t y = start_y; y != end_y; y += step_y) {
        uint32_t *d_row = &dest->buffer[static_cast<size_t>(dy + y) * dp_u32 + static_cast<size_t>(dx)];
        uint32_t *s_row = &src->buffer[static_cast<size_t>(sy + y) * sp_u32 + static_cast<size_t>(sx)];
        if (same_buffer && overlap)
            memmove(d_row, s_row, row_bytes);
        else
            memcpy(d_row, s_row, row_bytes);
    }
}

void gui_blit_rect_fill(Surface *dest, Surface *src, int32_t dx, int32_t dy, int32_t sx, int32_t sy, int32_t w,
                        int32_t h, uint32_t fill)
{
    if (!dest || !dest->buffer || !src || !src->buffer || w <= 0 || h <= 0)
        return;
    gui_fill_rect(dest, dx, dy, w, h, fill);
    gui_blit_rect(dest, src, dx, dy, sx, sy, w, h);
}

int gui_commit_window_damage(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!s || !s->buffer || !g_my_window || s->pitch == 0)
        return -1;

    int32_t damage_max_w = s->width > 0 ? static_cast<int32_t>(s->width) : g_my_window->w;
    int32_t damage_max_h = s->height > 0 ? static_cast<int32_t>(s->height) : g_my_window->h;
    if (!gui_clip_rect_to_bounds(&x, &y, &w, &h, damage_max_w, damage_max_h))
        return -1;

    // Window clients must publish damage to the compositor, never present their
    // backing store directly to the framebuffer. Direct presents race with WM
    // composition and repaint rectangular pixels over rounded frame corners,
    // borders, titlebars, and moved/resized regions.
    asm volatile("sfence" ::: "memory");
    damage_push(&g_my_window->damage, x, y, w, h);
    uint32_t resize_serial = g_my_window->resize_serial;
    if (resize_serial != 0 && g_my_window->buffer_resize_serial != resize_serial &&
        g_my_window->w > 0 && g_my_window->h > 0 &&
        s->width >= static_cast<uint32_t>(g_my_window->w) &&
        s->height >= static_cast<uint32_t>(g_my_window->h)) {
        g_my_window->buffer_resize_serial = resize_serial;
    }
    asm volatile("sfence" ::: "memory");
    return 0;
}

int gui_poll_frame(uint64_t *frame_ticks, uint32_t *completed_sequence)
{
    DisplayEvent event = {};
    if (display_poll_event(&event) != 0)
        return 0;
    if (event.type != DISPLAY_EVENT_FLIP_COMPLETE && event.type != DISPLAY_EVENT_VBLANK)
        return 0;
    if (frame_ticks)
        *frame_ticks = event.timestamp_ticks;
    if (completed_sequence)
        *completed_sequence = event.sequence;
    return 1;
}

int gui_wait_frame(uint64_t *frame_ticks, uint32_t *completed_sequence)
{
    for (;;) {
        DisplayEvent event = {};
        if (display_wait_event(&event) != 0)
            return -1;
        if (event.type != DISPLAY_EVENT_FLIP_COMPLETE && event.type != DISPLAY_EVENT_VBLANK)
            continue;
        if (frame_ticks)
            *frame_ticks = event.timestamp_ticks;
        if (completed_sequence)
            *completed_sequence = event.sequence;
        return 1;
    }
}

void gui_blit_to_screen_rect(Surface *src, int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!src || !src->buffer || src->pitch == 0)
        return;

    if (g_my_window) {
        (void)gui_commit_window_damage(src, x, y, w, h);
        return;
    }

    if (!gui_clip_rect_to_bounds(&x, &y, &w, &h, static_cast<int32_t>(src->width), static_cast<int32_t>(src->height)))
        return;

    Rect rect = gui_rect_make(x, y, w, h);
    DisplayPresentRequest req = {};
    req.buffer = src->buffer + (static_cast<size_t>(y) * (src->pitch / 4) + static_cast<size_t>(x));
    req.stride = src->pitch / 4;
    req.rects = &rect;
    req.rect_count = 1;
    req.frame_sequence = 0;
    req.flags = DISPLAY_PRESENT_VBLANK;
    req.source_origin_x = x;
    req.source_origin_y = y;
    display_present(&req);
}

Surface gui_register_window_ex(const char *title, uint32_t w, uint32_t h, uint32_t flags)
{
    const char *window_title = (title && *title) ? title : "Window";

    if (!g_registry) {
        uint64_t reg_ptr = syscall1(SYS_SHM_MAP, 0);
        if (reg_ptr == 0 || reg_ptr == static_cast<uint64_t>(-1)) {
            LOG_ERROR("gui", "register_window failed: no registry for %s", window_title);
            return {NULL, 0, 0, 0, false};
        }
        g_registry = reinterpret_cast<Registry *>(reg_ptr);

        int timeout = 0;
        while (g_registry->magic != REGISTRY_MAGIC && timeout < 1000) {
            syscall1(SYS_YIELD, 0);
            timeout++;
        }
        if (timeout >= 1000) {
            LOG_ERROR("gui", "register_window timed out: %s", window_title);
            g_registry = NULL;
            return {NULL, 0, 0, 0, false};
        }
    }

    uint32_t buffer_pitch = 0;
    size_t buffer_bytes = 0;
    if (!gui_surface_layout(w, h, &buffer_pitch, &buffer_bytes)) {
        LOG_ERROR("gui", "register_window invalid size: %s (%ux%u)", window_title, w, h);
        return {NULL, 0, 0, 0, false};
    }

    uint32_t buffer_w = w;
    uint32_t buffer_h = h;

    int shm_id = static_cast<int>(syscall1(SYS_SHM_GET, static_cast<uint64_t>(buffer_bytes)));
    if (shm_id < 0) {
        LOG_ERROR("gui", "register_window SHM_GET failed: %s (%ux%u)", window_title, w, h);
        return {NULL, 0, 0, 0, false};
    }

    uint64_t win_ptr = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(shm_id));
    if (win_ptr == 0 || win_ptr == static_cast<uint64_t>(-1)) {
        LOG_ERROR("gui", "register_window SHM_MAP failed: %s shm=%d", window_title, shm_id);
        gui_window_register_cleanup(shm_id, false);
        return {NULL, 0, 0, 0, false};
    }
    memset(reinterpret_cast<void *>(win_ptr), 0, buffer_bytes);

    int win_idx = -1;
    if (strcmp(window_title, "Menubar") == 0) {
        if (gui_reserve_window_slot(0))
            win_idx = 0;
    } else if (strcmp(window_title, "Dock") == 0) {
        if (gui_reserve_window_slot(1))
            win_idx = 1;
    } else {
        for (int i = 2; i < MAX_WINDOWS; i++) {
            if (gui_reserve_window_slot(i)) {
                win_idx = i;
                break;
            }
        }
    }

    if (win_idx < 0 || win_idx >= MAX_WINDOWS) {
        LOG_ERROR("gui", "register_window table full or slot busy: %s", window_title);
        gui_window_register_cleanup(shm_id, true);
        return {NULL, 0, 0, 0, false};
    }

    gui_publish_window_count_for_slot(win_idx);

    WindowEntry *win_entry = &g_registry->windows[win_idx];
    memset(win_entry, 0, sizeof(*win_entry));
    win_entry->shm_id = WIN_SHM_RESERVED;
    win_entry->x = 100 + (win_idx * 40);
    win_entry->y = 100 + (win_idx * 40);
    win_entry->w = static_cast<int>(w);
    win_entry->h = static_cast<int>(h);
    win_entry->restore_x = win_entry->x;
    win_entry->restore_y = win_entry->y;
    win_entry->restore_w = win_entry->w;
    win_entry->restore_h = win_entry->h;
    win_entry->buffer_w = static_cast<int>(buffer_w);
    win_entry->buffer_h = static_cast<int>(buffer_h);
    win_entry->min_w = 0;
    win_entry->min_h = 0;
    win_entry->title[0] = '\0';
    strncpy(win_entry->title, window_title, 63);
    win_entry->title[63] = '\0';
    win_entry->flags = flags;
    win_entry->owner_pid = static_cast<uint32_t>(syscall1(SYS_GETPID, 0));
    win_entry->state = WIN_NORMAL;
    win_entry->resize_serial = 0;
    win_entry->buffer_resize_serial = 0;
    win_entry->buffer_generation = 1u;
    win_entry->buffer_ack_generation = 1u;
    win_entry->active = false;
    win_entry->ready = false;
    win_entry->request_close = false;
    win_entry->request_focus = false;
    win_entry->request_minimize = false;
    win_entry->request_maximize = false;
    win_entry->request_restore = false;
    damage_reset(&win_entry->damage);

    asm volatile("sfence" ::: "memory");
    win_entry->shm_id = shm_id;
    asm volatile("sfence" ::: "memory");
    win_entry->ready = true;
    g_my_window = win_entry;
    g_window_shm_id = shm_id;
    g_window_buffer_w = buffer_w;
    g_window_buffer_h = buffer_h;
    gui_init_retired_window_buffers();

    return {reinterpret_cast<uint32_t *>(win_ptr), w, h, buffer_pitch, false};
}

Surface gui_register_window(const char *title, uint32_t w, uint32_t h)
{
    return gui_register_window_ex(title, w, h, 0);
}

int gui_set_window_owner_pid(uint32_t pid)
{
    if (!g_my_window)
        return -1;
    if (pid == 0) {
        pid = static_cast<uint32_t>(syscall1(SYS_GETPID, 0));
    }
    g_my_window->owner_pid = pid;
    asm volatile("sfence" ::: "memory");
    return 0;
}

int gui_request_focus(void)
{
    if (!g_my_window)
        return -1;
    g_my_window->request_focus = true;
    asm volatile("sfence" ::: "memory");
    return 0;
}

int gui_window_set_min_size(int width, int height)
{
    if (!g_my_window)
        return -1;
    g_my_window->min_w = (width > 0) ? width : 0;
    g_my_window->min_h = (height > 0) ? height : 0;
    asm volatile("sfence" ::: "memory");
    return 0;
}

int gui_window_get_min_size(int *width, int *height)
{
    if (!g_my_window)
        return -1;
    if (width)
        *width = g_my_window->min_w;
    if (height)
        *height = g_my_window->min_h;
    return 0;
}

int gui_sync_window_size(Surface *s)
{
    if (!s || !g_my_window)
        return -1;

    gui_release_retired_window_buffer();

    uint32_t new_w = (g_my_window->w > 0) ? static_cast<uint32_t>(g_my_window->w) : s->width;
    uint32_t new_h = (g_my_window->h > 0) ? static_cast<uint32_t>(g_my_window->h) : s->height;
    bool requested_resize =
        (g_my_window->flags & WIN_FLAG_RESIZABLE) != 0 && (new_w != g_window_buffer_w || new_h != g_window_buffer_h);
    if (requested_resize && !gui_resize_window_backing(s, new_w, new_h)) {
        new_w = g_window_buffer_w;
        new_h = g_window_buffer_h;
    }

    uint32_t max_w = s->pitch / 4;
    if (new_w > max_w)
        new_w = max_w;
    if (g_window_buffer_h > 0 && new_h > g_window_buffer_h)
        new_h = g_window_buffer_h;

    if (s->width == new_w && s->height == new_h)
        return 0;
    s->width = new_w;
    s->height = new_h;
    return 1;
}

int gui_set_content_size(Surface *s, int content_w, int content_h)
{
    if (!s || !g_my_window)
        return -1;

    int view_w = g_my_window->w > 0 ? g_my_window->w : static_cast<int>(s->width);
    int view_h = g_my_window->h > 0 ? g_my_window->h : static_cast<int>(s->height);
    if (content_w < view_w)
        content_w = view_w;
    if (content_h < view_h)
        content_h = view_h;

    if (static_cast<uint32_t>(content_w) > g_window_buffer_w || static_cast<uint32_t>(content_h) > g_window_buffer_h) {
        if (!gui_resize_window_backing(s, static_cast<uint32_t>(content_w), static_cast<uint32_t>(content_h)))
            return -1;
    }

    s->width = static_cast<uint32_t>(content_w);
    s->height = static_cast<uint32_t>(content_h);
    g_my_window->content_w = content_w;
    g_my_window->content_h = content_h;
    asm volatile("sfence" ::: "memory");
    return 0;
}

Registry *gui_registry(void)
{
    if (!g_registry) {
        uint64_t reg_ptr = syscall1(SYS_SHM_MAP, 0);
        if (reg_ptr != 0 && reg_ptr != static_cast<uint64_t>(-1)) {
            g_registry = reinterpret_cast<Registry *>(reg_ptr);
        }
    }
    return g_registry;
}

void gui_apply_theme(GuiThemeMode mode)
{
    copy_theme_tables(mode == GUI_THEME_LIGHT ? GUI_THEME_LIGHT : GUI_THEME_DARK);
}

bool gui_sync_theme_from_registry(void)
{
    Registry *registry = gui_registry();
    if (!registry)
        return false;

    asm volatile("lfence" ::: "memory");
    if (!theme_tables_initialized()) {
        copy_theme_tables(GUI_THEME_DARK);
    }

    GuiThemeMode next = (registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
    if (next != g_applied_theme_mode) {
        g_applied_theme_mode = next;
        copy_theme_tables(next);
        return true;
    }
    return false;
}

int gui_ui_scale_pct(void)
{
    return resolve_ui_scale_pct();
}

int gui_scaled_metric(int base_px)
{
    return scaled_metric_floor(base_px);
}

int gui_space_1(void)
{
    return scaled_metric_floor(GUI_SPACE_1);
}
int gui_space_1_5(void)
{
    return scaled_metric_floor(GUI_SPACE_1_5);
}
int gui_space_2(void)
{
    return scaled_metric_floor(GUI_SPACE_2);
}
int gui_space_3(void)
{
    return scaled_metric_floor(GUI_SPACE_3);
}
int gui_space_4(void)
{
    return scaled_metric_floor(GUI_SPACE_4);
}
int gui_card_header_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_title()) + gui_scaled_metric(12), scaled_metric_floor(28),
                        scaled_metric_floor(34));
}
int gui_badge_h(void)
{
    return gui_font_line_height(gui_font_default()) + gui_scaled_metric(4);
}
int gui_badge_pad_x(void)
{
    return scaled_metric_floor(GUI_BADGE_PAD_X);
}
int gui_window_title_min_x(void)
{
    return scaled_metric_floor(GUI_WINDOW_TITLE_MIN_X);
}
int gui_app_outer_padding(void)
{
    return scaled_metric_floor(GUI_APP_OUTER_PADDING);
}
int gui_app_section_gap(void)
{
    return scaled_metric_floor(GUI_APP_SECTION_GAP);
}
int gui_app_header_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_title()) + gui_font_line_height(gui_font_default()) +
                            gui_scaled_metric(24),
                        scaled_metric_floor(GUI_APP_HEADER_H), scaled_metric_floor(76));
}
int gui_app_row_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(15), scaled_metric_floor(32),
                        scaled_metric_floor(40));
}
int gui_app_control_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(10), scaled_metric_floor(24),
                        scaled_metric_floor(30));
}
int gui_title_bar_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_title()) + gui_scaled_metric(12), scaled_metric_floor(30),
                        scaled_metric_floor(36));
}
int gui_menubar_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(16), scaled_metric_floor(28),
                        scaled_metric_floor(34));
}
int gui_system_menubar_canvas_h(void)
{
    int menu_item_h = scaled_metric_floor(k_system_menu_item_h_px);
    int menu_gap = scaled_metric_floor(k_system_menu_gap_px);
    int menu_extra_h = scaled_metric_floor(k_system_menu_extra_h_px);
    int total_h = gui_menubar_h() + menu_gap + (menu_item_h * k_system_menu_item_count) + menu_extra_h;
    return total_h;
}

static void gui_draw_accent_strip(Surface *s, int x, int y, int w, int h, uint32_t color)
{
    if (!s || w <= 0 || h <= 0)
        return;
    gui_fill_rounded_rect(s, x, y, w, h, gui_corner_radius(w, h, w / 2), color);
}

GuiAppLayout gui_app_begin(Surface *s)
{
    GuiAppLayout layout = {};
    if (!s)
        return layout;

    int view_w = (int)s->width;
    int view_h = (int)s->height;
    if (g_my_window) {
        if (g_my_window->w > 0)
            view_w = g_my_window->w;
        if (g_my_window->h > 0)
            view_h = g_my_window->h;
    }

    const int outer_padding = gui_app_outer_padding();
    const int header_h = gui_app_header_h();
    const int section_gap = gui_app_section_gap();
    const int bottom_padding = outer_padding;
    gui_fill_surface(s, g_gui_style.app_bg);
    layout.outer_x = outer_padding;
    layout.outer_y = outer_padding;
    layout.outer_w = view_w - outer_padding * 2;
    layout.outer_h = view_h - outer_padding - bottom_padding;
    if (layout.outer_w < 0)
        layout.outer_w = 0;
    if (layout.outer_h < 0)
        layout.outer_h = 0;

    layout.header_rect = gui_rect_make(layout.outer_x, layout.outer_y, layout.outer_w, header_h);
    int body_y = layout.outer_y + header_h + section_gap;
    int body_h = layout.outer_h - header_h - section_gap;
    if (body_h < 0)
        body_h = 0;
    layout.body_rect = gui_rect_make(layout.outer_x, body_y, layout.outer_w, body_h);
    return layout;
}

void gui_app_draw_header(Surface *s, const GuiAppLayout *layout, const char *title, const char *subtitle,
                         const char *detail)
{
    if (!s || !layout)
        return;
    const int pad_x = gui_space_2();
    const int right_pad = gui_space_2();
    Rect r = layout->header_rect;
    if (g_my_window)
        r.y += g_my_window->scroll_y;

    gui_fill_rounded_rect(s, r.x, r.y + 1, r.w, r.h, gui_panel_radius(r.w, r.h), 0x10000000u);
    gui_draw_panel_inset(s, r.x, r.y, r.w, r.h, g_gui_style.app_surface_alt, g_gui_style.border,
                         g_gui_style.app_surface);

    int text_x = r.x + pad_x;
    int title_h = gui_font_line_height(gui_font_title());
    int subtitle_h = (subtitle && *subtitle) ? gui_font_line_height(gui_font_default()) : 0;
    int block_h = title_h + (subtitle_h > 0 ? gui_scaled_metric(3) + subtitle_h : 0);
    int title_y = r.y + (r.h - block_h) / 2;
    int subtitle_y = title_y + title_h + gui_scaled_metric(3);

    int detail_reserved_w = detail && *detail ? gui_scaled_metric(170) : 0;
    int title_max_w = r.w - pad_x - right_pad - detail_reserved_w;
    if (title_max_w < gui_scaled_metric(96))
        title_max_w = r.w - pad_x - right_pad;

    if (title) {
        gui_draw_text_clipped(s, gui_font_title(), text_x, title_y, title_max_w, title, g_gui_style.text, 0);
    }
    if (subtitle && *subtitle) {
        gui_draw_text_clipped(s, gui_font_default(), text_x, subtitle_y, title_max_w, subtitle, g_gui_style.text_dim,
                              0);
    }
    if (detail && *detail) {
        int detail_w = gui_measure_text(gui_font_default(), detail);
        int detail_x = r.x + r.w - right_pad - detail_w;
        if (detail_x < text_x + gui_scaled_metric(120))
            detail_x = text_x + gui_scaled_metric(120);
        int detail_y = gui_align_text_y(gui_font_default(), r.y, r.h);
        gui_draw_text_clipped(s, gui_font_default(), detail_x, detail_y, r.x + r.w - right_pad - detail_x, detail,
                              g_gui_style.text_muted, 0);
    }
}

void gui_app_draw_nav_item(Surface *s, int x, int y, int w, int h, const char *label, const char *detail, bool active,
                           bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int pad_x = gui_space_2();
    uint32_t bg = active ? g_gui_style.chrome_bg : (hovered ? g_gui_style.app_surface_alt : g_gui_style.app_surface);
    if (active || hovered) {
        gui_fill_rounded_rect(s, x, y, w, h, gui_radius_md(), bg);
        if (active)
            gui_draw_rounded_rect(s, x, y, w, h, gui_radius_md(), g_gui_style.border_hover);
    }
    if (active) {
        int strip_w = gui_scaled_metric(2);
        int strip_inset = gui_scaled_metric(9);
        gui_draw_accent_strip(s, x + gui_scaled_metric(5), y + strip_inset, strip_w, h - strip_inset * 2,
                              g_gui_style.border_focus);
    }
    int title_h = gui_font_line_height(gui_font_title());
    int detail_h = (detail && *detail) ? gui_line_height() : 0;
    int block_h = title_h + (detail_h > 0 ? (gui_scaled_metric(2) + detail_h) : 0);
    int title_y = y + (h - block_h) / 2;
    int text_x = x + pad_x + (active ? gui_scaled_metric(3) : 0);
    gui_draw_text_clipped(s, gui_font_title(), text_x, title_y, w - (text_x - x) - pad_x, label ? label : "",
                          active ? g_gui_style.text : g_gui_style.text_dim, bg);
    if (detail && *detail) {
        gui_draw_text_clipped(s, gui_font_default(), text_x, title_y + title_h + gui_scaled_metric(2),
                              w - (text_x - x) - pad_x, detail, g_gui_style.text_muted, bg);
    }
}

void gui_app_draw_list_row(Surface *s, int x, int y, int w, int h, const char *badge, const char *title,
                           const char *detail, bool active, bool hovered, bool muted)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    uint32_t bg =
        active ? g_gui_style.chrome_bg_alt : (hovered ? g_gui_style.app_surface_alt : g_gui_style.app_surface);
    gui_fill_rounded_rect(s, x, y, w, h, gui_radius_md(), bg);
    gui_draw_rounded_rect(s, x, y, w, h, gui_radius_md(), active ? g_gui_style.border_focus : g_gui_style.border);

    int text_x = x + space_2;
    if (badge && *badge) {
        uint32_t badge_bg = muted ? g_gui_style.chrome_bg : g_gui_style.accent_soft;
        gui_draw_badge(s, x + space_2, y + (h - gui_badge_h()) / 2, badge, badge_bg,
                       muted ? g_gui_style.text_muted : g_gui_chrome.badge_text);
        text_x += gui_measure_text(gui_font_default(), badge) + gui_badge_pad_x() * 2 + space_2;
    }

    int text_w = w - (text_x - x) - space_2;
    if (detail && *detail) {
        int detail_w = gui_measure_text(gui_font_default(), detail);
        int detail_x = x + w - space_2 - detail_w;
        if (detail_x > text_x + gui_scaled_metric(64)) {
            int detail_y = gui_align_text_y(gui_font_default(), y, h);
            gui_draw_text_clipped(s, gui_font_default(), detail_x, detail_y, w - (detail_x - x) - space_2, detail,
                                  g_gui_style.text_muted, bg);
            text_w = detail_x - text_x - space_1;
        }
    }

    int title_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), text_x, title_y, text_w, title ? title : "",
                          muted ? g_gui_style.text_dim : g_gui_style.text, bg);
}

void gui_app_draw_toggle_row(Surface *s, int x, int y, int w, int h, const char *label, const char *detail, bool on,
                             bool active, bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_2 = gui_space_2();
    uint32_t bg =
        active ? g_gui_style.chrome_bg_alt : (hovered ? g_gui_style.app_surface_alt : g_gui_style.app_surface);
    gui_fill_rounded_rect(s, x, y, w, h, gui_radius_md(), bg);
    gui_draw_rounded_rect(s, x, y, w, h, gui_radius_md(), active ? g_gui_style.border_focus : g_gui_style.border);

    int switch_w = scaled_metric_floor(36);
    int switch_h = scaled_metric_floor(20);
    int text_limit = w - (switch_w + space_2 * 2 + scaled_metric_floor(18));
    if (text_limit < scaled_metric_floor(84))
        text_limit = scaled_metric_floor(84);
    int text_block_h = gui_line_height() + ((detail && *detail) ? (gui_scaled_metric(2) + gui_line_height()) : 0);
    int label_y = y + (h - text_block_h) / 2;
    gui_draw_text_clipped(s, gui_font_default(), x + space_2, label_y, text_limit, label ? label : "", g_gui_style.text,
                          bg);
    if (detail && *detail) {
        gui_draw_text_clipped(s, gui_font_default(), x + space_2, label_y + gui_line_height() + gui_scaled_metric(2),
                              text_limit, detail, g_gui_style.text_muted, bg);
    }

    int switch_x = x + w - space_2 - switch_w;
    int switch_y = y + (h - switch_h) / 2;
    int switch_r = switch_h / 2;
    uint32_t track_bg = on ? g_gui_style.accent : g_gui_style.chrome_bg_alt;
    uint32_t track_border = on ? g_gui_style.border_focus : (hovered ? g_gui_style.border_hover : g_gui_style.border);
    gui_fill_rounded_rect(s, switch_x, switch_y, switch_w, switch_h, switch_r, track_bg);
    gui_draw_rounded_rect(s, switch_x, switch_y, switch_w, switch_h, switch_r, track_border);

    int knob_d = switch_h - gui_scaled_metric(6);
    if (knob_d < gui_scaled_metric(12))
        knob_d = gui_scaled_metric(12);
    if (knob_d > switch_h - gui_scaled_metric(4))
        knob_d = switch_h - gui_scaled_metric(4);
    int knob_y = switch_y + (switch_h - knob_d) / 2;
    int knob_x = on ? (switch_x + switch_w - knob_d - gui_scaled_metric(3)) : (switch_x + gui_scaled_metric(3));
    uint32_t knob_bg = on ? 0xFFFFFFFFu : g_gui_style.text_dim;
    uint32_t knob_shadow = on ? 0x28000000u : g_gui_style.chrome_edge;
    gui_fill_rounded_rect(s, knob_x, knob_y + 1, knob_d, knob_d, knob_d / 2, knob_shadow);
    gui_fill_rounded_rect(s, knob_x, knob_y, knob_d, knob_d, knob_d / 2, knob_bg);
}

void gui_app_draw_segmented_choice(Surface *s, int x, int y, int w, int h, const char *const *labels, int count,
                                   int selected, int hovered_index)
{
    if (!s || !labels || count <= 0 || w <= 0 || h <= 0)
        return;
    const int pad = gui_scaled_metric(2);
    gui_fill_rounded_rect(s, x, y, w, h, gui_corner_radius(w, h, h / 2), g_gui_style.app_surface_alt);
    gui_draw_rounded_rect(s, x, y, w, h, gui_corner_radius(w, h, h / 2), g_gui_style.border);

    int seg_w = w / count;
    int pill_y = y + pad;
    int pill_h = h - pad * 2;

    for (int i = 0; i < count; i++) {
        int seg_x = x + i * seg_w;
        int actual_w = (i == count - 1) ? (x + w - seg_x) : seg_w;
        bool active = i == selected;
        bool hovered = i == hovered_index;
        uint32_t text_bg = g_gui_style.app_surface_alt;

        if (active || hovered) {
            int pill_x = seg_x + pad;
            int pill_w = actual_w - pad * 2;
            if (pill_w > 0 && pill_h > 0) {
                uint32_t fill = active ? g_gui_style.chrome_bg_alt : g_gui_style.chrome_bg;
                int pill_r = gui_corner_radius(pill_w, pill_h, pill_h / 2);
                gui_fill_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, pill_r, fill);
                if (active)
                    gui_draw_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, pill_r, g_gui_style.border_hover);
                text_bg = fill;
            }
        } else if (i > 0) {
            int separator_x = seg_x;
            gui_fill_rect(s, separator_x, y + gui_scaled_metric(5), 1, h - gui_scaled_metric(10), g_gui_style.border);
        }

        int text_y = gui_align_text_y(gui_font_default(), y, h);
        gui_draw_text_clipped(s, gui_font_default(), seg_x + gui_space_1(), text_y, actual_w - gui_space_2(), labels[i],
                              active ? g_gui_style.text : g_gui_style.text_dim, text_bg);
    }
}

void gui_app_draw_text_field(Surface *s, int x, int y, int w, int h, const char *value, bool focused, bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    uint32_t bg = focused ? g_gui_style.app_surface : g_gui_style.app_surface_alt;
    uint32_t border = focused ? g_gui_style.border_hover : (hovered ? g_gui_style.border_hover : g_gui_style.border);
    int r = gui_corner_radius(w, h, gui_radius_md());

    // Shadow to match button height/weight
    gui_fill_rounded_rect(s, x, y + 1, w, h, r, 0x08000000u);

    gui_fill_rounded_rect(s, x, y, w, h, r, bg);
    gui_draw_rounded_rect(s, x, y, w, h, r, border);
    if (focused && w > 4 && h > 4)
        gui_draw_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, gui_corner_radius(w - 2, h - 2, r - 1),
                              g_gui_style.chrome_bg);
    const char *text = value ? value : "";
    int text_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), x + space_1, text_y, w - space_2, text, g_gui_style.text, bg);
    if (focused) {
        int text_w = gui_measure_text(gui_font_default(), text);
        int max_text_w = w - space_2 - space_1;
        if (max_text_w < 0)
            max_text_w = 0;
        if (text_w > max_text_w)
            text_w = max_text_w;
        int caret_w = gui_scaled_metric(1);
        if (caret_w < 1)
            caret_w = 1;
        int caret_h = h - space_1 * 2;
        if (caret_h < 4)
            caret_h = 4;
        gui_fill_rect(s, x + space_1 + text_w + 1, y + (h - caret_h) / 2, caret_w, caret_h, g_gui_style.accent);
    }
}

void gui_app_draw_button(Surface *s, int x, int y, int w, int h, const char *label, bool primary, bool focused,
                         bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    uint32_t bg = primary ? g_gui_style.accent : g_gui_style.chrome_bg_alt;
    if (!primary && hovered)
        bg = g_gui_style.app_surface_alt;
    if (primary && hovered)
        bg = g_gui_style.border_hover;
    uint32_t border = focused ? g_gui_style.border_hover : (hovered ? g_gui_style.border_hover : g_gui_style.border);
    int r = gui_corner_radius(w, h, gui_radius_md());

    // Shadow (only if not maximized/fullscreen logic, but here simple)
    gui_fill_rounded_rect(s, x, y + 1, w, h, r, primary ? 0x18000000u : 0x10000000u);

    // Main surface
    gui_fill_rounded_rect(s, x, y, w, h, r, bg);

    // Outer border
    gui_draw_rounded_rect(s, x, y, w, h, r, border);

    // Inner highlight / polish (Unified logic)
    if (w > 4 && h > 4) {
        int ir = r > 0 ? r - 1 : 0;
        // Highlight / inner shadow
        uint32_t highlight = primary ? 0x20FFFFFFu : g_gui_style.chrome_bg;
        gui_draw_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, ir, highlight);
    }
    int text_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), x + space_1, text_y, w - space_2, label ? label : "",
                          primary ? COLOR_WHITE : g_gui_style.text, bg);
}

int gui_popup_menu_item_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(10), scaled_metric_floor(24),
                        scaled_metric_floor(32));
}

int gui_popup_menu_height(const GuiMenuItem *items, int count)
{
    if (!items || count <= 0)
        return 0;
    int total_h = popup_menu_outer_pad_y() * 2;
    int item_h = gui_popup_menu_item_h();
    int gap = popup_menu_row_gap();
    for (int i = 0; i < count; i++) {
        total_h += items[i].separator ? popup_menu_separator_h() : item_h;
        if (i + 1 < count)
            total_h += gap;
    }
    return total_h;
}

int gui_popup_menu_width(const GuiMenuItem *items, int count, int min_width)
{
    if (!items || count <= 0)
        return 0;
    int base_width = popup_menu_inner_width(items, count, min_width);
    return base_width;
}

int gui_popup_menu_hit_test(const GuiMenuItem *items, int count, int x, int y, int w, int mx, int my)
{
    if (!items || count <= 0)
        return -1;
    int menu_h = gui_popup_menu_height(items, count);
    if (mx < x || mx >= x + w || my < y || my >= y + menu_h)
        return -1;

    int local_y = my - y;
    int item_h = gui_popup_menu_item_h();
    for (int i = 0; i < count; i++) {
        int item_y = popup_menu_item_y_offset(items, count, i);
        int block_h = items[i].separator ? popup_menu_separator_h() : item_h;
        if (local_y < item_y || local_y >= item_y + block_h)
            continue;
        if (items[i].separator || !items[i].enabled)
            return -1;
        return i;
    }
    return -1;
}

void gui_draw_popup_menu(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count, int hovered_index)
{
    if (!s || !items || count <= 0 || w <= 0)
        return;

    int item_h = gui_popup_menu_item_h();
    int menu_h = gui_popup_menu_height(items, count);
    int outer_pad_x = popup_menu_outer_pad_x();
    int row_pad_x = popup_menu_row_pad_x();
    int row_gap = popup_menu_row_gap();
    int separator_inset = popup_menu_separator_inset();
    int y_cursor = y + popup_menu_outer_pad_y();

    int radius = gui_corner_radius(w, menu_h, gui_radius_xl());

    gui_fill_rounded_rect(s, x, y + gui_scaled_metric(6), w, menu_h, radius, 0x08000000u);
    gui_fill_rounded_rect(s, x, y + gui_scaled_metric(3), w, menu_h, radius, 0x0C000000u);
    gui_fill_rounded_rect(s, x, y + gui_scaled_metric(1), w, menu_h, radius, 0x10000000u);

    gui_draw_panel_inset_ext(s, x, y, w, menu_h, radius, g_gui_style.app_surface, g_gui_style.border,
                             g_gui_style.chrome_bg_alt);

    for (int i = 0; i < count; i++) {
        bool hovered = i == hovered_index && !items[i].separator && items[i].enabled;
        uint32_t row_bg = hovered ? g_gui_style.chrome_bg_alt : g_gui_style.app_surface;
        uint32_t fg = items[i].enabled ? g_gui_style.text : g_gui_style.text_muted;
        if (items[i].separator) {
            int separator_y = y_cursor + popup_menu_separator_h() / 2;
            gui_fill_rect(s, x + separator_inset, separator_y, w - separator_inset * 2, 1, g_gui_style.chrome_edge);
        } else {
            if (hovered) {
                int row_x = x + outer_pad_x;
                int row_w = w - outer_pad_x * 2;
                int row_r = radius - outer_pad_x;
                if (row_r < gui_radius_sm())
                    row_r = gui_radius_sm();
                gui_fill_rounded_rect(s, row_x, y_cursor, row_w, item_h, row_r, row_bg);
            }
            int text_y = gui_align_text_y(gui_font_default(), y_cursor, item_h);
            int text_x = x + outer_pad_x + row_pad_x;
            int text_w = w - (outer_pad_x + row_pad_x) * 2;
            gui_draw_text_clipped(s, gui_font_default(), text_x, text_y, text_w, items[i].label ? items[i].label : "",
                                  fg, row_bg);
        }
        y_cursor += items[i].separator ? popup_menu_separator_h() : item_h;
        if (i + 1 < count)
            y_cursor += row_gap;
    }
}

struct CursorAssetDescriptor
{
    const char *dir_name;
    uint16_t role;
    int32_t hotspot_x_nominal;
    int32_t hotspot_y_nominal;
    int32_t nominal_size;
};

struct CursorSurfaceCacheEntry
{
    Surface surface;
    int32_t hotspot_x;
    int32_t hotspot_y;
    uint32_t frame_duration_ms;
    bool has_hotspot;
    bool attempted;
};

static constexpr int k_gui_cursor_kind_count = GUI_CURSOR_RESIZE_D2 + 1;
static constexpr int k_cursor_asset_sizes[] = {16, 20, 24, 32, 40, 48, 64};
static constexpr int k_cursor_asset_size_count = (int)(sizeof(k_cursor_asset_sizes) / sizeof(k_cursor_asset_sizes[0]));

static CursorSurfaceCacheEntry g_cursor_asset_cache[k_gui_cursor_kind_count][k_cursor_asset_size_count] = {};

static const CursorAssetDescriptor k_cursor_asset_descriptors[k_gui_cursor_kind_count] = {
    {"default", GUI_UOCU_ROLE_ARROW, 7, 4, 24},
    {"move", GUI_UOCU_ROLE_MOVE, 7, 4, 24},
    {"ew-resize", GUI_UOCU_ROLE_RESIZE_EW, 12, 12, 24},
    {"ns-resize", GUI_UOCU_ROLE_RESIZE_NS, 12, 11, 24},
    {"nwse-resize", GUI_UOCU_ROLE_RESIZE_NWSE, 12, 11, 24},
    {"nesw-resize", GUI_UOCU_ROLE_RESIZE_NESW, 12, 11, 24},
};

static const CursorAssetDescriptor *cursor_descriptor_for_kind(GuiCursorKind kind)
{
    int index = (int)kind;
    if (index < 0 || index >= k_gui_cursor_kind_count)
        return nullptr;
    return &k_cursor_asset_descriptors[index];
}

static int cursor_pick_asset_size_index(const CursorAssetDescriptor *descriptor)
{
    if (!descriptor)
        return -1;

    int target_size = (descriptor->nominal_size * gui_ui_scale_pct() + 50) / 100;
    int best_index = 0;
    int best_distance = target_size - k_cursor_asset_sizes[0];
    if (best_distance < 0)
        best_distance = -best_distance;

    for (int i = 1; i < k_cursor_asset_size_count; i++) {
        int distance = target_size - k_cursor_asset_sizes[i];
        if (distance < 0)
            distance = -distance;
        if (distance < best_distance) {
            best_distance = distance;
            best_index = i;
        }
    }
    return best_index;
}

static int32_t scale_cursor_hotspot(int32_t hotspot_nominal, int32_t nominal_size, int32_t asset_size)
{
    if (nominal_size <= 0)
        return hotspot_nominal;
    return (hotspot_nominal * asset_size + nominal_size / 2) / nominal_size;
}

static bool cursor_metrics_for_kind(GuiCursorKind kind, int32_t *hot_x, int32_t *hot_y, int32_t *width, int32_t *height)
{
    const CursorAssetDescriptor *descriptor = cursor_descriptor_for_kind(kind);
    if (!descriptor)
        return false;

    int index = cursor_pick_asset_size_index(descriptor);
    if (index < 0)
        return false;

    int32_t asset_w = k_cursor_asset_sizes[index];
    int32_t asset_h = asset_w;
    CursorSurfaceCacheEntry &entry = g_cursor_asset_cache[(int)kind][index];
    if (entry.attempted && entry.surface.buffer) {
        asset_w = (int32_t)entry.surface.width;
        asset_h = (int32_t)entry.surface.height;
    }

    if (hot_x)
        *hot_x = entry.has_hotspot
                     ? entry.hotspot_x
                     : scale_cursor_hotspot(descriptor->hotspot_x_nominal, descriptor->nominal_size, asset_w);
    if (hot_y)
        *hot_y = entry.has_hotspot
                     ? entry.hotspot_y
                     : scale_cursor_hotspot(descriptor->hotspot_y_nominal, descriptor->nominal_size, asset_h);
    if (width)
        *width = asset_w;
    if (height)
        *height = asset_h;
    return true;
}

static const Surface *cursor_surface_for_kind(GuiCursorKind kind, int32_t *hot_x, int32_t *hot_y)
{
    const CursorAssetDescriptor *descriptor = cursor_descriptor_for_kind(kind);
    if (!descriptor)
        return nullptr;

    int index = cursor_pick_asset_size_index(descriptor);
    if (index < 0)
        return nullptr;

    CursorSurfaceCacheEntry &entry = g_cursor_asset_cache[(int)kind][index];
    if (!entry.attempted) {
        char path[192];
        snprintf(path, sizeof(path), "/usr/share/cursors/%s.uocu", descriptor->dir_name);

        Surface loaded = {};
        uint16_t hotspot_x = 0;
        uint16_t hotspot_y = 0;
        uint32_t frame_duration_ms = 0;
        if (gui_load_uocu(path, descriptor->role, (uint32_t)descriptor->nominal_size, (uint32_t)gui_ui_scale_pct(),
                          GUI_UOCU_VARIANT_DEFAULT, &loaded, &hotspot_x, &hotspot_y, &frame_duration_ms)) {
            entry.surface = loaded;
            entry.hotspot_x = hotspot_x;
            entry.hotspot_y = hotspot_y;
            entry.frame_duration_ms = frame_duration_ms;
            entry.has_hotspot = true;
        }
        entry.attempted = true;
    }

    if (!entry.surface.buffer)
        return nullptr;

    if (hot_x)
        *hot_x = entry.has_hotspot ? entry.hotspot_x
                                   : scale_cursor_hotspot(descriptor->hotspot_x_nominal, descriptor->nominal_size,
                                                          (int32_t)entry.surface.width);
    if (hot_y)
        *hot_y = entry.has_hotspot ? entry.hotspot_y
                                   : scale_cursor_hotspot(descriptor->hotspot_y_nominal, descriptor->nominal_size,
                                                          (int32_t)entry.surface.height);
    return &entry.surface;
}

static void cursor_hotspot_for_kind(GuiCursorKind kind, int32_t *hot_x, int32_t *hot_y)
{
    int32_t x = 0;
    int32_t y = 0;
    if (!cursor_metrics_for_kind(kind, &x, &y, nullptr, nullptr)) {
        switch (kind) {
            case GUI_CURSOR_MOVE:
            case GUI_CURSOR_RESIZE_H:
            case GUI_CURSOR_RESIZE_V:
            case GUI_CURSOR_RESIZE_D1:
            case GUI_CURSOR_RESIZE_D2:
                x = 8;
                y = 8;
                break;
            case GUI_CURSOR_ARROW:
            default:
                break;
        }
    }

    if (hot_x)
        *hot_x = x;
    if (hot_y)
        *hot_y = y;
}

static void draw_cursor_bitmap(Surface *s, int32_t x, int32_t y, const uint8_t data[16][16])
{
    if (!s || !s->buffer)
        return;
    const int32_t cursor_w = 16;
    const int32_t cursor_h = 16;
    if (x >= (int32_t)s->width || y >= (int32_t)s->height || x + cursor_w <= 0 || y + cursor_h <= 0)
        return;

    int32_t start_col = (x < 0) ? -x : 0;
    int32_t start_row = (y < 0) ? -y : 0;
    int32_t end_col = (x + cursor_w > (int32_t)s->width) ? ((int32_t)s->width - x) : cursor_w;
    int32_t end_row = (y + cursor_h > (int32_t)s->height) ? ((int32_t)s->height - y) : cursor_h;
    uint32_t stride = s->pitch / 4;

    for (int32_t row = start_row; row < end_row; row++) {
        uint32_t *dst = &s->buffer[(y + row) * stride + x + start_col];
        for (int32_t col = start_col; col < end_col; col++) {
            uint8_t pixel = data[row][col];
            if (pixel == 1) {
                *dst = 0xFFFFFFFF;
            } else if (pixel == 2) {
                *dst = 0xFF010101;
            }
            dst++;
        }
    }
}

static inline uint32_t blend_premultiplied_over_opaque(uint32_t dst, uint32_t src)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0)
        return dst;
    if (alpha == 255)
        return 0xFF000000u | (src & 0x00FFFFFFu);

    uint32_t inv = 255u - alpha;
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8) & 0xFFu;
    uint32_t db = dst & 0xFFu;
    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >> 8) & 0xFFu;
    uint32_t sb = src & 0xFFu;

    uint32_t r = sr + (dr * inv + 127u) / 255u;
    uint32_t g = sg + (dg * inv + 127u) / 255u;
    uint32_t b = sb + (db * inv + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void draw_cursor_surface(Surface *s, int32_t x, int32_t y, const Surface *cursor)
{
    if (!s || !s->buffer || !cursor || !cursor->buffer)
        return;

    int32_t cursor_w = (int32_t)cursor->width;
    int32_t cursor_h = (int32_t)cursor->height;
    if (x >= (int32_t)s->width || y >= (int32_t)s->height || x + cursor_w <= 0 || y + cursor_h <= 0)
        return;

    int32_t start_col = (x < 0) ? -x : 0;
    int32_t start_row = (y < 0) ? -y : 0;
    int32_t end_col = (x + cursor_w > (int32_t)s->width) ? ((int32_t)s->width - x) : cursor_w;
    int32_t end_row = (y + cursor_h > (int32_t)s->height) ? ((int32_t)s->height - y) : cursor_h;
    uint32_t dst_stride = s->pitch / 4;
    uint32_t src_stride = cursor->pitch / 4;

    for (int32_t row = start_row; row < end_row; row++) {
        uint32_t *dst = &s->buffer[(y + row) * dst_stride + x + start_col];
        const uint32_t *src = &cursor->buffer[(size_t)row * src_stride + start_col];
        for (int32_t col = start_col; col < end_col; col++) {
            uint32_t pixel = *src++;
            uint32_t alpha = pixel >> 24;
            if (alpha == 0) {
                dst++;
                continue;
            }
            *dst = blend_premultiplied_over_opaque(*dst, pixel);
            dst++;
        }
    }
}

void gui_draw_cursor_kind(Surface *s, int32_t x, int32_t y, GuiCursorKind kind)
{
    int32_t asset_hot_x = 0;
    int32_t asset_hot_y = 0;
    const Surface *asset_surface = cursor_surface_for_kind(kind, &asset_hot_x, &asset_hot_y);
    if (asset_surface) {
        draw_cursor_surface(s, x - asset_hot_x, y - asset_hot_y, asset_surface);
        return;
    }

    static const uint8_t arrow_data[16][16] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {1, 2, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}};
    static const uint8_t move_data[16][16] = {
        {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0},
        {0, 1, 2, 1, 0, 0, 0, 2, 0, 0, 0, 1, 2, 1, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0},
        {0, 1, 2, 1, 0, 0, 0, 2, 0, 0, 0, 1, 2, 1, 0, 0}, {0, 0, 1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    static const uint8_t resize_h_data[16][16] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1}, {1, 1, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 0, 1, 1},
        {1, 2, 1, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 1, 2, 1}, {1, 2, 2, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 2, 2, 1},
        {1, 2, 2, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 2, 2, 1}, {1, 2, 1, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 1, 2, 1},
        {1, 1, 0, 0, 0, 1, 2, 2, 2, 1, 0, 0, 0, 0, 1, 1}, {1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    static const uint8_t resize_v_data[16][16] = {
        {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 1, 1, 0, 0, 0, 0},
        {0, 0, 1, 2, 1, 2, 2, 2, 2, 1, 2, 1, 0, 0, 0, 0}, {0, 1, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 1, 0, 0, 0},
        {0, 0, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0},
        {0, 1, 2, 2, 1, 2, 2, 2, 2, 1, 2, 2, 1, 0, 0, 0}, {0, 0, 1, 2, 1, 2, 2, 2, 2, 1, 2, 1, 0, 0, 0, 0},
        {0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 1, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}};
    static const uint8_t resize_d1_data[16][16] = {
        {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1}};
    static const uint8_t resize_d2_data[16][16] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

    const uint8_t(*cursor_data)[16] = arrow_data;
    switch (kind) {
        case GUI_CURSOR_MOVE:
            cursor_data = move_data;
            break;
        case GUI_CURSOR_RESIZE_H:
            cursor_data = resize_h_data;
            break;
        case GUI_CURSOR_RESIZE_V:
            cursor_data = resize_v_data;
            break;
        case GUI_CURSOR_RESIZE_D1:
            cursor_data = resize_d1_data;
            break;
        case GUI_CURSOR_RESIZE_D2:
            cursor_data = resize_d2_data;
            break;
        case GUI_CURSOR_ARROW:
        default:
            cursor_data = arrow_data;
            break;
    }
    int32_t hot_x = 0;
    int32_t hot_y = 0;
    cursor_hotspot_for_kind(kind, &hot_x, &hot_y);
    draw_cursor_bitmap(s, x - hot_x, y - hot_y, cursor_data);
}

void gui_draw_cursor(Surface *s, int32_t x, int32_t y)
{
    gui_draw_cursor_kind(s, x, y, GUI_CURSOR_ARROW);
}

void gui_get_cursor_hotspot(GuiCursorKind kind, int32_t *hot_x, int32_t *hot_y)
{
    cursor_hotspot_for_kind(kind, hot_x, hot_y);
}

void gui_get_cursor_bounds(GuiCursorKind kind, int32_t x, int32_t y, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh)
{
    int32_t hot_x = 0;
    int32_t hot_y = 0;
    int32_t width = 16;
    int32_t height = 16;
    if (!cursor_metrics_for_kind(kind, &hot_x, &hot_y, &width, &height))
        cursor_hotspot_for_kind(kind, &hot_x, &hot_y);
    if (bx)
        *bx = x - hot_x;
    if (by)
        *by = y - hot_y;
    if (bw)
        *bw = width;
    if (bh)
        *bh = height;
}

bool gui_intersect_rect(int x1, int y1, int w1, int h1, int x2, int y2, int w2, int h2, int *ox, int *oy, int *ow,
                        int *oh)
{
    if (w1 <= 0 || h1 <= 0 || w2 <= 0 || h2 <= 0)
        return false;

    int64_t left = (x1 > x2) ? x1 : x2;
    int64_t top = (y1 > y2) ? y1 : y2;
    int64_t right_1 = (int64_t)x1 + (int64_t)w1;
    int64_t right_2 = (int64_t)x2 + (int64_t)w2;
    int64_t bottom_1 = (int64_t)y1 + (int64_t)h1;
    int64_t bottom_2 = (int64_t)y2 + (int64_t)h2;
    int64_t right = (right_1 < right_2) ? right_1 : right_2;
    int64_t bottom = (bottom_1 < bottom_2) ? bottom_1 : bottom_2;

    if (right <= left || bottom <= top)
        return false;

    if (ox)
        *ox = (int)left;
    if (oy)
        *oy = (int)top;
    if (ow)
        *ow = (int)(right - left);
    if (oh)
        *oh = (int)(bottom - top);
    return true;
}

} // extern "C"
