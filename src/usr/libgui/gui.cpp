#include "gui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <uapi/syscalls.h>
#include <unistd.h>

#include "../libc/log.h"
#include "../libc/syscall.h"
#include "font_internal.h"
#include "gui_canvas_utils.h"
#include "gui_pixops.h"

static Registry *g_registry = NULL;
extern "C" {
WindowEntry *g_my_window = NULL;
}
static int g_window_shm_id = WIN_SHM_INVALID;
static uint32_t g_window_buffer_w = 0;
static uint32_t g_window_buffer_h = 0;
// Configure serial the client last synced its surface to. The commit acks
// exactly this serial instead of reading the live entry at commit time,
// which a fast drag may have advanced past the frame the client drew.
static uint32_t g_synced_resize_serial = 0;
static constexpr int GUI_RETIRED_WINDOW_BUFFER_SLOTS = 4;
struct RetiredWindowBuffer
{
    int shm_id;
    uint32_t generation;
};
static RetiredWindowBuffer g_retired_window_buffers[GUI_RETIRED_WINDOW_BUFFER_SLOTS] = {};
static int g_ui_scale_pct = 0;
static constexpr int k_system_menu_gap_px = 0; // dropdown attaches flush to the bar
static constexpr int k_system_menu_item_h_px = 24;
static constexpr int k_system_menu_item_count = 8;
// Worst-case dropdown: an app menu (MENU_MAX_ITEMS) plus a separator and the
// menubar-composed Window list. Drives the shared menubar canvas height.
static constexpr int k_menubar_canvas_max_rows = MENU_MAX_ITEMS + 1 + 8;

#include <drivers/video/font.h>

extern "C" {
GuiStylePalette g_gui_style = {};
GuiChromePalette g_gui_chrome = {};
}

// A 4-tier tinted-slate elevation system. Dark mode never uses pure black for
// content surfaces: the canvas (#1E1E22) lifts through a sidebar/header surface
// (#26262B) up to boxed-list cards (#2F2F36), giving physical depth without
// harsh high-contrast edges. The Adwaita blue (#3584E4) anchors only primary
// CTAs, toggle tracks and focus rings; sidebar selection is a neutral 12% white
// capsule, never solid blue. Field order matches gui.h — keep aligned.

static const GuiStylePalette k_gui_style_dark = {
    0xFF1E1E22, 0xFF26262B, 0xFF2F2F36, 0xFF26262B, 0xFF2F2F36, 0xFF37373C, 0xFF33333A, 0xFF3584E4, 0xFF3C3C44,
    0xFF3584E4, 0xFF2A3850, 0xFFFFFFFF, 0xFFDEDDDA, 0xFF9A9996, 0xFF30D158, 0xFFFF9F0A, 0xFFFF453A, 0x7A000000u};

static const GuiStylePalette k_gui_style_light = {
    0xFFF6F5F4, 0xFFFFFFFF, 0xFFF0EFEE, 0xFFF6F5F4, 0xFFECECEC, 0xFFE0DEDE, 0xFFE0E0E0, 0xFF3584E4, 0xFFD4D4D4,
    0xFF3584E4, 0xFFD9E7FA, 0xFF1C1C1E, 0xFF5E5E5E, 0xFF858589, 0xFF34C759, 0xFFFF9500, 0xFFFF3B30, 0x55000000u};

static const GuiChromePalette k_gui_chrome_dark = {0xFF141416, 0xFF33333A, 0xFFFF5F57,
                                                   0xFFFFBD2E, 0xFF28C840, 0xFF33333A};

static const GuiChromePalette k_gui_chrome_light = {0xFFECECEC, 0xFFE0E0E0, 0xFFFF5F57,
                                                    0xFFFFBD2E, 0xFF28C840, 0xFFE0E0E0};

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

        bool is_memfd = (retired.shm_id & 0x40000000) != 0;
        if (is_memfd) {
            // Memory is kept alive by Window Manager; locally it is already closed
            retired.shm_id = WIN_SHM_INVALID;
            retired.generation = 0;
        } else {
            syscall1(SYS_SHM_FREE, static_cast<uint64_t>(retired.shm_id));
            retired.shm_id = WIN_SHM_INVALID;
            retired.generation = 0;
        }
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

    int new_memfd = memfd_create("window_buffer_resize", 0);
    if (new_memfd < 0)
        return false;

    if (ftruncate(new_memfd, shm_bytes) < 0) {
        close(new_memfd);
        return false;
    }

    void *mapped = mmap(NULL, (size_t)shm_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, new_memfd, 0);
    if (mapped == MAP_FAILED) {
        close(new_memfd);
        return false;
    }

    int new_wm_fd = fd_transfer(0, new_memfd);
    if (new_wm_fd < 0) {
        munmap(mapped, (size_t)shm_bytes);
        close(new_memfd);
        return false;
    }

    close(new_memfd);
    int new_shm_id = new_wm_fd | 0x40000000;

    uint32_t *new_buffer = reinterpret_cast<uint32_t *>(mapped);
    const bool transparent = (g_my_window->flags & WIN_FLAG_TRANSPARENT) != 0;
    if (transparent) {
        memset(new_buffer, 0, static_cast<size_t>(shm_bytes));
    } else {
        const uint32_t fill = theme_tables_initialized() ? g_gui_style.app_bg : 0xFF000000u;
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
    uint32_t *old_buffer_addr = s->buffer;
    uint32_t old_buffer_w = g_window_buffer_w;
    uint32_t old_buffer_h = g_window_buffer_h;

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

    bool old_is_memfd = (old_shm_id & 0x40000000) != 0;
    if (old_is_memfd) {
        size_t old_size = (size_t)old_buffer_w * old_buffer_h * 4;
        munmap(old_buffer_addr, old_size);
    } else {
        syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(old_shm_id));
    }

    g_retired_window_buffers[retired_slot].shm_id = old_shm_id;
    g_retired_window_buffers[retired_slot].generation = generation;
    return true;
}

static void copy_theme_tables(GuiThemeMode mode)
{
    const GuiStylePalette *style = &k_gui_style_dark;
    const GuiChromePalette *chrome = &k_gui_chrome_dark;
    if (mode == GUI_THEME_LIGHT) {
        style = &k_gui_style_light;
        chrome = &k_gui_chrome_light;
    }

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
    Surface s = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t size = 0;
    if (!gui_surface_layout(width, height, &s.pitch, &size))
        return s;

    s.width = width;
    s.height = height;
    s.capacity_w = width;
    s.capacity_h = height;
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
    s->capacity_w = 0;
    s->capacity_h = 0;
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

    pix_fill_row(first_row, static_cast<uint32_t>(w), color);

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

static inline uint32_t blend_pixel(uint32_t dst, uint32_t src, uint8_t coverage)
{
    return gui_blend_pixel(dst, src, coverage);
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

void gui_fill_rect_blend(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!s || !s->buffer || w <= 0 || h <= 0)
        return;
    if (!gui_clip_rect_to_bounds(&x, &y, &w, &h, static_cast<int32_t>(s->width), static_cast<int32_t>(s->height)))
        return;

    uint8_t alpha = static_cast<uint8_t>((color >> 24) & 0xFFu);
    if (alpha == 255) {
        gui_fill_rect(s, x, y, w, h, color);
        return;
    }
    if (alpha == 0)
        return;

    const uint32_t stride = s->pitch / 4;
    for (int32_t row = y; row < y + h; row++) {
        uint32_t *dst = &s->buffer[static_cast<size_t>(row) * stride + x];
        for (int32_t col = 0; col < w; col++)
            dst[col] = blend_pixel(dst[col], color, 255);
    }
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
    uint32_t src_rb = color & 0x00FF00FFu;
    uint32_t src_ag = (color >> 8) & 0x00FF00FFu;
    uint32_t inv_a = 255u - base_alpha;

    for (int32_t py = 0; py < h; py++) {
        uint32_t *dst = &s->buffer[static_cast<size_t>(y + py) * pitch + x];
        for (int32_t px = 0; px < w; px++) {
            uint32_t d = dst[px];
            if ((d >> 24) != 255u) {
                // Translucent destination: full compositing keeps its alpha.
                dst[px] = blend_pixel(d, color, 255);
                continue;
            }
            // Opaque destination: branchless SWAR blend, result stays opaque.
            uint32_t d_rb = d & 0x00FF00FFu;
            uint32_t d_ag = (d >> 8) & 0x00FF00FFu;

            uint32_t rb = src_rb * base_alpha + d_rb * inv_a + 0x00800080u;
            rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
            rb &= 0x00FF00FFu;

            uint32_t ag = src_ag * base_alpha + d_ag * inv_a + 0x00800080u;
            ag = (ag + ((ag >> 8) & 0x00FF00FFu)) >> 8;
            ag &= 0x00FF00FFu;

            dst[px] = 0xFF000000u | ((ag << 8) & 0x0000FF00u) | rb;
        }
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
    float result;
    asm("sqrtss %1, %0" : "=x"(result) : "x"(n));
    return result;
}

// 256-level analytic coverage for a disk of radius r, evaluated from a
// pre-computed squared distance (pixel-center to disk-center). Early-out
// skips the sqrt for fully-interior and fully-exterior pixels.
static inline uint8_t disk_coverage_from_dist_sq(float dist_sq, float r)
{
    float r_in = r - 0.5f;
    float r_out = r + 0.5f;
    if (dist_sq <= r_in * r_in)
        return 255;
    if (dist_sq >= r_out * r_out)
        return 0;
    float dist = libgui_sqrt(dist_sq);
    float cov = r + 0.5f - dist;
    return static_cast<uint8_t>(cov * 255.0f + 0.5f);
}

static inline uint8_t disk_coverage(float dx, float dy, float r)
{
    return disk_coverage_from_dist_sq(dx * dx + dy * dy, r);
}
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

static uint8_t *build_rounded_corner_fill_mask(int radius)
{
    if (radius <= 0)
        return nullptr;
    size_t count = static_cast<size_t>(radius) * static_cast<size_t>(radius);
    uint8_t *mask = static_cast<uint8_t *>(malloc(count));
    if (!mask)
        return nullptr;

    float rf = static_cast<float>(radius);
    for (int row = 0; row < radius; row++) {
        for (int col = 0; col < radius; col++) {
            float dx = static_cast<float>(col) + 0.5f - rf;
            float dy = static_cast<float>(row) + 0.5f - rf;
            mask[static_cast<size_t>(row) * static_cast<size_t>(radius) + static_cast<size_t>(col)] =
                disk_coverage(dx, dy, rf);
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
        float cx_f = static_cast<float>((col < r) ? r : (w - r));
        float cy_f = static_cast<float>(top_band ? r : (h - r));
        float dx = static_cast<float>(col) + 0.5f - cx_f;
        float dy = static_cast<float>(row) + 0.5f - cy_f;
        return disk_coverage(dx, dy, static_cast<float>(r));
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
    float dx = static_cast<float>(px) + 0.5f - static_cast<float>(cx);
    float dy = static_cast<float>(py) + 0.5f - static_cast<float>(cy);
    return disk_coverage_from_dist_sq(dx * dx + dy * dy, static_cast<float>(r));
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
    float rf = static_cast<float>(r);
    float r_out_sq = (rf + 0.5f) * (rf + 0.5f);

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
        float dy = static_cast<float>(py) + 0.5f - static_cast<float>(y);
        float dy_sq = dy * dy;
        for (int32_t px = start_x; px < end_x; px++) {
            float dx = static_cast<float>(px) + 0.5f - static_cast<float>(x);
            float dist_sq = dx * dx + dy_sq;
            if (dist_sq >= r_out_sq)
                continue;
            uint8_t coverage = disk_coverage_from_dist_sq(dist_sq, rf);
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
    float rf = static_cast<float>(r);
    float inner_rf = static_cast<float>(inner_r);
    float r_out_sq = (rf + 0.5f) * (rf + 0.5f);
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
        float dy = static_cast<float>(py) + 0.5f - static_cast<float>(y);
        float dy_sq = dy * dy;
        for (int32_t px = start_x; px < end_x; px++) {
            float dx = static_cast<float>(px) + 0.5f - static_cast<float>(x);
            float dist_sq = dx * dx + dy_sq;
            if (dist_sq >= r_out_sq)
                continue;

            uint8_t outer = disk_coverage_from_dist_sq(dist_sq, rf);
            if (outer == 0)
                continue;

            uint8_t inner = (inner_r > 0) ? disk_coverage_from_dist_sq(dist_sq, inner_rf) : 0;
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

    if (!font) {
        size_t n = 0;
        while (n < len && str[n] && str[n] != '\n')
            n++;
        return (int)(n * 8u);
    }
    return (gui_text_advance26(font, str, len) + 32) >> 6;
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
    if (gui_measure_text_n(font, str, len) <= max_width) {
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
    if (font) {
        // Walk codepoints: the cut must land on a UTF-8 boundary.
        GuiTextWalk walk;
        gui_text_walk_init(&walk, font, str, len);
        uint32_t cp = 0;
        int32_t pos26 = 0;
        size_t consumed = 0;
        const GuiGlyph *glyph = nullptr;
        while ((glyph = gui_text_walk_next(&walk, &cp, &pos26, &consumed)) != nullptr) {
            if (((pos26 + glyph->advance_x26 + 32) >> 6) > target_width)
                break;
            clipped += consumed;
        }
    } else {
        int clipped_width = 0;
        while (clipped < len && str[clipped]) {
            if (clipped_width + 8 > target_width)
                break;
            clipped_width += 8;
            clipped++;
        }
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

    bool same_buffer = dest->buffer == src->buffer;
    bool overlap = same_buffer && !(dx + w <= sx || sx + w <= dx || dy + h <= sy || sy + h <= dy);

    int32_t start_y = 0, end_y = h, step_y = 1;
    if (overlap && dy > sy) {
        start_y = h - 1;
        end_y = -1;
        step_y = -1;
    }

    for (int32_t y = start_y; y != end_y; y += step_y) {
        int32_t start_x = 0, end_x = w, step_x = 1;
        if (overlap && dx > sx) {
            start_x = w - 1;
            end_x = -1;
            step_x = -1;
        }

        uint32_t *drow = &dest->buffer[static_cast<size_t>(dy + y) * dp + static_cast<size_t>(dx)];
        uint32_t *srow = &src->buffer[static_cast<size_t>(sy + y) * sp + static_cast<size_t>(sx)];

        if (!overlap) {
            pix_blend_row_premultiplied(drow, srow, static_cast<uint32_t>(w));
            continue;
        }

        for (int32_t x = start_x; x != end_x; x += step_x) {
            uint32_t pixel = srow[x];
            uint8_t alpha = static_cast<uint8_t>(pixel >> 24);
            if (alpha == 0)
                continue;
            if (alpha == 255)
                drow[x] = pixel;
            else {
                if ((drow[x] >> 24) == 255)
                    drow[x] = gui_blend_premultiplied_opaque_dst(drow[x], pixel);
                else
                    drow[x] = gui_blend_premultiplied(drow[x], pixel);
            }
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

    // Ack the configure this frame actually answered: the serial recorded
    // when the surface was synced, never the live entry (a fast drag may
    // have advanced it past the frame just drawn). The compositor looks the
    // serial up in its configure history and flips the window bounds to that
    // geometry — never ahead of what was drawn.
    if (g_synced_resize_serial != 0 && g_my_window->buffer_resize_serial != g_synced_resize_serial) {
        g_my_window->buffer_resize_serial = g_synced_resize_serial;
        g_synced_resize_serial = 0;
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
        return {NULL, 0, 0, 0, false};
    }

    uint32_t buffer_pitch = 0;
    size_t buffer_bytes = 0;
    if (!gui_surface_layout(w, h, &buffer_pitch, &buffer_bytes)) {
        LOG_ERROR("gui", "register_window invalid size: %s (%ux%u)", window_title, w, h);
        g_registry->windows[win_idx].shm_id = WIN_SHM_INVALID;
        return {NULL, 0, 0, 0, false};
    }

    uint32_t buffer_w = w;
    uint32_t buffer_h = h;

    int memfd = memfd_create("window_buffer", 0);
    if (memfd < 0) {
        LOG_ERROR("gui", "register_window memfd_create failed: %s (%ux%u)", window_title, w, h);
        g_registry->windows[win_idx].shm_id = WIN_SHM_INVALID;
        return {NULL, 0, 0, 0, false};
    }

    if (ftruncate(memfd, buffer_bytes) < 0) {
        LOG_ERROR("gui", "register_window ftruncate failed: %s size=%zu", window_title, buffer_bytes);
        close(memfd);
        g_registry->windows[win_idx].shm_id = WIN_SHM_INVALID;
        return {NULL, 0, 0, 0, false};
    }

    void *mapped_ptr = mmap(NULL, buffer_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (mapped_ptr == MAP_FAILED) {
        LOG_ERROR("gui", "register_window mmap failed: %s fd=%d", window_title, memfd);
        close(memfd);
        g_registry->windows[win_idx].shm_id = WIN_SHM_INVALID;
        return {NULL, 0, 0, 0, false};
    }

    int wm_fd = fd_transfer(0, memfd);
    if (wm_fd < 0) {
        LOG_ERROR("gui", "register_window fd_transfer failed: %s fd=%d", window_title, memfd);
        munmap(mapped_ptr, buffer_bytes);
        close(memfd);
        g_registry->windows[win_idx].shm_id = WIN_SHM_INVALID;
        return {NULL, 0, 0, 0, false};
    }

    close(memfd);
    int shm_id = wm_fd | 0x40000000;
    uint64_t win_ptr = reinterpret_cast<uint64_t>(mapped_ptr);
    memset(reinterpret_cast<void *>(win_ptr), 0, buffer_bytes);

    gui_publish_window_count_for_slot(win_idx);

    WindowEntry *win_entry = &g_registry->windows[win_idx];
    memset(win_entry, 0, sizeof(*win_entry));
    win_entry->shm_id = WIN_SHM_RESERVED;
    // Initial placement: centered in the WM's published work area (before
    // the WM publishes, fall back to a conservative default), with a small
    // per-window cascade so stacked windows stay distinguishable.
    int area_x = 0, area_y = 0, area_w = 1152, area_h = 748;
    if (g_registry->work_w > 0 && g_registry->work_h > 0) {
        area_x = static_cast<int>(g_registry->work_x);
        area_y = static_cast<int>(g_registry->work_y);
        area_w = static_cast<int>(g_registry->work_w);
        area_h = static_cast<int>(g_registry->work_h);
    }
    const int cascade = static_cast<int>(static_cast<uint32_t>(win_idx) % 6u) * 20;
    int place_x = area_x + (area_w - static_cast<int>(w)) / 2 + cascade;
    int place_y = area_y + (area_h - static_cast<int>(h)) / 2 + cascade;
    if (place_x + static_cast<int>(w) > area_x + area_w)
        place_x = area_x + area_w - static_cast<int>(w);
    if (place_x < area_x)
        place_x = area_x;
    if (place_y + static_cast<int>(h) > area_y + area_h)
        place_y = area_y + area_h - static_cast<int>(h);
    if (place_y < area_y)
        place_y = area_y;
    win_entry->x = place_x;
    win_entry->y = place_y;
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

    // Sample geometry + configure serial as one consistent triple: the WM
    // bumps position_serial after writing the geometry, so an unchanged
    // guard pair proves no write landed inside the read window (loads are
    // not reordered on x86; a compiler barrier is enough). A torn sample
    // would pair one configure's serial with another's size.
    uint32_t req_w, req_h, sync_serial;
    uint32_t position_guard;
    do {
        position_guard = g_my_window->position_serial;
        req_w = (g_my_window->w > 0) ? static_cast<uint32_t>(g_my_window->w) : s->width;
        req_h = (g_my_window->h > 0) ? static_cast<uint32_t>(g_my_window->h) : s->height;
        sync_serial = g_my_window->resize_serial;
        asm volatile("" ::: "memory");
    } while (g_my_window->position_serial != position_guard);
    bool resized = false;

    if ((g_my_window->flags & WIN_FLAG_RESIZABLE) != 0 && (req_w != s->width || req_h != s->height)) {
        // Grow the backing in place when needed (capacity grows with slack,
        // so a drag reallocates only occasionally); shrinking reuses it. If
        // the backing cannot fit the target, keep the current size; the
        // compositor retries the configure.
        if (!gui_resize_window_backing(s, req_w, req_h))
            return 0;
        s->width = req_w;
        s->height = req_h;
        resized = true;
    }

    if (sync_serial != 0 && g_my_window->buffer_resize_serial != sync_serial) {
        if (resized) {
            // The commit following the redraw acks this serial.
            g_synced_resize_serial = sync_serial;
        } else {
            // The surface already covers the requested geometry: the current
            // frame already is the answer.
            g_my_window->buffer_resize_serial = sync_serial;
            asm volatile("sfence" ::: "memory");
        }
    }

    return resized ? 1 : 0;
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
    // Clamp the WM-owned scroll offset to the new content bounds in the same
    // store-fence group as the content size. The WM re-clamps in its commit
    // pass, but that runs after the app already drew this frame with the stale
    // offset, which left sticky panels off-canvas for a frame on shrink
    // (navigate to a smaller folder while scrolled). Clamping here makes the
    // app draw and the compositor agree on the offset immediately.
    int max_scroll_x = content_w > view_w ? content_w - view_w : 0;
    int max_scroll_y = content_h > view_h ? content_h - view_h : 0;
    if (max_scroll_x <= 0 || g_my_window->scroll_x < 0)
        g_my_window->scroll_x = 0;
    else if (g_my_window->scroll_x > max_scroll_x)
        g_my_window->scroll_x = max_scroll_x;
    if (max_scroll_y <= 0 || g_my_window->scroll_y < 0)
        g_my_window->scroll_y = 0;
    else if (g_my_window->scroll_y > max_scroll_y)
        g_my_window->scroll_y = max_scroll_y;
    asm volatile("sfence" ::: "memory");
    return 0;
}

void gui_window_set_header_input(const Rect *rects, int count)
{
    if (!g_my_window)
        return;
    if (!rects)
        count = 0;
    if (count < 0)
        count = 0;
    if (count > WINDOW_HEADER_INPUT_MAX)
        count = WINDOW_HEADER_INPUT_MAX;

    WindowEntry *e = g_my_window;
    e->header_input_seq++;
    asm volatile("sfence" ::: "memory");
    e->header_input_count = count;
    for (int i = 0; i < count; i++)
        e->header_input[i] = rects[i];
    asm volatile("sfence" ::: "memory");
    e->header_input_seq++;
}

int gui_set_window_title(const char *title)
{
    if (!g_my_window || !title)
        return -1;
    strncpy(const_cast<char *>(g_my_window->title), title, sizeof(g_my_window->title) - 1);
    g_my_window->title[sizeof(g_my_window->title) - 1] = '\0';
    asm volatile("sfence" ::: "memory");
    return 0;
}

bool gui_window_title_matches(const char *window_title, const char *app_title)
{
    if (!window_title || !window_title[0] || !app_title || !app_title[0])
        return false;

    // window_title lives in shared memory (WindowEntry.title[64]); copy it into
    // a NUL-forced local so a writer that omitted the terminator cannot run
    // strlen past the field.
    char title[68];
    size_t n = 0;
    for (; n < 64 && window_title[n]; n++)
        title[n] = window_title[n];
    title[n] = '\0';
    if (!title[0])
        return false;

    if (strcmp(title, app_title) == 0)
        return true;

    // Apps rename their windows to "detail - App" (e.g. "data - Files"); the
    // app identity is the segment after the last " - " separator.
    size_t window_len = strlen(title);
    size_t app_len = strlen(app_title);
    if (window_len < app_len + 3)
        return false;
    if (strcmp(title + window_len - app_len, app_title) != 0)
        return false;
    const char *sep = title + window_len - app_len - 3;
    return sep[0] == ' ' && sep[1] == '-' && sep[2] == ' ';
}

void gui_notify(const char *title, const char *message)
{
    Registry *registry = gui_registry();
    if (!registry)
        return;
    strncpy(registry->notify_title, title ? title : "", sizeof(registry->notify_title) - 1);
    registry->notify_title[sizeof(registry->notify_title) - 1] = '\0';
    strncpy(registry->notify_message, message ? message : "", sizeof(registry->notify_message) - 1);
    registry->notify_message[sizeof(registry->notify_message) - 1] = '\0';
    asm volatile("sfence" ::: "memory");
    __sync_fetch_and_add(&registry->notify_generation, 1u);
    asm volatile("sfence" ::: "memory");
}

bool gui_open_request_submit(const char *path)
{
    Registry *registry = gui_registry();
    if (!registry || !path || !path[0])
        return false;
    // Seqlock: bump to an odd generation to mark the write in flight, publish
    // the path, then bump to even. A reader that observes an odd generation
    // knows the payload is mid-write and retries instead of copying it.
    __sync_fetch_and_add(&registry->open_generation, 1u);
    asm volatile("sfence" ::: "memory");
    strncpy(registry->open_path, path, sizeof(registry->open_path) - 1);
    registry->open_path[sizeof(registry->open_path) - 1] = '\0';
    asm volatile("sfence" ::: "memory");
    __sync_fetch_and_add(&registry->open_generation, 1u);
    asm volatile("sfence" ::: "memory");
    return true;
}

bool gui_open_request_take(char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return false;
    out[0] = '\0';
    Registry *registry = gui_registry();
    if (!registry)
        return false;
    // Seqlock snapshot: only an even, stable generation means the payload is
    // complete. Odd means the writer is mid-publish; retry briefly.
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t gen = registry->open_generation;
        if (gen == 0)
            return false;
        if (gen & 1u)
            continue;
        asm volatile("lfence" ::: "memory");
        strncpy(out, registry->open_path, out_size - 1);
        out[out_size - 1] = '\0';
        asm volatile("lfence" ::: "memory");
        if (registry->open_generation == gen) {
            __sync_lock_test_and_set(&registry->open_generation, 0u);
            return out[0] != '\0';
        }
    }
    return false;
}

void gui_menu_model_reset(MenuModel *model)
{
    if (!model)
        return;
    memset(model, 0, sizeof(*model));
}

int gui_menu_model_add_menu(MenuModel *model, const char *name)
{
    if (!model || !name || !name[0] || model->menu_count >= MENU_MAX_MENUS)
        return -1;
    MenuDef *def = &model->menus[model->menu_count];
    memset(def, 0, sizeof(*def));
    strncpy(def->name, name, sizeof(def->name) - 1);
    def->name[sizeof(def->name) - 1] = '\0';
    return (int)model->menu_count++;
}

bool gui_menu_model_add_item(MenuModel *model, int menu_index, const char *label, uint32_t id, uint32_t flags,
                             const char *accel)
{
    if (!model || menu_index < 0 || menu_index >= (int)model->menu_count || !label || !label[0])
        return false;
    // Ids in the menubar-reserved range would be intercepted by the menubar
    // instead of dispatched to the app; reject them at publish time.
    if (id >= MENU_CMD_APP_MAX)
        return false;
    MenuDef *def = &model->menus[menu_index];
    if (def->count >= MENU_MAX_ITEMS)
        return false;
    MenuItem *item = &def->items[def->count];
    memset(item, 0, sizeof(*item));
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';
    if (accel) {
        strncpy(item->accel, accel, sizeof(item->accel) - 1);
        item->accel[sizeof(item->accel) - 1] = '\0';
    }
    item->id = id;
    item->flags = (uint8_t)(flags & 0xFFu);
    def->count++;
    return true;
}

bool gui_menu_model_add_separator(MenuModel *model, int menu_index)
{
    if (!model || menu_index < 0 || menu_index >= (int)model->menu_count)
        return false;
    MenuDef *def = &model->menus[menu_index];
    if (def->count >= MENU_MAX_ITEMS)
        return false;
    memset(&def->items[def->count], 0, sizeof(MenuItem));
    def->count++;
    return true;
}

bool gui_menu_publish(const MenuModel *model)
{
    Registry *registry = gui_registry();
    if (!registry || !model || model->menu_count > MENU_MAX_MENUS)
        return false;
    if (!g_my_window)
        return false;

    MenuModel *slot = const_cast<MenuModel *>(&registry->menu_model);
    uint32_t owner_pid = g_my_window->owner_pid;

    // Bump to an odd seq to mark the payload in flight, write, then bump to
    // an even seq. Readers accept only a stable even seq.
    uint32_t begin_seq = __sync_add_and_fetch(&slot->seq, 1u);
    if ((begin_seq & 1u) == 0u)
        begin_seq = __sync_add_and_fetch(&slot->seq, 1u);
    asm volatile("sfence" ::: "memory");

    slot->owner_pid = owner_pid;
    slot->menu_count = model->menu_count;
    memcpy(const_cast<MenuDef *>(slot->menus), model->menus, sizeof(model->menus));
    asm volatile("sfence" ::: "memory");
    __sync_add_and_fetch(&slot->seq, 1u);
    return true;
}

bool gui_menu_take_command(uint32_t *out_id)
{
    static uint32_t last_seq = 0;
    if (!out_id || !g_my_window)
        return false;
    uint32_t seq = g_my_window->menu_command_seq;
    if (seq == last_seq)
        return false;
    last_seq = seq;
    *out_id = g_my_window->menu_command_id;
    return true;
}

bool gui_clipboard_copy(const char *text, size_t len)
{
    Registry *registry = gui_registry();
    if (!registry || (!text && len > 0))
        return false;
    if (len > MENU_CLIPBOARD_CAP)
        len = MENU_CLIPBOARD_CAP;

    uint32_t begin_seq = __sync_add_and_fetch(&registry->clipboard_seq, 1u);
    if ((begin_seq & 1u) == 0u)
        begin_seq = __sync_add_and_fetch(&registry->clipboard_seq, 1u);
    asm volatile("sfence" ::: "memory");

    if (len > 0)
        memcpy(registry->clipboard, text, len);
    registry->clipboard[len] = '\0';
    registry->clipboard_len = (uint32_t)len;
    asm volatile("sfence" ::: "memory");
    __sync_add_and_fetch(&registry->clipboard_seq, 1u);
    return true;
}

bool gui_clipboard_paste(char *out, size_t out_size, size_t *out_len)
{
    Registry *registry = gui_registry();
    if (!registry || !out || out_size == 0)
        return false;

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t seq_before = registry->clipboard_seq;
        if ((seq_before & 1u) != 0u)
            continue;
        uint32_t len = registry->clipboard_len;
        if (len > MENU_CLIPBOARD_CAP)
            len = MENU_CLIPBOARD_CAP;
        size_t copy_len = len < out_size - 1 ? len : out_size - 1;
        if (copy_len > 0)
            memcpy(out, const_cast<const char *>(registry->clipboard), copy_len);
        out[copy_len] = '\0';
        asm volatile("sfence" ::: "memory");
        if (registry->clipboard_seq == seq_before) {
            if (out_len)
                *out_len = copy_len;
            return copy_len > 0;
        }
    }
    out[0] = '\0';
    if (out_len)
        *out_len = 0;
    return false;
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

// Theme-aware translucent washes (see gui.h). Dark surfaces lift with low-alpha
// white; light surfaces shade with low-alpha black.
bool gui_theme_is_light(void)
{
    return g_applied_theme_mode == GUI_THEME_LIGHT;
}

uint32_t gui_hairline_color(void)
{
    return gui_theme_is_light() ? 0x14000000u : 0x14FFFFFFu;
}

uint32_t gui_hover_wash_color(void)
{
    return gui_theme_is_light() ? 0x0A000000u : 0x0FFFFFFFu;
}

uint32_t gui_inset_wash_color(void)
{
    return gui_theme_is_light() ? 0x14000000u : 0x1FFFFFFFu;
}

uint32_t gui_edge_wash_color(void)
{
    return gui_theme_is_light() ? 0x1A000000u : 0x12FFFFFFu;
}

uint32_t gui_subtle_card_wash_color(void)
{
    return gui_theme_is_light() ? 0x0A000000u : 0x0FFFFFFFu;
}

uint32_t gui_active_wash_color(void)
{
    return gui_theme_is_light() ? 0x1A000000u : 0x1FFFFFFFu;
}

int gui_ui_scale_pct(void)
{
    return resolve_ui_scale_pct();
}

int gui_scaled_metric(int base_px)
{
    return scaled_metric_floor(base_px);
}

int gui_space_0_5(void)
{
    return scaled_metric_floor(GUI_SPACE_0_5);
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
    return clamp_metric(gui_font_line_height(gui_font_title()) + gui_scaled_metric(14), scaled_metric_floor(32),
                        scaled_metric_floor(40));
}
int gui_badge_h(void)
{
    return gui_font_line_height(gui_font_default()) + gui_scaled_metric(4);
}
int gui_badge_pad_x(void)
{
    return scaled_metric_floor(GUI_BADGE_PAD_X);
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
                            gui_scaled_metric(22),
                        scaled_metric_floor(GUI_APP_HEADER_H), scaled_metric_floor(80));
}
int gui_app_row_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(24), scaled_metric_floor(40),
                        scaled_metric_floor(48));
}
int gui_app_control_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(12), scaled_metric_floor(28),
                        scaled_metric_floor(34));
}
int gui_app_row_gap(void)
{
    return gui_scaled_metric(2);
}
int gui_app_row_tall_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) * 2 + gui_scaled_metric(16), scaled_metric_floor(48),
                        scaled_metric_floor(58));
}
int gui_app_nav_h(void)
{
    return clamp_metric(gui_font_line_height(gui_font_default()) + gui_scaled_metric(16), scaled_metric_floor(32),
                        scaled_metric_floor(36));
}
int gui_scrollbar_w(void)
{
    return gui_scaled_metric(6);
}
int gui_scrollbar_min_thumb(void)
{
    return gui_scaled_metric(16);
}
int gui_dialog_button_w(void)
{
    return scaled_metric_floor(GUI_DIALOG_BUTTON_W);
}
int gui_headerbar_h(void)
{
    // The unified header height apps reserve at the top of their client area for
    // traffic lights + toolbar/breadcrumbs.
    return clamp_metric(gui_scaled_metric(44), scaled_metric_floor(40), scaled_metric_floor(48));
}
int gui_traffic_lights_w(void)
{
    // Right edge of the 3-button cluster the WM overlays at the headerbar's
    // left; apps start their own headerbar content at or past this x.
    return gui_scaled_metric(HEADER_TRAFFIC_CLUSTER_W);
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
    int rows =
        k_system_menu_item_count > k_menubar_canvas_max_rows ? k_system_menu_item_count : k_menubar_canvas_max_rows;
    // The space below the menu must hold the whole drop shadow: the canvas
    // is the dropdown's window, and a shadow clipped at its edge looks cut
    // off on screen.
    int total_h = gui_menubar_h() + menu_gap + (menu_item_h * rows) + gui_panel_shadow_pad();
    return total_h;
}

static void gui_draw_accent_strip(Surface *s, int x, int y, int w, int h, uint32_t color)
{
    if (!s || w <= 0 || h <= 0)
        return;
    gui_fill_rounded_rect(s, x, y, w, h, gui_corner_radius(w, h, w / 2), color);
}

// ---------------------------------------------------------------------------
// Glyph assets.
//
// Icons are authored as SVG sources under glyphs/ and rasterized to white
// .uoic silhouettes (the same pipeline as the app icons), staged under
// /usr/share/glyphs/. gui_draw_glyph decodes a silhouette once per
// kind/size, caches it and tints it to the requested foreground at draw
// time, so one asset serves every color context.
// ---------------------------------------------------------------------------

struct GlyphAssetEntry
{
    GuiGlyphKind kind;
    int size;
    uint32_t last_use;
    Surface surface;
};

static constexpr int GLYPH_ASSET_CACHE_SLOTS = 40;
static GlyphAssetEntry g_glyph_asset_cache[GLYPH_ASSET_CACHE_SLOTS] = {};
static uint32_t g_glyph_asset_clock = 0;

static const char *glyph_asset_name(GuiGlyphKind kind)
{
    switch (kind) {
        case GUI_GLYPH_FOLDER:
            return "folder";
        case GUI_GLYPH_FOLDER_UP:
            return "folder-up";
        case GUI_GLYPH_FILE:
            return "file";
        case GUI_GLYPH_FILE_TEXT:
            return "file-text";
        case GUI_GLYPH_FILE_CODE:
            return "file-code";
        case GUI_GLYPH_FILE_CONFIG:
            return "file-sliders";
        case GUI_GLYPH_FILE_IMAGE:
            return "file-image";
        case GUI_GLYPH_FILE_ARCHIVE:
            return "archive";
        case GUI_GLYPH_FILE_BINARY:
            return "file-binary";
        case GUI_GLYPH_DRIVE:
        case GUI_GLYPH_STORAGE:
            return "hard-drive";
        case GUI_GLYPH_HOME:
            return "house";
        case GUI_GLYPH_DESKTOP:
        case GUI_GLYPH_DISPLAY:
            return "monitor";
        case GUI_GLYPH_DOCUMENTS:
            return "copy";
        case GUI_GLYPH_DOWNLOADS:
            return "download";
        case GUI_GLYPH_MUSIC:
            return "music";
        case GUI_GLYPH_PICTURES:
            return "image";
        case GUI_GLYPH_ARROW_UP:
            return "arrow-up";
        case GUI_GLYPH_NETWORK:
            return "globe";
        case GUI_GLYPH_SETTINGS:
            return "settings";
        case GUI_GLYPH_APPEARANCE:
            return "sun";
        case GUI_GLYPH_DEVICES:
            return "laptop";
        case GUI_GLYPH_INFO:
            return "info";
        case GUI_GLYPH_WARNING:
            return "triangle-alert";
        case GUI_GLYPH_CLOCK:
            return "clock";
        case GUI_GLYPH_GRID:
            return "grid-2x2";
        case GUI_GLYPH_ANIMATION:
            return "play";
        case GUI_GLYPH_TRANSPARENCY:
            return "checkerboard";
        case GUI_GLYPH_VOLUME:
            return "volume-2";
        case GUI_GLYPH_CALENDAR:
            return "calendar";
        case GUI_GLYPH_TERMINAL:
            return "square-terminal";
        case GUI_GLYPH_APP:
            return "layout-grid";
        case GUI_GLYPH_COMMAND:
            return "terminal";
        case GUI_GLYPH_SEARCH:
            return "search";
        default:
            return nullptr;
    }
}

// Bilinearly resample a premultiplied BGRA frame into a square surface of
// the requested size (premultiplied channels filter without color fringing).
static bool glyph_resample_frame(Surface *dst, const Surface *src, int size)
{
    if (!dst || !dst->buffer || !src || !src->buffer || size <= 0)
        return false;
    const uint32_t dst_stride = dst->pitch / 4u;
    const uint32_t src_stride = src->pitch / 4u;
    const uint32_t src_w = src->width;
    const uint32_t src_h = src->height;

    for (int py = 0; py < size; py++) {
        uint64_t src_y_fp = ((uint64_t)py * (uint64_t)src_h * 65536u) / (uint32_t)size;
        uint32_t sy0 = (uint32_t)(src_y_fp >> 16);
        uint32_t frac_y = ((uint32_t)src_y_fp >> 8) & 0xFFu;
        uint32_t sy1 = sy0 + 1u < src_h ? sy0 + 1u : sy0;
        const uint32_t *src_row0 = &src->buffer[(size_t)sy0 * src_stride];
        const uint32_t *src_row1 = &src->buffer[(size_t)sy1 * src_stride];
        uint32_t *dst_row = &dst->buffer[(size_t)py * dst_stride];

        for (int px = 0; px < size; px++) {
            uint64_t src_x_fp = ((uint64_t)px * (uint64_t)src_w * 65536u) / (uint32_t)size;
            uint32_t sx0 = (uint32_t)(src_x_fp >> 16);
            uint32_t frac_x = ((uint32_t)src_x_fp >> 8) & 0xFFu;
            uint32_t sx1 = sx0 + 1u < src_w ? sx0 + 1u : sx0;

            uint32_t p00 = src_row0[sx0], p10 = src_row0[sx1];
            uint32_t p01 = src_row1[sx0], p11 = src_row1[sx1];
            uint32_t inv_fx = 256u - frac_x, inv_fy = 256u - frac_y;

            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                uint32_t c00 = (p00 >> shift) & 0xFFu, c10 = (p10 >> shift) & 0xFFu;
                uint32_t c01 = (p01 >> shift) & 0xFFu, c11 = (p11 >> shift) & 0xFFu;
                uint32_t top = c00 * inv_fx + c10 * frac_x;
                uint32_t bot = c01 * inv_fx + c11 * frac_x;
                uint32_t v = (top * inv_fy + bot * frac_y + 32768u) >> 16;
                if (v > 255u)
                    v = 255u;
                out |= v << shift;
            }
            dst_row[px] = out;
        }
    }
    return true;
}

static const Surface *glyph_asset_get(GuiGlyphKind kind, const char *name, int size)
{
    g_glyph_asset_clock++;

    for (int i = 0; i < GLYPH_ASSET_CACHE_SLOTS; i++) {
        GlyphAssetEntry &entry = g_glyph_asset_cache[i];
        if (entry.surface.buffer && entry.kind == kind && entry.size == size) {
            entry.last_use = g_glyph_asset_clock;
            return &entry.surface;
        }
    }

    int victim = 0;
    for (int i = 0; i < GLYPH_ASSET_CACHE_SLOTS; i++) {
        if (!g_glyph_asset_cache[i].surface.buffer) {
            victim = i;
            break;
        }
        if (g_glyph_asset_cache[i].last_use < g_glyph_asset_cache[victim].last_use)
            victim = i;
    }

    GlyphAssetEntry &entry = g_glyph_asset_cache[victim];
    if (entry.surface.buffer)
        gui_destroy_surface(&entry.surface);
    entry = {};

    char path[64];
    snprintf(path, sizeof(path), "/usr/share/glyphs/%s.uoic", name);

    Surface frame = {};
    if (!gui_load_uoic(path, (uint32_t)size, 100u, &frame) || !frame.buffer)
        return nullptr;

    if ((int)frame.width == size && (int)frame.height == size) {
        entry.surface = frame;
    } else {
        entry.surface = gui_create_surface((uint32_t)size, (uint32_t)size);
        if (!entry.surface.buffer || !glyph_resample_frame(&entry.surface, &frame, size)) {
            if (entry.surface.buffer)
                gui_destroy_surface(&entry.surface);
            gui_destroy_surface(&frame);
            entry = {};
            return nullptr;
        }
        gui_destroy_surface(&frame);
    }

    entry.kind = kind;
    entry.size = size;
    entry.last_use = g_glyph_asset_clock;
    return &entry.surface;
}

int gui_glyph_std_size(void)
{
    return gui_scaled_metric(16);
}

void gui_draw_glyph(Surface *s, int32_t x, int32_t y, int32_t size, GuiGlyphKind glyph, uint32_t fg)
{
    if (!s || !s->buffer || size < 8 || glyph == GUI_GLYPH_NONE || glyph >= GUI_GLYPH_COUNT)
        return;
    const char *name = glyph_asset_name(glyph);
    if (!name)
        return;
    const Surface *asset = glyph_asset_get(glyph, name, size);
    if (!asset || !asset->buffer || (int)asset->width != size || (int)asset->height != size)
        return;

    int sx = 0, sy = 0;
    int w = size, h = size;
    if (x < 0) {
        sx = -x;
        w += x;
        x = 0;
    }
    if (y < 0) {
        sy = -y;
        h += y;
        y = 0;
    }
    if (x + w > (int)s->width)
        w = (int)s->width - x;
    if (y + h > (int)s->height)
        h = (int)s->height - y;
    if (w <= 0 || h <= 0)
        return;

    // The asset is a white premultiplied silhouette: alpha carries coverage,
    // so tinting is a plain coverage blend of the requested foreground.
    const uint32_t src_stride = asset->pitch / 4u;
    const uint32_t dst_stride = s->pitch / 4u;
    for (int py = 0; py < h; py++) {
        const uint32_t *src_row = &asset->buffer[(size_t)(sy + py) * src_stride + sx];
        uint32_t *dst_row = &s->buffer[(size_t)(y + py) * dst_stride + x];
        for (int px = 0; px < w; px++) {
            uint8_t coverage = (uint8_t)(src_row[px] >> 24);
            if (coverage == 0)
                continue;
            dst_row[px] = gui_blend_straight_opaque_dst_coverage(dst_row[px], fg, coverage);
        }
    }
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
    // Unified headerbar: the top gui_headerbar_h() pixels are a layout-only
    // region for traffic lights and toolbar controls — NOT a painted box. The
    // canvas background fills the entire window continuously with no divider
    // below the band, so header and content read as one seamless surface.
    const int top_padding = gui_headerbar_h();
    gui_fill_surface(s, g_gui_style.app_bg);
    const int content_top = top_padding + gui_space_1();
    layout.outer_x = outer_padding;
    layout.outer_y = content_top;
    layout.outer_w = view_w - outer_padding * 2;
    layout.outer_h = view_h - content_top - outer_padding;
    if (layout.outer_w < 0)
        layout.outer_w = 0;
    if (layout.outer_h < 0)
        layout.outer_h = 0;

    // header_rect is the unified headerbar band itself (full width, top
    // gui_headerbar_h() rows). Apps place toolbar controls there and publish
    // them via gui_window_set_header_input so they stay clickable; body_rect
    // fills the content region below.
    layout.header_rect = gui_rect_make(0, 0, view_w, top_padding);
    layout.body_rect = gui_rect_make(layout.outer_x, layout.outer_y, layout.outer_w, layout.outer_h);
    return layout;
}

void gui_app_draw_nav_item(Surface *s, int x, int y, int w, int h, GuiGlyphKind icon, const char *label, bool active,
                           bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int pad_x = gui_space_1();
    const int r = gui_radius_sm();
    if (active) {
        // Neutral selection capsule, not solid blue — Adwaita/macOS sidebar
        // selection uses a subtle white overlay with crisp white text.
        gui_fill_rounded_rect(s, x, y, w, h, r, gui_active_wash_color());
    } else if (hovered) {
        gui_fill_rounded_rect(s, x, y, w, h, r, gui_hover_wash_color());
    }

    int text_x = x + pad_x;
    uint32_t fg = active ? g_gui_style.text : (hovered ? g_gui_style.text : g_gui_style.text_dim);
    if (icon != GUI_GLYPH_NONE) {
        int icon_size = gui_glyph_std_size();
        if (icon_size > h - gui_space_0_5())
            icon_size = h - gui_space_0_5();
        if (icon_size < 8)
            icon_size = 8;
        gui_draw_glyph(s, text_x, y + (h - icon_size) / 2, icon_size, icon, fg);
        text_x += icon_size + gui_space_1();
    }
    int text_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), text_x, text_y, x + w - pad_x - text_x, label ? label : "", fg, 0);
}

void gui_app_draw_list_row(Surface *s, int x, int y, int w, int h, GuiGlyphKind icon, const char *title,
                           const char *detail, bool active, bool hovered, bool muted)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    const int r = gui_radius_sm();

    // Flat rows: selection/hover are soft tonal washes, never framed boxes.
    if (active)
        gui_fill_rounded_rect(s, x, y, w, h, r, g_gui_style.accent_soft);
    else if (hovered)
        gui_fill_rounded_rect(s, x, y, w, h, r, gui_hover_wash_color());

    int text_x = x + space_2;
    uint32_t icon_fg = active ? g_gui_style.accent : (muted ? g_gui_style.text_muted : g_gui_style.text_dim);
    if (icon != GUI_GLYPH_NONE) {
        int icon_size = gui_glyph_std_size();
        if (icon_size > h - gui_space_0_5())
            icon_size = h - gui_space_0_5();
        if (icon_size < 8)
            icon_size = 8;
        gui_draw_glyph(s, text_x, y + (h - icon_size) / 2, icon_size, icon, icon_fg);
        text_x += icon_size + space_1 + gui_scaled_metric(2);
    }

    uint32_t detail_bg = active ? g_gui_style.accent_soft : 0;
    int text_w = w - (text_x - x) - space_2;
    if (detail && *detail) {
        int detail_w = gui_measure_text(gui_font_default(), detail);
        int detail_x = x + w - space_2 - detail_w;
        if (detail_x > text_x + gui_scaled_metric(64)) {
            int detail_y = gui_align_text_y(gui_font_default(), y, h);
            gui_draw_text_clipped(s, gui_font_default(), detail_x, detail_y, w - (detail_x - x) - space_2, detail,
                                  g_gui_style.text_muted, detail_bg);
            text_w = detail_x - text_x - space_1;
        }
    }

    int title_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), text_x, title_y, text_w, title ? title : "",
                          muted ? g_gui_style.text_muted : g_gui_style.text, detail_bg);
}

void gui_app_draw_badged_row(Surface *s, int x, int y, int w, int h, const char *badge, const char *title,
                             const char *detail, bool active, bool hovered, bool muted)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    const int r = gui_radius_sm();

    if (active)
        gui_fill_rounded_rect(s, x, y, w, h, r, g_gui_style.accent_soft);
    else if (hovered)
        gui_fill_rounded_rect(s, x, y, w, h, r, gui_hover_wash_color());

    int text_x = x + space_2;
    uint32_t wash = active ? g_gui_style.accent_soft : 0;
    if (badge && *badge) {
        uint32_t badge_bg = muted ? gui_edge_wash_color() : g_gui_style.accent_soft;
        uint32_t badge_fg = muted ? g_gui_style.text_muted : g_gui_style.accent;
        gui_draw_badge(s, x + space_2, y + (h - gui_badge_h()) / 2, badge, badge_bg, badge_fg);
        text_x += gui_measure_text(gui_font_default(), badge) + gui_badge_pad_x() * 2 + space_2;
    }

    int text_w = w - (text_x - x) - space_2;
    if (detail && *detail) {
        int detail_w = gui_measure_text(gui_font_default(), detail);
        int detail_x = x + w - space_2 - detail_w;
        if (detail_x > text_x + gui_scaled_metric(64)) {
            int detail_y = gui_align_text_y(gui_font_default(), y, h);
            gui_draw_text_clipped(s, gui_font_default(), detail_x, detail_y, w - (detail_x - x) - space_2, detail,
                                  g_gui_style.text_muted, wash);
            text_w = detail_x - text_x - space_1;
        }
    }

    int title_y = gui_align_text_y(gui_font_default(), y, h);
    gui_draw_text_clipped(s, gui_font_default(), text_x, title_y, text_w, title ? title : "",
                          muted ? g_gui_style.text_muted : g_gui_style.text, wash);
}

void gui_app_draw_toggle_row(Surface *s, int x, int y, int w, int h, GuiGlyphKind icon, const char *label,
                             const char *detail, bool on, bool active, bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_2 = gui_space_2();
    (void)active;
    if (hovered)
        gui_fill_rounded_rect(s, x, y, w, h, gui_radius_sm(), gui_hover_wash_color());

    int switch_w = scaled_metric_floor(38);
    int switch_h = scaled_metric_floor(22);
    int text_limit = w - (switch_w + space_2 * 2 + scaled_metric_floor(16));
    if (text_limit < scaled_metric_floor(84))
        text_limit = scaled_metric_floor(84);

    int text_x = x + space_2;
    if (icon != GUI_GLYPH_NONE) {
        int icon_size = gui_glyph_std_size();
        if (icon_size > h - gui_space_0_5())
            icon_size = h - gui_space_0_5();
        if (icon_size < 8)
            icon_size = 8;
        gui_draw_glyph(s, text_x, y + (h - icon_size) / 2, icon_size, icon,
                       on ? g_gui_style.accent : g_gui_style.text_dim);
        text_x += icon_size + gui_space_1() + gui_scaled_metric(2);
        text_limit -= (text_x - (x + space_2));
    }

    int text_block_h = gui_line_height() + ((detail && *detail) ? (gui_scaled_metric(2) + gui_line_height()) : 0);
    int label_y = y + (h - text_block_h) / 2;
    gui_draw_text_clipped(s, gui_font_default(), text_x, label_y, text_limit, label ? label : "", g_gui_style.text, 0);
    if (detail && *detail) {
        gui_draw_text_clipped(s, gui_font_default(), text_x, label_y + gui_line_height() + gui_scaled_metric(2),
                              text_limit, detail, g_gui_style.text_muted, 0);
    }

    int switch_x = x + w - space_2 - switch_w;
    int switch_y = y + (h - switch_h) / 2;
    int switch_r = switch_h / 2;
    uint32_t track_bg = on ? g_gui_style.accent : gui_inset_wash_color();
    gui_fill_rounded_rect(s, switch_x, switch_y, switch_w, switch_h, switch_r, track_bg);
    if (!on)
        gui_draw_rounded_rect(s, switch_x, switch_y, switch_w, switch_h, switch_r, gui_edge_wash_color());

    int knob_d = switch_h - gui_scaled_metric(5);
    if (knob_d < gui_scaled_metric(13))
        knob_d = gui_scaled_metric(13);
    if (knob_d > switch_h - gui_scaled_metric(3))
        knob_d = switch_h - gui_scaled_metric(3);
    int knob_y = switch_y + (switch_h - knob_d) / 2;
    int knob_x = on ? (switch_x + switch_w - knob_d - gui_scaled_metric(3)) : (switch_x + gui_scaled_metric(3));
    gui_fill_rounded_rect(s, knob_x, knob_y + 1, knob_d, knob_d, knob_d / 2, 0x33000000u);
    gui_fill_rounded_rect(s, knob_x, knob_y, knob_d, knob_d, knob_d / 2, COLOR_WHITE);
}

int gui_app_slider_h(void)
{
    return gui_space_2() * 2 + gui_line_height() + gui_scaled_metric(10) + gui_scaled_metric(16);
}

Rect gui_app_slider_track_rect(int x, int y, int w, int h)
{
    const int pad = gui_space_2();
    const int track_h = gui_scaled_metric(16);
    int track_y = y + h - pad - track_h;
    if (track_y < y + pad)
        track_y = y + pad;
    int track_w = w - pad * 2;
    if (track_w < 0)
        track_w = 0;
    return gui_rect_make(x + pad, track_y, track_w, track_h);
}

uint32_t gui_app_slider_value_from_x(int mouse_x, const Rect *track, uint32_t max_value)
{
    if (!track || track->w <= 0 || max_value == 0)
        return 0;
    int rel = mouse_x - track->x;
    if (rel < 0)
        rel = 0;
    if (rel > track->w)
        rel = track->w;
    uint64_t value = ((uint64_t)rel * max_value + (uint32_t)track->w / 2u) / (uint32_t)track->w;
    return value > max_value ? max_value : (uint32_t)value;
}

void gui_app_draw_slider(Surface *s, int x, int y, int w, int h, const char *label, uint32_t value, uint32_t max_value,
                         bool hovered, const char *value_text)
{
    if (!s || w <= 0 || h <= 0)
        return;
    if (max_value == 0)
        max_value = 1;
    if (value > max_value)
        value = max_value;

    const int space_2 = gui_space_2();
    (void)hovered;

    char percent_text[16];
    if (!value_text) {
        uint32_t percent = (uint32_t)(((uint64_t)value * 100u + max_value / 2u) / max_value);
        snprintf(percent_text, sizeof(percent_text), "%u%%", percent);
        value_text = percent_text;
    }
    int label_y = y + space_2;
    int value_w = gui_measure_text(gui_font_default(), value_text);
    gui_draw_text_clipped(s, gui_font_default(), x + space_2, label_y, w - value_w - space_2 * 3, label ? label : "",
                          g_gui_style.text, 0);
    gui_draw_text_clipped(s, gui_font_default(), x + w - space_2 - value_w, label_y, value_w + space_2, value_text,
                          g_gui_style.text_muted, 0);

    Rect track = gui_app_slider_track_rect(x, y, w, h);
    if (track.w <= 0 || track.h <= 0)
        return;
    int track_r = track.h / 2;
    gui_fill_rounded_rect(s, track.x, track.y, track.w, track.h, track_r, gui_inset_wash_color());

    uint64_t fill_w64 = ((uint64_t)value * track.w + max_value / 2u) / max_value;
    int fill_w = (int)fill_w64;
    if (value > 0 && fill_w < track.h)
        fill_w = track.h; // keep the fill cap round at small values
    if (fill_w > track.w)
        fill_w = track.w;
    if (fill_w > 0)
        gui_fill_rounded_rect(s, track.x, track.y, fill_w, track.h, track_r, g_gui_style.accent);

    int knob_d = track.h + gui_scaled_metric(6);
    int knob_x = track.x + fill_w - knob_d / 2;
    if (knob_x < track.x)
        knob_x = track.x;
    if (knob_x + knob_d > track.x + track.w)
        knob_x = track.x + track.w - knob_d;
    int knob_y = track.y + (track.h - knob_d) / 2;
    gui_fill_rounded_rect(s, knob_x, knob_y + 1, knob_d, knob_d, knob_d / 2, 0x33000000u);
    gui_fill_rounded_rect(s, knob_x, knob_y, knob_d, knob_d, knob_d / 2, COLOR_WHITE);
    gui_draw_rounded_rect(s, knob_x, knob_y, knob_d, knob_d, knob_d / 2, 0x14000000u);
}

void gui_app_draw_segmented_choice(Surface *s, int x, int y, int w, int h, const char *const *labels, int count,
                                   int selected, int hovered_index)
{
    if (!s || !labels || count <= 0 || w <= 0 || h <= 0)
        return;
    const int pad = gui_scaled_metric(2);
    int outer_r = gui_corner_radius(w, h, gui_radius_sm() + pad);
    // Inset well the segments sit in.
    gui_fill_rounded_rect(s, x, y, w, h, outer_r, g_gui_style.app_bg);
    gui_draw_rounded_rect(s, x, y, w, h, outer_r, gui_edge_wash_color());

    int seg_w = w / count;
    int pill_y = y + pad;
    int pill_h = h - pad * 2;

    for (int i = 0; i < count; i++) {
        int seg_x = x + i * seg_w;
        int actual_w = (i == count - 1) ? (x + w - seg_x) : seg_w;
        bool active = i == selected;
        bool hovered = i == hovered_index;

        if (active || hovered) {
            int pill_x = seg_x + pad;
            int pill_w = actual_w - pad * 2;
            if (pill_w > 0 && pill_h > 0) {
                if (active) {
                    gui_fill_rounded_rect(s, pill_x, pill_y + 1, pill_w, pill_h, gui_radius_sm(), 0x28000000u);
                    gui_fill_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, gui_radius_sm(),
                                          g_gui_style.app_surface_alt);
                } else {
                    gui_fill_rounded_rect(s, pill_x, pill_y, pill_w, pill_h, gui_radius_sm(), gui_hover_wash_color());
                }
            }
        }

        int text_y = gui_align_text_y(gui_font_default(), y, h);
        gui_draw_text_clipped(s, gui_font_default(), seg_x + gui_space_1(), text_y, actual_w - gui_space_2(), labels[i],
                              active ? g_gui_style.text : g_gui_style.text_dim, 0);
    }
}

void gui_app_draw_text_field(Surface *s, int x, int y, int w, int h, const char *value, bool focused, bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    uint32_t bg = g_gui_style.app_bg;
    int r = gui_corner_radius(w, h, gui_radius_sm());

    gui_fill_rounded_rect(s, x, y, w, h, r, bg);
    if (focused) {
        gui_draw_rounded_rect(s, x, y, w, h, r, g_gui_style.accent);
    } else {
        uint32_t border = gui_edge_wash_color();
        if (hovered)
            border = gui_theme_is_light() ? 0x2A000000u : 0x22FFFFFFu;
        gui_draw_rounded_rect(s, x, y, w, h, r, border);
    }
    const char *text = value ? value : "";
    int text_y = gui_align_text_y(gui_font_default(), y, h);
    const GuiFont *font = gui_font_default();
    int caret_w = gui_scaled_metric(1);
    if (caret_w < 1)
        caret_w = 1;
    int caret_h = h - space_1 * 2;
    if (caret_h < 4)
        caret_h = 4;
    int max_text_w = w - space_2 - space_1;
    if (max_text_w < 0)
        max_text_w = 0;
    int text_w = gui_measure_text(font, text);

    if (!focused || text_w <= max_text_w) {
        gui_draw_text_clipped(s, font, x + space_1, text_y, w - space_2, text, g_gui_style.text, bg);
    } else {
        // Overflow while editing at the end: reveal the tail so the caret and
        // the most recently typed characters stay visible.
        int budget = max_text_w - caret_w - 2;
        if (budget < 0)
            budget = 0;
        size_t len = strlen(text);
        size_t start = len;
        while (start > 0 && gui_measure_text_n(font, text + start - 1, len - (start - 1)) <= budget)
            start--;
        const char *shown = text + start;
        gui_draw_text_clipped(s, font, x + space_1, text_y, max_text_w, shown, g_gui_style.text, bg);
        text_w = gui_measure_text(font, shown);
        if (text_w > max_text_w)
            text_w = max_text_w;
    }
    if (focused) {
        if (text_w > max_text_w)
            text_w = max_text_w;
        gui_fill_rect(s, x + space_1 + text_w + 1, y + (h - caret_h) / 2, caret_w, caret_h, g_gui_style.accent);
    }
}

void gui_app_draw_button(Surface *s, int x, int y, int w, int h, const char *label, bool primary, bool focused,
                         bool hovered)
{
    gui_app_draw_button_ex(s, x, y, w, h, label, primary, focused, hovered, false);
}

void gui_app_draw_button_ex(Surface *s, int x, int y, int w, int h, const char *label, bool primary, bool focused,
                            bool hovered, bool pressed)
{
    if (!s || w <= 0 || h <= 0)
        return;
    const int space_1 = gui_space_1();
    const int space_2 = gui_space_2();
    uint32_t bg;
    uint32_t fg;
    int r = gui_corner_radius(w, h, gui_radius_sm());

    if (primary) {
        bg = g_gui_style.accent;
        if (hovered)
            bg = blend_pixel(bg, COLOR_WHITE, 26);
        if (pressed)
            bg = blend_pixel(g_gui_style.accent, 0xFF000000u, 44);
        fg = COLOR_WHITE;
        gui_fill_rounded_rect(s, x, y, w, h, r, bg);
    } else {
        bg = g_gui_style.app_surface_alt;
        if (hovered)
            bg = blend_pixel(bg, COLOR_WHITE, 14);
        if (pressed)
            bg = blend_pixel(g_gui_style.app_surface_alt, 0xFF000000u, 30);
        fg = g_gui_style.text;
        gui_fill_rounded_rect(s, x, y, w, h, r, bg);
        uint32_t border = focused
                              ? g_gui_style.accent
                              : (hovered ? (gui_theme_is_light() ? 0x2A000000u : 0x22FFFFFFu) : gui_edge_wash_color());
        gui_draw_rounded_rect(s, x, y, w, h, r, border);
    }

    int text_y = gui_align_text_y(gui_font_default(), y, h) + (pressed ? 1 : 0);
    int text_x = gui_align_text_x_center(gui_font_default(), x + space_1, w - space_2, label ? label : "");
    gui_draw_text_clipped(s, gui_font_default(), text_x, text_y, w - space_2, label ? label : "", fg, bg);
}

int gui_app_sidebar_row_h(void)
{
    return gui_app_nav_h();
}

void gui_draw_boxed_container(Surface *s, int x, int y, int w, int h)
{
    if (!s || w <= 0 || h <= 0)
        return;
    int r = gui_panel_radius(w, h);
    gui_fill_rounded_rect(s, x, y, w, h, r, g_gui_style.app_surface_alt);
}

void gui_draw_boxed_divider(Surface *s, int x, int y, int w, int indent)
{
    if (!s || w <= 0)
        return;
    if (indent > w - gui_space_1())
        indent = w - gui_space_1();
    if (indent < 0)
        indent = 0;
    int line_w = w - indent - gui_space_1_5();
    if (line_w <= 0)
        return;
    gui_draw_separator_h(s, x + indent, y, line_w, gui_hairline_color());
}

void gui_app_draw_section_caption(Surface *s, int x, int y, int w, const char *caption)
{
    if (!s || !caption || w <= 0)
        return;
    char upper[64];
    size_t i = 0;
    for (; caption[i] && i + 1 < sizeof(upper); i++) {
        char c = caption[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        upper[i] = c;
    }
    upper[i] = '\0';
    // Half-opacity uppercase caption segments sections without rules.
    uint32_t fg = blend_pixel(g_gui_style.app_surface, g_gui_style.text_muted, 200);
    gui_draw_text_clipped(s, gui_font_default(), x, y, w, upper, fg, 0);
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

// Distance from point (px,py) to segment (ax,ay)-(bx,by). Clamping the
// projection parameter to [0,1] yields round caps at the endpoints for free.
static inline float gui_point_seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax;
    float dy = by - ay;
    float len_sq = dx * dx + dy * dy;
    float t = 0.0f;
    if (len_sq > 0.0f) {
        t = ((px - ax) * dx + (py - ay) * dy) / len_sq;
        if (t < 0.0f)
            t = 0.0f;
        else if (t > 1.0f)
            t = 1.0f;
    }
    float ex = px - (ax + t * dx);
    float ey = py - (ay + t * dy);
    return libgui_sqrt(ex * ex + ey * ey);
}

static void draw_popup_checkmark(Surface *s, int x, int y, int size, uint32_t fg)
{
    if (!s || !s->buffer || s->pitch == 0 || size < 6)
        return;
    uint32_t stride = s->pitch / 4u;

    // Normalized tick geometry scaled into the [x, x+size] x [y, y+size] box:
    // a short descending leg meets a long ascending leg at a low elbow. The
    // stroke weight tracks the glyph size so it pairs with Inter at any scale.
    const float ox = static_cast<float>(x);
    const float oy = static_cast<float>(y);
    const float fl = static_cast<float>(size);
    const float ax = ox + fl * 0.12f;
    const float ay = oy + fl * 0.52f;
    const float bx = ox + fl * 0.40f;
    const float by = oy + fl * 0.80f;
    const float cx = ox + fl * 0.88f;
    const float cy = oy + fl * 0.20f;

    // Distance-field stroke: coverage ramps across a 1px band, giving smooth
    // anti-aliased edges like the font and rounded-rect renderers. Taking the
    // minimum distance to the two segments produces a round join at the elbow
    // and round caps at both ends.
    const float half = fl * 0.09f;
    const float aa = 0.5f;
    const float pad = half + aa;

    float min_x = ax < bx ? ax : bx;
    min_x = min_x < cx ? min_x : cx;
    float max_x = ax > bx ? ax : bx;
    max_x = max_x > cx ? max_x : cx;
    float min_y = ay < by ? ay : by;
    min_y = min_y < cy ? min_y : cy;
    float max_y = ay > by ? ay : by;
    max_y = max_y > cy ? max_y : cy;

    int x0 = static_cast<int>(min_x - pad);
    int y0 = static_cast<int>(min_y - pad);
    int x1 = static_cast<int>(max_x + pad) + 1;
    int y1 = static_cast<int>(max_y + pad) + 1;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > static_cast<int>(s->width))
        x1 = static_cast<int>(s->width);
    if (y1 > static_cast<int>(s->height))
        y1 = static_cast<int>(s->height);
    if (x0 >= x1 || y0 >= y1)
        return;

    for (int py = y0; py < y1; py++) {
        uint32_t *row = &s->buffer[static_cast<uint32_t>(py) * stride];
        float pyf = static_cast<float>(py) + 0.5f;
        for (int px = x0; px < x1; px++) {
            float pxf = static_cast<float>(px) + 0.5f;
            float d1 = gui_point_seg_dist(pxf, pyf, ax, ay, bx, by);
            float d2 = gui_point_seg_dist(pxf, pyf, bx, by, cx, cy);
            float d = d1 < d2 ? d1 : d2;
            float cov = half + aa - d;
            if (cov <= 0.0f)
                continue;
            uint8_t coverage = cov >= 1.0f ? 255 : static_cast<uint8_t>(cov * 255.0f);
            uint32_t *dst = &row[static_cast<uint32_t>(px)];
            *dst = gui_blend_straight_opaque_dst_coverage(*dst, fg, coverage);
        }
    }
}

void gui_draw_popup_menu_clipped(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count,
                                 int hovered_index, int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h)
{
    gui_draw_popup_menu_ext_clipped(s, x, y, w, items, count, hovered_index, nullptr, nullptr, clip_x, clip_y, clip_w,
                                    clip_h);
}

void gui_draw_popup_menu_ext_clipped(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count,
                                     int hovered_index, const char *const *accel_labels, const bool *checked_flags,
                                     int32_t clip_x, int32_t clip_y, int32_t clip_w, int32_t clip_h)
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

    gui_draw_panel_shadow_clipped(s, x, y, w, menu_h, radius, clip_x, clip_y, clip_w, clip_h);

    gui_draw_window_frame(s, x, y, w, menu_h, radius, g_gui_style.app_surface);

    int check_size = gui_font_line_height(gui_font_default()) * 2 / 3;
    int check_gap = gui_scaled_metric(4);
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
            bool checked = checked_flags && checked_flags[i];
            const char *accel = accel_labels ? accel_labels[i] : nullptr;
            int accel_w = 0;
            if (accel && accel[0])
                accel_w = gui_measure_text(gui_font_default(), accel);
            int text_y = gui_align_text_y(gui_font_default(), y_cursor, item_h);
            int text_x = x + outer_pad_x + row_pad_x;
            if (checked) {
                // ascent*5/8 = Inter cap-height midpoint; sit the tick on the
                // text optical axis instead of the taller row box.
                int check_y = text_y + (gui_font_ascent(gui_font_default()) * 5) / 8 - check_size / 2;
                draw_popup_checkmark(s, text_x, check_y, check_size, fg);
                text_x += check_size + check_gap;
            }
            int text_right = x + w - outer_pad_x - row_pad_x;
            if (accel_w > 0) {
                gui_draw_text_clipped(s, gui_font_default(), text_right - accel_w, text_y, accel_w, accel,
                                      items[i].enabled ? g_gui_style.text_muted : fg, row_bg);
                text_right -= accel_w + check_gap * 2;
            }
            int text_w = text_right - text_x;
            if (text_w > 0)
                gui_draw_text_clipped(s, gui_font_default(), text_x, text_y, text_w,
                                      items[i].label ? items[i].label : "", fg, row_bg);
        }
        y_cursor += items[i].separator ? popup_menu_separator_h() : item_h;
        if (i + 1 < count)
            y_cursor += row_gap;
    }
}

void gui_draw_popup_menu_ext(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count, int hovered_index,
                             const char *const *accel_labels, const bool *checked_flags)
{
    gui_draw_popup_menu_ext_clipped(s, x, y, w, items, count, hovered_index, accel_labels, checked_flags, 0, 0, 0, 0);
}

void gui_draw_popup_menu(Surface *s, int x, int y, int w, const GuiMenuItem *items, int count, int hovered_index)
{
    gui_draw_popup_menu_ext(s, x, y, w, items, count, hovered_index, nullptr, nullptr);
}

int gui_panel_shadow_pad(void)
{
    // Single source of truth for the soft-shadow spread. The WM's damage
    // math (wm_frame_shadow_offset_*), popup clamping and the shadow itself
    // must agree on it or moves/resize leave shadow trails.
    return gui_scaled_metric(16);
}

void gui_draw_panel_shadow_clipped(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, int32_t clip_x,
                                   int32_t clip_y, int32_t clip_w, int32_t clip_h)
{
    if (!s || !s->buffer || w <= 0 || h <= 0)
        return;
    // Soft, symmetric ambient shadow via a rounded-rect distance field.
    // Only the rim between the silhouette and the padded outer bounds is
    // touched (the body drawn afterwards covers the interior), and the
    // falloff is continuous rather than stacked quantized layers — no
    // banding, and an order of magnitude fewer blend operations than
    // filling whole translucent rects per layer.
    const int pad = gui_panel_shadow_pad();
    if (pad < 1)
        return;
    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;

    const int32_t ox = x - pad, oy = y - pad;
    const int32_t ow = w + pad * 2, oh = h + pad * 2;
    int32_t ix = ox, iy = oy, iw = ow, ih = oh;
    if (clip_w > 0 && clip_h > 0) {
        int32_t cx, cy, cw, ch;
        if (!gui_intersect_rect(ox, oy, ow, oh, clip_x, clip_y, clip_w, clip_h, &cx, &cy, &cw, &ch))
            return;
        ix = cx;
        iy = cy;
        iw = cw;
        ih = ch;
    }
    if (ix < 0) {
        iw += ix;
        ix = 0;
    }
    if (iy < 0) {
        ih += iy;
        iy = 0;
    }
    if (ix + iw > (int32_t)s->width)
        iw = (int32_t)s->width - ix;
    if (iy + ih > (int32_t)s->height)
        ih = (int32_t)s->height - iy;
    if (iw <= 0 || ih <= 0)
        return;

    // macOS-like presence: a visible contact edge that eases into a long
    // ambient tail — subtle, not obvious. Opaque black as the blend source;
    // the per-pixel coverage carries the alpha (gui_blend_pixel scales
    // src_a by coverage, so a zero-alpha src would be a no-op).
    const uint32_t max_alpha = gui_theme_is_light() ? 0x48u : 0x55u;
    const float half_w = (float)w * 0.5f;
    const float half_h = (float)h * 0.5f;
    const float cxr = (float)x + half_w;
    const float cyr = (float)y + half_h;
    const float fr = (float)r;
    const float fpad = (float)pad;
    const uint32_t stride = s->pitch / 4;

    for (int32_t py = iy; py < iy + ih; py++) {
        uint32_t *row = &s->buffer[(size_t)py * stride];
        // Sample at pixel CENTERS: the rect boundary sits at x+w (exclusive),
        // so integer sampling classifies the first outside pixel on
        // right/bottom as interior (dist==0) and clips its shadow, while
        // left/top keep theirs — an asymmetric 1-px transparent band.
        const float qy = fabsf((float)py + 0.5f - cyr) - (half_h - fr);
        for (int32_t px = ix; px < ix + iw; px++) {
            const float qx = fabsf((float)px + 0.5f - cxr) - (half_w - fr);
            // Rounded-rect signed distance (negative inside the silhouette):
            // length(max(q,0)) + min(max(qx,qy),0) - radius. The clamped
            // form used here yields identical outside distances.
            const float ax = qx > 0.0f ? qx : 0.0f;
            const float ay = qy > 0.0f ? qy : 0.0f;
            const float out = (ax > ay ? ax : ay);
            const float dist = sqrtf(ax * ax + ay * ay) + (out < 0.0f ? out : 0.0f) - fr;
            if (dist <= 0.0f)
                continue; // interior: the body covers it
            if (dist >= fpad)
                continue; // beyond the spread
            const float t = dist / fpad;
            // (1-t)^1.5: keeps a modest contact band for the first few
            // pixels, then a gradual tail to the spread edge.
            const float f = 1.0f - t;
            const uint32_t a = (uint32_t)((float)max_alpha * f * sqrtf(f) + 0.5f);
            if (a == 0)
                continue;
            row[px] = gui_blend_pixel(row[px], 0xFF000000u, (uint8_t)a);
        }
    }
}

void gui_draw_panel_shadow(Surface *s, int32_t x, int32_t y, int32_t w, int32_t h, int32_t r)
{
    gui_draw_panel_shadow_clipped(s, x, y, w, h, r, 0, 0, 0, 0);
}

uint32_t gui_window_outer_stroke_color(void)
{
    // A single semi-transparent hairline rather than an opaque grey: an opaque
    // mid-grey reads harsh/muddy against wallpapers, whereas a low-alpha stroke
    // darkens (light theme) or catches light on (dark theme) whatever sits behind
    // it, staying crisp over any wallpaper. The rich drop shadow does the heavy
    // lifting for separation, so this can stay faint.
    return gui_theme_is_light() ? 0x24000000u : 0x26FFFFFFu;
}

uint32_t gui_window_inner_rim_color(void)
{
    // Disabled: a second inner ring read as a 90s bevel/halo. The edge is a
    // single 1-px hairline; returning fully transparent makes gui_draw_window_rim
    // a no-op.
    return 0x00000000u;
}

void gui_draw_window_rim(Surface *s, int x, int y, int w, int h, int radius)
{
    if (!s || w <= 4 || h <= 4)
        return;
    uint32_t rim = gui_window_inner_rim_color();
    if ((rim >> 24) == 0)
        return; // rim disabled
    int r = gui_corner_radius(w, h, radius);
    gui_draw_rounded_rect(s, x + 1, y + 1, w - 2, h - 2, r > 0 ? r - 1 : 0, rim);
}

void gui_draw_window_ring(Surface *s, int x, int y, int w, int h, int radius)
{
    if (!s || w <= 2 || h <= 2)
        return;
    int r = gui_corner_radius(w, h, radius);
    // Outer dark stroke on the silhouette, then a light rim one pixel inside.
    gui_draw_rounded_rect(s, x, y, w, h, r, gui_window_outer_stroke_color());
    gui_draw_window_rim(s, x, y, w, h, radius);
}

void gui_draw_window_frame_no_rim(Surface *s, int x, int y, int w, int h, int radius, uint32_t body)
{
    if (!s || w <= 0 || h <= 0)
        return;
    int r = gui_corner_radius(w, h, radius);
    gui_fill_rounded_rect(s, x, y, w, h, r, body);
    if (w > 2 && h > 2)
        gui_draw_rounded_rect(s, x, y, w, h, r, gui_window_outer_stroke_color());
}

void gui_draw_window_frame(Surface *s, int x, int y, int w, int h, int radius, uint32_t body)
{
    gui_draw_window_frame_no_rim(s, x, y, w, h, radius, body);
    gui_draw_window_rim(s, x, y, w, h, radius);
}

static int dialog_panel_radius()
{
    return gui_radius_xl();
}

GuiDialogLayout gui_dialog_layout(int view_w, int view_h, int view_scroll_y, const char *const *lines, int line_count,
                                  bool has_field)
{
    GuiDialogLayout layout = {};
    if (view_w <= 0 || view_h <= 0)
        return layout;
    if (view_scroll_y < 0)
        view_scroll_y = 0;

    int content_w = 0;
    for (int i = 0; lines && i < line_count; i++) {
        int line_w = gui_measure_text(gui_font_default(), lines[i] ? lines[i] : "");
        if (line_w > content_w)
            content_w = line_w;
    }
    int panel_w = content_w + gui_space_2() * 2;
    if (panel_w < scaled_metric_floor(GUI_DIALOG_MIN_W))
        panel_w = scaled_metric_floor(GUI_DIALOG_MIN_W);
    if (panel_w > scaled_metric_floor(GUI_DIALOG_MAX_W))
        panel_w = scaled_metric_floor(GUI_DIALOG_MAX_W);
    if (panel_w > view_w - gui_space_4())
        panel_w = view_w - gui_space_4();
    if (panel_w < 0)
        panel_w = 0;

    int body_h = has_field ? gui_app_control_h()
                           : (line_count > 0 ? line_count * (gui_line_height() + gui_space_1()) - gui_space_1() : 0);
    int panel_h = gui_card_header_h() + gui_space_2() + body_h + gui_space_2() + gui_app_control_h() + gui_space_2();
    if (panel_h > view_h - gui_space_4())
        panel_h = view_h - gui_space_4();
    if (panel_h < 0)
        panel_h = 0;

    int panel_x = (view_w - panel_w) / 2;
    int panel_y = view_scroll_y + (view_h - panel_h) / 2;
    if (panel_x < 0)
        panel_x = 0;
    if (panel_y < view_scroll_y)
        panel_y = view_scroll_y;
    layout.panel = gui_rect_make(panel_x, panel_y, panel_w, panel_h);

    if (has_field) {
        layout.field = gui_rect_make(panel_x + gui_space_2(), panel_y + gui_card_header_h() + gui_space_2(),
                                     panel_w - gui_space_4(), gui_app_control_h());
    }

    int btn_w = gui_dialog_button_w();
    layout.confirm = gui_rect_make(panel_x + panel_w - gui_space_2() - btn_w,
                                   panel_y + panel_h - gui_space_2() - gui_app_control_h(), btn_w, gui_app_control_h());
    layout.cancel =
        gui_rect_make(layout.confirm.x - gui_space_1() - btn_w, layout.confirm.y, btn_w, gui_app_control_h());
    return layout;
}

void gui_draw_dialog(Surface *s, int view_w, int view_h, int view_scroll_y, const GuiDialogLayout *layout,
                     const char *title, const char *const *lines, int line_count, const char *field_value,
                     const char *confirm_label, bool confirm_hovered, bool confirm_pressed, const char *cancel_label,
                     bool cancel_hovered, bool cancel_pressed)
{
    if (!s || !layout || layout->panel.w <= 0 || layout->panel.h <= 0)
        return;
    if (view_scroll_y < 0)
        view_scroll_y = 0;

    gui_fill_rect_blend(s, 0, view_scroll_y, view_w, view_h, g_gui_style.overlay_scrim);

    const Rect &panel = layout->panel;
    int r = gui_corner_radius(panel.w, panel.h, dialog_panel_radius());
    gui_draw_panel_shadow(s, panel.x, panel.y, panel.w, panel.h, r);
    gui_draw_window_frame(s, panel.x, panel.y, panel.w, panel.h, r, g_gui_style.app_surface);
    gui_draw_card_header_ext(s, panel.x + 1, panel.y + 1, panel.w - 2,
                             gui_corner_radius(panel.w - 2, panel.h - 2, r - 1), title, nullptr);

    int text_x = panel.x + gui_space_2();
    int text_w = panel.w - gui_space_4();
    int text_y = panel.y + gui_card_header_h() + gui_space_2();
    if (field_value) {
        if (!gui_rect_is_empty(layout->field))
            gui_app_draw_text_field(s, layout->field.x, layout->field.y, layout->field.w, layout->field.h, field_value,
                                    true, false);
    } else {
        for (int i = 0; lines && i < line_count; i++) {
            gui_draw_text_clipped(s, gui_font_default(), text_x, text_y, text_w, lines[i] ? lines[i] : "",
                                  g_gui_style.text, g_gui_style.app_surface);
            text_y += gui_line_height() + gui_space_1();
        }
    }

    if (cancel_label && *cancel_label) {
        gui_app_draw_button_ex(s, layout->cancel.x, layout->cancel.y, layout->cancel.w, layout->cancel.h, cancel_label,
                               false, false, cancel_hovered, cancel_pressed);
    }
    gui_app_draw_button_ex(s, layout->confirm.x, layout->confirm.y, layout->confirm.w, layout->confirm.h,
                           confirm_label && *confirm_label ? confirm_label : "Close", true, false, confirm_hovered,
                           confirm_pressed);
}

void gui_draw_scrollbar(Surface *s, int x, int y, int w, int h, int thumb_offset, int thumb_length, bool hovered)
{
    if (!s || w <= 0 || h <= 0)
        return;
    int r = w / 2;
    gui_fill_rounded_rect(s, x, y, w, h, r, g_gui_style.app_surface_alt);
    if (thumb_length <= 0)
        return;
    if (thumb_offset < 0)
        thumb_offset = 0;
    if (thumb_offset + thumb_length > h)
        thumb_offset = h - thumb_length;
    if (thumb_offset < 0)
        thumb_offset = 0;
    uint32_t thumb_color = hovered ? g_gui_style.text_muted : g_gui_style.text_dim;
    gui_fill_rounded_rect(s, x, y + thumb_offset, w, thumb_length, r, thumb_color);
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

    const uint8_t (*cursor_data)[16] = arrow_data;
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

bool gui_load_file(const char *path, uint8_t **out_data, uint32_t *out_size)
{
    if (out_data)
        *out_data = nullptr;
    if (out_size)
        *out_size = 0;
    if (!path || !out_data || !out_size)
        return false;

    VNodeStat st = {};
    if (stat(path, &st) != 0 || st.is_dir || st.size == 0 || st.size > 0xFFFFFFFFu)
        return false;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    uint8_t *data = static_cast<uint8_t *>(malloc((size_t)st.size));
    if (!data) {
        close(fd);
        return false;
    }

    uint64_t total = 0;
    while (total < st.size) {
        int n = read(fd, data + total, (size_t)(st.size - total));
        if (n <= 0)
            break;
        total += (uint64_t)n;
    }
    close(fd);
    if (total != st.size) {
        free(data);
        return false;
    }

    *out_data = data;
    *out_size = (uint32_t)st.size;
    return true;
}

} // extern "C"
