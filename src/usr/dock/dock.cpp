#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/fs.h>
#include <uapi/gui.h>
#include <uapi/syscalls.h>
#include <uapi/sysinfo.h>
#include <unistd.h>

#include "../libc/syscall.h"
#include "../libgui/gui.h"
#include "../libgui/gui_canvas_utils.h"
#include "../shell_layout.h"

struct DockItem
{
    const char *title;
    const char *path;
    const char *glyph;
    const char *icon_name;
    uint32_t fallback_color;
};

static void reap_exited_children()
{
    int status = 0;
    while (waitpid_nohang(-1, &status) > 0) {
    }
}

struct DockIconAsset
{
    Surface surface;
    bool loaded;
    int active_left;
    int active_right;
};

static constexpr DockItem k_dock_items[] = {
    {"Files", "/bin/files.elf", "F", "files", 0xFFEAF0F8},
    {"Latitude", "/bin/latitude.elf", "L", "latitude", 0xFF26323F},
    {"Terminal", "/bin/terminal.elf", ">", "terminal", 0xFF151A22},
    {"Calculator", "/bin/calculator.elf", "C", "calculator", 0xFF4A90E2},
    {"Calendar", "/bin/calendar.elf", "D", "calendar", 0xFFD0021B},
    {"Clock", "/bin/clock.elf", "T", "clock", 0xFF417505},
    {"Settings", "/bin/preferences.elf", "S", "preferences", 0xFF818A99},
};
static DockIconAsset g_dock_icon_assets[(int)(sizeof(k_dock_items) / sizeof(k_dock_items[0]))] = {};
static DockIconAsset g_calendar_day_assets[7] = {};
static DockIconAsset g_calendar_num_assets[10] = {};
static Surface g_blur_surface = {};
static uint32_t g_last_blur_generation = 0;
static int g_last_blur_shm_id = -1;

static constexpr int k_dock_item_count = (int)(sizeof(k_dock_items) / sizeof(k_dock_items[0]));
static_assert(k_dock_item_count == SHELL_DOCK_ITEM_COUNT, "Dock layout constants must match dock items");

static inline int dock_hover_padding()
{
    return gui_scaled_metric(10);
}
static inline Rect dock_full_rect(uint32_t dock_w, uint32_t dock_h)
{
    return {0, 0, (int)dock_w, (int)dock_h};
}
static inline int dock_panel_radius(int panel_w, int panel_h)
{
    return gui_corner_radius(panel_w, panel_h, gui_scaled_metric(20));
}

static inline int dock_item_x(int item_index, uint32_t dock_w)
{
    int icon_size = shell_dock_icon_size();
    int spacing = shell_dock_icon_spacing();
    int total_icons_w = shell_dock_total_icons_w(k_dock_item_count);
    return shell_dock_panel_x(dock_w) + (shell_dock_panel_w(dock_w) - total_icons_w) / 2 +
           item_index * (icon_size + spacing);
}

static Rect dock_item_damage_rect(int item_index, uint32_t dock_w, uint32_t dock_h);
static int count_windows_for_item(Registry *registry, const DockItem &item);

static void get_surface_active_bounds(const Surface &s, int *out_left, int *out_right)
{
    int left = (int)s.width;
    int right = 0;
    uint32_t stride = s.pitch / 4u;
    for (uint32_t y = 0; y < s.height; y++) {
        const uint32_t *row = &s.buffer[(size_t)y * stride];
        for (uint32_t x = 0; x < s.width; x++) {
            uint8_t alpha = (uint8_t)(row[x] >> 24);
            if (alpha > 0) {
                if ((int)x < left)
                    left = (int)x;
                if ((int)x > right)
                    right = (int)x;
            }
        }
    }
    if (left > right) {
        *out_left = 0;
        *out_right = (int)s.width - 1;
    } else {
        *out_left = left;
        *out_right = right;
    }
}

static void submit_damage(WindowEntry &entry, const Rect &r)
{
    Rect clipped = r;
    int damage_w = entry.buffer_w > 0 ? entry.buffer_w : entry.w;
    int damage_h = entry.buffer_h > 0 ? entry.buffer_h : entry.h;
    if (!gui_clamp_rect_to_canvas(&clipped, damage_w, damage_h))
        return;
    damage_push(&entry.damage, clipped.x, clipped.y, clipped.w, clipped.h);
}

static void load_dock_icons()
{
    for (int i = 0; i < k_dock_item_count; i++) {
        DockIconAsset &asset = g_dock_icon_assets[i];
        if (asset.loaded)
            continue;

        char path[128];
        snprintf(path, sizeof(path), "/usr/share/appicons/%s.uoic", k_dock_items[i].icon_name);
        if (gui_load_uoic(path, SHELL_DOCK_ICON_SIZE, (uint32_t)gui_ui_scale_pct(), &asset.surface)) {
            asset.loaded = true;
        }
    }

    static const char *day_names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    for (int i = 0; i < 7; i++) {
        if (g_calendar_day_assets[i].loaded)
            continue;
        char path[128];
        snprintf(path, sizeof(path), "/usr/share/calendar/%s.uoic", day_names[i]);
        if (gui_load_uoic(path, SHELL_DOCK_ICON_SIZE / 2, (uint32_t)gui_ui_scale_pct(),
                          &g_calendar_day_assets[i].surface)) {
            g_calendar_day_assets[i].loaded = true;
        }
    }

    for (int i = 0; i < 10; i++) {
        if (g_calendar_num_assets[i].loaded)
            continue;
        char path[128];
        snprintf(path, sizeof(path), "/usr/share/calendar/%d.uoic", i);
        if (gui_load_uoic(path, SHELL_DOCK_ICON_SIZE / 2, (uint32_t)gui_ui_scale_pct(),
                          &g_calendar_num_assets[i].surface)) {
            g_calendar_num_assets[i].loaded = true;
            get_surface_active_bounds(g_calendar_num_assets[i].surface,
                                      &g_calendar_num_assets[i].active_left,
                                      &g_calendar_num_assets[i].active_right);
        }
    }
}

static bool ensure_blur_surface(Registry *registry, uint32_t dock_w, uint32_t dock_h)
{
    auto reset_blur_surface = []() {
        if (g_blur_surface.buffer && gui_shm_id_is_valid(g_last_blur_shm_id))
            syscall1(SYS_SHM_UNMAP, (uint64_t)g_last_blur_shm_id);
        g_blur_surface = {};
        g_last_blur_generation = 0;
        g_last_blur_shm_id = -1;
    };

    if (!registry || !gui_shm_id_is_valid(registry->dk_blur_shm_id) || registry->dk_blur_generation == 0) {
        reset_blur_surface();
        return false;
    }

    bool needs_remap = !g_blur_surface.buffer || g_last_blur_shm_id != registry->dk_blur_shm_id ||
                       g_blur_surface.width != dock_w || g_blur_surface.height != dock_h ||
                       g_blur_surface.pitch != dock_w * 4u;
    if (!needs_remap) {
        g_last_blur_generation = registry->dk_blur_generation;
        return true;
    }

    if (g_blur_surface.buffer && gui_shm_id_is_valid(g_last_blur_shm_id)) {
        syscall1(SYS_SHM_UNMAP, (uint64_t)g_last_blur_shm_id);
        g_blur_surface = {};
        g_last_blur_shm_id = -1;
        g_last_blur_generation = 0;
    }

    uint64_t required_bytes = (uint64_t)dock_w * (uint64_t)dock_h * 4u;
    uint64_t shm_bytes = syscall1(SYS_SHM_INFO, (uint64_t)registry->dk_blur_shm_id);
    if (shm_bytes == (uint64_t)-1 || shm_bytes < required_bytes) {
        reset_blur_surface();
        return false;
    }

    uint64_t map = syscall1(SYS_SHM_MAP, (uint64_t)registry->dk_blur_shm_id);
    if (map == 0 || map == (uint64_t)-1) {
        reset_blur_surface();
        return false;
    }

    g_blur_surface.buffer = (uint32_t *)map;
    g_blur_surface.width = dock_w;
    g_blur_surface.height = dock_h;
    g_blur_surface.pitch = dock_w * 4u;
    g_blur_surface.owns_buffer = false;
    g_last_blur_generation = registry->dk_blur_generation;
    g_last_blur_shm_id = registry->dk_blur_shm_id;
    return true;
}

static inline uint8_t dock_scale_alpha_u8(uint8_t alpha, uint8_t coverage)
{
    return (uint8_t)(((uint32_t)alpha * (uint32_t)coverage + 127u) / 255u);
}

static inline uint32_t dock_blend_pixel(uint32_t dst, uint32_t src, uint8_t coverage)
{
    uint8_t src_alpha = dock_scale_alpha_u8((uint8_t)(src >> 24), coverage);
    if (src_alpha == 0)
        return dst;

    uint8_t dst_alpha = (uint8_t)(dst >> 24);
    if (dst_alpha == 0)
        return ((uint32_t)src_alpha << 24) | (src & 0x00FFFFFFu);
    if (src_alpha == 255)
        return 0xFF000000u | (src & 0x00FFFFFFu);

    uint32_t inv = 255u - src_alpha;
    uint32_t out_alpha = (uint32_t)src_alpha + ((uint32_t)dst_alpha * inv + 127u) / 255u;
    if (out_alpha == 0)
        return 0;

    uint32_t dr_p = ((dst >> 16) & 0xFFu) * (uint32_t)dst_alpha;
    uint32_t dg_p = ((dst >> 8) & 0xFFu) * (uint32_t)dst_alpha;
    uint32_t db_p = (dst & 0xFFu) * (uint32_t)dst_alpha;
    uint32_t sr_p = ((src >> 16) & 0xFFu) * (uint32_t)src_alpha;
    uint32_t sg_p = ((src >> 8) & 0xFFu) * (uint32_t)src_alpha;
    uint32_t sb_p = (src & 0xFFu) * (uint32_t)src_alpha;

    uint32_t out_r_p = sr_p + (dr_p * inv + 127u) / 255u;
    uint32_t out_g_p = sg_p + (dg_p * inv + 127u) / 255u;
    uint32_t out_b_p = sb_p + (db_p * inv + 127u) / 255u;

    uint32_t r = (out_r_p + out_alpha / 2u) / out_alpha;
    uint32_t g = (out_g_p + out_alpha / 2u) / out_alpha;
    uint32_t b = (out_b_p + out_alpha / 2u) / out_alpha;
    return (out_alpha << 24) | (r << 16) | (g << 8) | b;
}

static inline uint8_t dock_unpremultiply_channel(uint32_t value, uint32_t alpha)
{
    if (alpha == 0)
        return 0;
    uint32_t out = (value * 255u + alpha / 2u) / alpha;
    return (uint8_t)(out > 255u ? 255u : out);
}

static inline uint32_t dock_blend_premultiplied_pixel(uint32_t dst, uint32_t src)
{
    uint32_t src_alpha = src >> 24;
    if (src_alpha == 0)
        return dst;
    if (src_alpha == 255)
        return 0xFF000000u | (src & 0x00FFFFFFu);

    uint32_t sr_pm = (src >> 16) & 0xFFu;
    uint32_t sg_pm = (src >> 8) & 0xFFu;
    uint32_t sb_pm = src & 0xFFu;

    uint32_t dst_alpha = dst >> 24;
    if (dst_alpha == 0) {
        uint32_t r = dock_unpremultiply_channel(sr_pm, src_alpha);
        uint32_t g = dock_unpremultiply_channel(sg_pm, src_alpha);
        uint32_t b = dock_unpremultiply_channel(sb_pm, src_alpha);
        return (src_alpha << 24) | (r << 16) | (g << 8) | b;
    }

    uint32_t inv = 255u - src_alpha;
    if (dst_alpha == 255) {
        uint32_t dr = (dst >> 16) & 0xFFu;
        uint32_t dg = (dst >> 8) & 0xFFu;
        uint32_t db = dst & 0xFFu;
        uint32_t r = sr_pm + (dr * inv + 127u) / 255u;
        uint32_t g = sg_pm + (dg * inv + 127u) / 255u;
        uint32_t b = sb_pm + (db * inv + 127u) / 255u;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    uint32_t out_alpha = src_alpha + (dst_alpha * inv + 127u) / 255u;
    if (out_alpha == 0)
        return 0;

    uint32_t dr_p = ((dst >> 16) & 0xFFu) * dst_alpha;
    uint32_t dg_p = ((dst >> 8) & 0xFFu) * dst_alpha;
    uint32_t db_p = (dst & 0xFFu) * dst_alpha;
    uint32_t sr_p = sr_pm * 255u;
    uint32_t sg_p = sg_pm * 255u;
    uint32_t sb_p = sb_pm * 255u;

    uint32_t out_r_p = sr_p + (dr_p * inv + 127u) / 255u;
    uint32_t out_g_p = sg_p + (dg_p * inv + 127u) / 255u;
    uint32_t out_b_p = sb_p + (db_p * inv + 127u) / 255u;

    uint32_t r = (out_r_p + out_alpha / 2u) / out_alpha;
    uint32_t g = (out_g_p + out_alpha / 2u) / out_alpha;
    uint32_t b = (out_b_p + out_alpha / 2u) / out_alpha;
    return (out_alpha << 24) | (r << 16) | (g << 8) | b;
}

static void blit_blur_rounded_rect(Surface *canvas, Surface *blur, int x, int y, int w, int h, int radius)
{
    if (!canvas || !blur || !canvas->buffer || !blur->buffer)
        return;
    uint32_t dst_stride = canvas->pitch / 4u;
    uint32_t src_stride = blur->pitch / 4u;
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            uint8_t coverage = gui_rounded_rect_coverage_local(px - x, py - y, w, h, radius, 3u);
            if (coverage == 0)
                continue;
            uint32_t src = blur->buffer[(size_t)py * src_stride + (uint32_t)px];
            uint32_t dst = canvas->buffer[(size_t)py * dst_stride + (uint32_t)px];
            canvas->buffer[(size_t)py * dst_stride + (uint32_t)px] = dock_blend_pixel(dst, src, coverage);
        }
    }
}

static void draw_dock_glass(Surface *canvas, Registry *registry, int panel_x, int panel_y, int panel_w, int panel_h,
                            int radius)
{
    bool is_light = registry->theme_mode == GUI_THEME_LIGHT;
    bool solid = registry && registry->transparency_level >= 255;

    uint32_t soft_shadow = is_light ? 0x10000000u : 0x12000000u;
    uint32_t key_shadow = is_light ? 0x18000000u : 0x1A000000u;
    uint32_t tint_color = is_light ? 0x80FFFFFFu : 0x801B1D21u;
    uint32_t border_color = is_light ? 0x56FFFFFFu : 0x22FFFFFFu;
    uint32_t inner_highlight = is_light ? 0x38FFFFFFu : 0x10FFFFFFu;

    gui_fill_rounded_rect(canvas, panel_x, panel_y + gui_scaled_metric(5), panel_w, panel_h, radius, soft_shadow);
    gui_fill_rounded_rect(canvas, panel_x, panel_y + gui_scaled_metric(2), panel_w, panel_h, radius, key_shadow);

    if (solid) {
        uint32_t fill = is_light ? 0xFFF7F9FCu : 0xFF141820u;
        uint32_t stroke = is_light ? 0xFFD8DEE8u : 0xFF333842u;
        uint32_t inner = is_light ? 0xFFFFFFFFu : 0xFF242A33u;
        gui_fill_rounded_rect(canvas, panel_x, panel_y, panel_w, panel_h, radius, fill);
        gui_draw_rounded_rect(canvas, panel_x, panel_y, panel_w, panel_h, radius, stroke);
        if (panel_w > 4 && panel_h > 4) {
            gui_draw_rounded_rect(canvas, panel_x + 1, panel_y + 1, panel_w - 2, panel_h - 2,
                                  gui_corner_radius(panel_w - 2, panel_h - 2, radius - 1), inner);
        }
        return;
    }

    bool blur_ready = ensure_blur_surface(registry, canvas->width, canvas->height);
    if (blur_ready)
        blit_blur_rounded_rect(canvas, &g_blur_surface, panel_x, panel_y, panel_w, panel_h, radius);

    gui_fill_rounded_rect(canvas, panel_x, panel_y, panel_w, panel_h, radius, tint_color);
    gui_draw_rounded_rect(canvas, panel_x, panel_y, panel_w, panel_h, radius, border_color);
    if (panel_w > 4 && panel_h > 4) {
        gui_draw_rounded_rect(canvas, panel_x + 1, panel_y + 1, panel_w - 2, panel_h - 2,
                              gui_corner_radius(panel_w - 2, panel_h - 2, radius - 1), inner_highlight);
    }
}

static uint32_t dock_visual_signature(Registry *registry)
{
    uint32_t sig = 5381u;
    for (int item = 0; item < k_dock_item_count; item++)
        sig = sig * 33u + (uint32_t)count_windows_for_item(registry, k_dock_items[item]);
    return sig;
}

static bool window_entry_present(const WindowEntry *entry)
{
    if (!entry)
        return false;
    if (!entry->ready || !gui_shm_id_is_valid(entry->shm_id) || entry->owner_pid == 0)
        return false;
    return true;
}

static bool window_entry_usable(const WindowEntry *entry)
{
    return window_entry_present(entry) && entry->state != WIN_HIDDEN;
}

static bool window_entry_title_equals(const WindowEntry &entry, const char *title)
{
    if (!title)
        return false;

    size_t i = 0;
    for (; i < sizeof(entry.title); i++) {
        char entry_ch = entry.title[i];
        char title_ch = title[i];
        if (entry_ch != title_ch)
            return false;
        if (entry_ch == '\0')
            return true;
    }
    return title[i] == '\0';
}

static bool window_title_matches_item(const WindowEntry &entry, const DockItem &item)
{
    return window_entry_title_equals(entry, item.title);
}

static bool window_matches_item(const WindowEntry &entry, const DockItem &item)
{
    return window_entry_usable(&entry) && window_title_matches_item(entry, item);
}

static bool window_matches_item_restorable(const WindowEntry &entry, const DockItem &item)
{
    return window_entry_present(&entry) && window_title_matches_item(entry, item);
}

static int count_windows_for_item(Registry *registry, const DockItem &item)
{
    uint32_t count = registry->window_count;
    if (count > MAX_WINDOWS)
        count = MAX_WINDOWS;
    int total = 0;
    for (uint32_t i = 2; i < count; i++) {
        if (window_matches_item(registry->windows[i], item))
            total++;
    }
    return total;
}

static int find_window_for_item(Registry *registry, const DockItem &item, int after_slot)
{
    uint32_t count = registry->window_count;
    if (count > MAX_WINDOWS)
        count = MAX_WINDOWS;
    int first_match = -1;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t start = (pass == 0 && after_slot >= 2) ? (uint32_t)(after_slot + 1) : 2u;
        for (uint32_t i = start; i < count; i++) {
            if (!window_matches_item_restorable(registry->windows[i], item))
                continue;
            if (first_match < 0)
                first_match = (int)i;
            if (registry->windows[i].state != WIN_HIDDEN)
                return (int)i;
        }
    }
    return first_match;
}

static Rect dock_item_damage_rect(int item_index, uint32_t dock_w, uint32_t dock_h)
{
    int icon_x = dock_item_x(item_index, dock_w);
    int icon_y = shell_dock_icon_y(dock_h);
    int icon_size = shell_dock_icon_size();
    int hover_pad = dock_hover_padding();

    int left = icon_x - hover_pad;
    int right = icon_x + icon_size + hover_pad;
    int top = icon_y - gui_scaled_metric(4);
    int bottom = shell_dock_indicator_y(dock_h) + shell_dock_indicator_size() + hover_pad;

    const DockItem &item = k_dock_items[item_index];
    const GuiFont *font = gui_font_default();
    int text_w = gui_measure_text(font, item.title ? item.title : "") + gui_scaled_metric(2);
    int pad_x = gui_scaled_metric(9);
    int pill_h = gui_scaled_metric(18);
    int pill_w = text_w + pad_x * 2;
    int pill_x = icon_x + (icon_size - pill_w) / 2;
    int pill_y = shell_dock_panel_y() - pill_h - gui_scaled_metric(4);
    int edge_pad = gui_scaled_metric(4);
    if (pill_x < edge_pad)
        pill_x = edge_pad;
    if (pill_x + pill_w > (int)dock_w - edge_pad)
        pill_x = (int)dock_w - edge_pad - pill_w;
    if (pill_y < gui_scaled_metric(2))
        pill_y = gui_scaled_metric(2);

    if (pill_x - gui_scaled_metric(2) < left)
        left = pill_x - gui_scaled_metric(2);
    if (pill_x + pill_w + gui_scaled_metric(2) > right)
        right = pill_x + pill_w + gui_scaled_metric(2);
    if (pill_y - gui_scaled_metric(2) < top)
        top = pill_y - gui_scaled_metric(2);

    Rect dirty = {left, top, right - left, bottom - top};
    if (!gui_clamp_rect_to_canvas(&dirty, (int)dock_w, (int)dock_h))
        return dock_full_rect(dock_w, dock_h);
    return dirty;
}

static void draw_fallback_icon(Surface *canvas, int item_index, int x, int y, int size)
{
    const DockItem &item = k_dock_items[item_index];
    int r = gui_corner_radius(size, size, gui_scaled_metric(11));
    uint32_t fill = item.fallback_color;
    uint32_t highlight = (fill == 0xFFEAF0F8u) ? 0x66FFFFFFu : 0x32FFFFFFu;
    uint32_t border = (fill == 0xFFEAF0F8u) ? 0x42000000u : 0x44FFFFFFu;

    gui_fill_rounded_rect(canvas, x, y, size, size, r, fill);
    if (size > 6)
        gui_fill_rounded_rect(canvas, x + 2, y + 2, size - 4, size / 2, gui_corner_radius(size - 4, size / 2, r - 2),
                              highlight);
    gui_draw_rounded_rect(canvas, x, y, size, size, r, border);

    uint32_t text_color = (fill == 0xFFEAF0F8u) ? 0xFF2A3240u : 0xFFFFFFFFu;
    int label_w = gui_text_width(item.glyph);
    int label_x = x + (size - label_w) / 2;
    int label_y = y + (size - gui_line_height()) / 2;
    gui_draw_string(canvas, label_x, label_y, item.glyph, text_color, fill);
}

static void draw_icon_shadow(Surface *canvas, int x, int y, int size)
{
    if (!canvas)
        return;

    int outer_inset = gui_scaled_metric(3);
    int outer_w = size - outer_inset * 2;
    int outer_h = size - outer_inset * 2;
    if (outer_w <= 0 || outer_h <= 0)
        return;

    int outer_x = x + outer_inset;
    int outer_y = y + outer_inset + gui_scaled_metric(1);
    int outer_radius = gui_corner_radius(outer_w, outer_h, gui_scaled_metric(11));
    gui_fill_rounded_rect(canvas, outer_x, outer_y, outer_w, outer_h, outer_radius, 0x0D000000u);

    int inner_inset = gui_scaled_metric(6);
    int inner_w = size - inner_inset * 2;
    int inner_h = size - inner_inset * 2;
    if (inner_w > 0 && inner_h > 0) {
        int inner_x = x + inner_inset;
        int inner_y = y + inner_inset;
        int inner_radius = gui_corner_radius(inner_w, inner_h, gui_scaled_metric(8));
        gui_fill_rounded_rect(canvas, inner_x, inner_y, inner_w, inner_h, inner_radius, 0x05000000u);
    }
}

static inline uint32_t bilinear_blend_channel(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, uint32_t fx,
                                              uint32_t fy)
{
    uint32_t inv_fx = 256u - fx;
    uint32_t inv_fy = 256u - fy;
    uint32_t top = c00 * inv_fx + c10 * fx;
    uint32_t bot = c01 * inv_fx + c11 * fx;
    return (top * inv_fy + bot * fy + 32768u) >> 16;
}

static inline uint32_t bilinear_sample(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, uint32_t fx, uint32_t fy)
{
    uint32_t a = bilinear_blend_channel(p00 >> 24, p10 >> 24, p01 >> 24, p11 >> 24, fx, fy);
    uint32_t r = bilinear_blend_channel((p00 >> 16) & 0xFFu, (p10 >> 16) & 0xFFu, (p01 >> 16) & 0xFFu,
                                        (p11 >> 16) & 0xFFu, fx, fy);
    uint32_t g =
        bilinear_blend_channel((p00 >> 8) & 0xFFu, (p10 >> 8) & 0xFFu, (p01 >> 8) & 0xFFu, (p11 >> 8) & 0xFFu, fx, fy);
    uint32_t b = bilinear_blend_channel(p00 & 0xFFu, p10 & 0xFFu, p01 & 0xFFu, p11 & 0xFFu, fx, fy);
    if (a > 255u)
        a = 255u;
    if (r > 255u)
        r = 255u;
    if (g > 255u)
        g = 255u;
    if (b > 255u)
        b = 255u;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static void draw_scaled_app_icon(Surface *canvas, const Surface *icon, int x, int y, int w, int h)
{
    if (!canvas || !canvas->buffer || !icon || !icon->buffer || w <= 0 || h <= 0 || icon->width == 0 ||
        icon->height == 0)
        return;

    int clip_left = x < 0 ? 0 : x;
    int clip_top = y < 0 ? 0 : y;
    int clip_right = x + w > (int)canvas->width ? (int)canvas->width : x + w;
    int clip_bottom = y + h > (int)canvas->height ? (int)canvas->height : y + h;
    if (clip_left >= clip_right || clip_top >= clip_bottom)
        return;

    uint32_t dst_stride = canvas->pitch / 4u;
    uint32_t src_stride = icon->pitch / 4u;
    uint32_t src_w = icon->width;
    uint32_t src_h = icon->height;

    for (int py = clip_top; py < clip_bottom; py++) {
        int local_y = py - y;
        // Fixed-point 16.16 source coordinate
        uint64_t src_y_fp = ((uint64_t)local_y * (uint64_t)src_h * 65536u) / (uint32_t)h;
        uint32_t sy0 = (uint32_t)(src_y_fp >> 16);
        uint32_t frac_y = ((uint32_t)src_y_fp >> 8) & 0xFFu;
        uint32_t sy1 = sy0 + 1u < src_h ? sy0 + 1u : sy0;

        uint32_t *dst_row = &canvas->buffer[(size_t)py * dst_stride];
        const uint32_t *src_row0 = &icon->buffer[(size_t)sy0 * src_stride];
        const uint32_t *src_row1 = &icon->buffer[(size_t)sy1 * src_stride];

        for (int px = clip_left; px < clip_right; px++) {
            int local_x = px - x;
            uint64_t src_x_fp = ((uint64_t)local_x * (uint64_t)src_w * 65536u) / (uint32_t)w;
            uint32_t sx0 = (uint32_t)(src_x_fp >> 16);
            uint32_t frac_x = ((uint32_t)src_x_fp >> 8) & 0xFFu;
            uint32_t sx1 = sx0 + 1u < src_w ? sx0 + 1u : sx0;

            uint32_t p00 = src_row0[sx0];
            uint32_t p10 = src_row0[sx1];
            uint32_t p01 = src_row1[sx0];
            uint32_t p11 = src_row1[sx1];

            uint32_t pixel = bilinear_sample(p00, p10, p01, p11, frac_x, frac_y);
            uint8_t alpha = (uint8_t)(pixel >> 24);
            if (alpha == 0)
                continue;
            if (alpha == 255)
                dst_row[px] = pixel;
            else
                dst_row[px] = dock_blend_premultiplied_pixel(dst_row[px], pixel);
        }
    }
}

static inline void draw_scaled_app_icon(Surface *canvas, const Surface *icon, int x, int y, int size)
{
    draw_scaled_app_icon(canvas, icon, x, y, size, size);
}

static int day_of_week(int year, int month, int day)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year;
    int m = month;
    int d = day;
    y -= m < 3;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void draw_calendar_contents(Surface *canvas, int x, int y, int size)
{
    SysTime t;
    if (get_time(&t) != 0)
        return;

    int weekday_h = (size * 40) / 100;
    int date_h = (size * 50) / 100;
    int gap = (size * 1) / 100;
    int total_h = weekday_h + gap + date_h;
    int start_y = y + (size - total_h) / 2 - (size * 2) / 100;

    int wday = day_of_week((int)t.year, (int)t.month, (int)t.day);
    if (g_calendar_day_assets[wday].loaded) {
        const Surface &ws = g_calendar_day_assets[wday].surface;
        int day_w = (int)(((float)weekday_h * (float)ws.width / (float)ws.height) + 0.5f);
        int day_x = x + (size - day_w) / 2;
        draw_scaled_app_icon(canvas, &ws, day_x, start_y, day_w, weekday_h);
    }

    int date = (int)t.day;
    int num_y = start_y + weekday_h + gap;

    if (date < 10) {
        if (g_calendar_num_assets[date].loaded) {
            const DockIconAsset &asset = g_calendar_num_assets[date];
            const Surface &ns = asset.surface;
            int num_w = (int)(((float)date_h * (float)ns.width / (float)ns.height) + 0.5f);

            float scale = (float)date_h / (float)ns.height;
            float vis_w = (float)(asset.active_right - asset.active_left + 1) * scale;
            float scaled_left = (float)asset.active_left * scale;

            float active_start_x = (float)x + ((float)size - vis_w) / 2.0f;
            float draw_x = active_start_x - scaled_left;

            draw_scaled_app_icon(canvas, &ns, (int)(draw_x + 0.5f), num_y, num_w, date_h);
        }
    } else {
        int d1 = date / 10;
        int d2 = date % 10;
        if (g_calendar_num_assets[d1].loaded && g_calendar_num_assets[d2].loaded) {
            const DockIconAsset &asset1 = g_calendar_num_assets[d1];
            const DockIconAsset &asset2 = g_calendar_num_assets[d2];
            const Surface &s1 = asset1.surface;
            const Surface &s2 = asset2.surface;

            int w1 = (int)(((float)date_h * (float)s1.width / (float)s1.height) + 0.5f);
            int w2 = (int)(((float)date_h * (float)s2.width / (float)s2.height) + 0.5f);

            float scale1 = (float)date_h / (float)s1.height;
            float scale2 = (float)date_h / (float)s2.height;

            float vis_w1 = (float)(asset1.active_right - asset1.active_left + 1) * scale1;
            float vis_w2 = (float)(asset2.active_right - asset2.active_left + 1) * scale2;

            float scaled_left1 = (float)asset1.active_left * scale1;
            float scaled_left2 = (float)asset2.active_left * scale2;

            int digit_gap = gui_scaled_metric(1);

            float total_vis_w = vis_w1 + vis_w2 + (float)digit_gap;
            float active_start_x = (float)x + ((float)size - total_vis_w) / 2.0f;

            float x1 = active_start_x - scaled_left1;
            float x2 = active_start_x + vis_w1 + (float)digit_gap - scaled_left2;

            draw_scaled_app_icon(canvas, &s1, (int)(x1 + 0.5f), num_y, w1, date_h);
            draw_scaled_app_icon(canvas, &s2, (int)(x2 + 0.5f), num_y, w2, date_h);
        }
    }
}

static void draw_hover_label(Surface *canvas, const DockItem &item, int panel_y, int icon_x, int icon_size,
                             bool is_light)
{
    const GuiFont *font = gui_font_default();
    const char *label = item.title ? item.title : "";
    int text_w = gui_measure_text(font, label) + gui_scaled_metric(2);
    int pad_x = gui_scaled_metric(9);
    int pill_h = gui_scaled_metric(18);
    int pill_w = text_w + pad_x * 2;
    int pill_x = icon_x + (icon_size - pill_w) / 2;
    int gap = gui_scaled_metric(4);
    int pill_y = panel_y - pill_h - gap;
    uint32_t pill_bg = is_light ? 0xF4FFFFFFu : 0xF01E2126u;
    uint32_t pill_border = is_light ? 0x22000000u : 0x30FFFFFFu;
    uint32_t text_color = is_light ? 0xFF121722u : 0xFFF4F6FAu;
    int radius = gui_corner_radius(pill_w, pill_h, pill_h / 2);

    if (pill_x < gui_scaled_metric(4))
        pill_x = gui_scaled_metric(4);
    if (pill_x + pill_w > (int)canvas->width - gui_scaled_metric(4))
        pill_x = (int)canvas->width - gui_scaled_metric(4) - pill_w;
    if (pill_y < gui_scaled_metric(2))
        pill_y = gui_scaled_metric(2);

    gui_fill_rounded_rect(canvas, pill_x, pill_y + 1, pill_w, pill_h, radius, is_light ? 0x12000000u : 0x16000000u);
    gui_fill_rounded_rect(canvas, pill_x, pill_y, pill_w, pill_h, radius, pill_bg);
    gui_draw_rounded_rect(canvas, pill_x, pill_y, pill_w, pill_h, radius, pill_border);
    int text_y = gui_align_text_y(font, pill_y, pill_h);
    gui_draw_text_clipped(canvas, font, pill_x + pad_x, text_y, pill_w - pad_x * 2, label, text_color, pill_bg);
}

static void draw_dock(Surface *canvas, Registry *registry, int hovered_idx)
{
    gui_fill_rect(canvas, 0, 0, (int32_t)canvas->width, (int32_t)canvas->height, 0);

    const int panel_x = shell_dock_panel_x(canvas->width);
    const int panel_y = shell_dock_panel_y();
    const int panel_w = shell_dock_panel_w(canvas->width);
    const int panel_h = shell_dock_panel_h(canvas->height);

    const int radius = dock_panel_radius(panel_w, panel_h);
    bool is_light = registry->theme_mode == GUI_THEME_LIGHT;

    draw_dock_glass(canvas, registry, panel_x, panel_y, panel_w, panel_h, radius);

    int num_icons = k_dock_item_count;
    int icon_size = shell_dock_icon_size();
    int spacing = shell_dock_icon_spacing();
    int total_icons_w = shell_dock_total_icons_w(num_icons);
    int x_ptr = panel_x + (panel_w - total_icons_w) / 2;
    int icon_y = shell_dock_icon_y(canvas->height);

    for (int i = 0; i < k_dock_item_count; i++) {
        bool is_hovered = (i == hovered_idx);
        int open_count = count_windows_for_item(registry, k_dock_items[i]);

        draw_icon_shadow(canvas, x_ptr, icon_y, icon_size);

        if (g_dock_icon_assets[i].loaded) {
            draw_scaled_app_icon(canvas, &g_dock_icon_assets[i].surface, x_ptr, icon_y, icon_size);
            if (strcmp(k_dock_items[i].icon_name, "calendar") == 0) {
                draw_calendar_contents(canvas, x_ptr, icon_y, icon_size);
            }
        } else {
            draw_fallback_icon(canvas, i, x_ptr, icon_y, icon_size);
        }

        if (open_count > 0) {
            bool focused = registry->focused_window >= 2 && registry->focused_window < (int)registry->window_count &&
                           window_title_matches_item(registry->windows[registry->focused_window], k_dock_items[i]);
            int dot_h = shell_dock_indicator_size();
            int dot_w = focused ? gui_scaled_metric(8) : dot_h;
            int dot_y = panel_y + panel_h - gui_scaled_metric(6) - dot_h;
            int dot_x = x_ptr + (icon_size - dot_w) / 2;

            uint32_t dot_color =
                is_light ? (focused ? 0x90000000u : 0x60000000u) : (focused ? 0xD0F2F2F0u : 0x85F2F2F0u);
            gui_fill_rounded_rect(canvas, dot_x, dot_y, dot_w, dot_h, dot_h / 2, dot_color);
        }
        if (is_hovered)
            draw_hover_label(canvas, k_dock_items[i], panel_y, x_ptr, icon_size, is_light);
        x_ptr += icon_size + spacing;
    }
}

static int dock_hit_test_local(int local_mx, int local_my, uint32_t dock_w, uint32_t dock_h)
{
    if (local_my < 0 || local_my > (int)dock_h || local_mx < 0 || local_mx > (int)dock_w)
        return -1;

    int spacing = shell_dock_icon_spacing();
    int icon_size = shell_dock_icon_size();
    int total_icons_w = shell_dock_total_icons_w(k_dock_item_count);
    int x_ptr = shell_dock_panel_x(dock_w) + (shell_dock_panel_w(dock_w) - total_icons_w) / 2;
    int icon_y = shell_dock_icon_y(dock_h);
    int hover_pad = dock_hover_padding();
    int indicator_bottom = shell_dock_indicator_y(dock_h) + shell_dock_indicator_size();

    for (int i = 0; i < k_dock_item_count; i++) {
        if (local_mx >= x_ptr - hover_pad && local_mx <= x_ptr + icon_size + hover_pad &&
            local_my >= icon_y - hover_pad && local_my <= indicator_bottom + hover_pad) {
            return i;
        }
        x_ptr += icon_size + spacing;
    }
    return -1;
}

static int get_hovered_icon(const Registry *reg, uint32_t dock_w, uint32_t dock_h)
{
    if (!reg)
        return -1;
    int local_mx = (int)reg->mouse_x - reg->windows[1].x;
    int local_my = (int)reg->mouse_y - reg->windows[1].y;
    return dock_hit_test_local(local_mx, local_my, dock_w, dock_h);
}

static int get_clicked_icon(const Registry *reg, uint32_t dock_w, uint32_t dock_h)
{
    if (!reg)
        return -1;
    int local_mx = (int)reg->dk_click_x - reg->windows[1].x;
    int local_my = (int)reg->dk_click_y - reg->windows[1].y;
    return dock_hit_test_local(local_mx, local_my, dock_w, dock_h);
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t info[4];
    fb_info(info);
    (void)info[0];
    (void)info[1];

    uint64_t reg_ptr = syscall1(SYS_SHM_MAP, 0);
    while (reg_ptr == (uint64_t)-1 || reg_ptr == 0) {
        sleep_ms(50);
        reg_ptr = syscall1(SYS_SHM_MAP, 0);
    }
    Registry *registry = (Registry *)reg_ptr;

    while (registry->magic != 0x52454749 || registry->dk_shm_id <= 0) {
        asm volatile("" ::: "memory");
        sleep_ms(50);
    }

    gui_apply_theme((registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK);
    load_dock_icons();

    uint64_t shm_bytes = syscall1(SYS_SHM_INFO, (uint64_t)registry->dk_shm_id);
    uint32_t dock_w = registry->dk_width ? registry->dk_width : (uint32_t)shell_dock_window_w(k_dock_item_count);
    if (shm_bytes == (uint64_t)-1 || shm_bytes < (uint64_t)dock_w * 4) {
        shm_bytes = (uint64_t)dock_w * (uint64_t)shell_dock_window_h() * 4;
    }
    uint32_t dock_h = (uint32_t)(shm_bytes / ((uint64_t)dock_w * 4));
    if (dock_h == 0 || dock_h > 128)
        dock_h = (uint32_t)shell_dock_window_h();

    registry->dk_width = dock_w;
    asm volatile("sfence" ::: "memory");

    uint32_t *canvas_ptr = (uint32_t *)syscall1(SYS_SHM_MAP, (uint64_t)registry->dk_shm_id);
    if (!canvas_ptr || canvas_ptr == (uint32_t *)-1)
        return 1;

    uint32_t *local_buffer = (uint32_t *)malloc((size_t)dock_w * dock_h * sizeof(uint32_t));
    if (!local_buffer)
        return 1;
    Surface local_canvas = {local_buffer, dock_w, dock_h, dock_w * 4, true, 0};

    memset(local_buffer, 0, dock_w * dock_h * 4);
    draw_dock(&local_canvas, registry, -1);
    gui_copy_to_canvas((volatile uint32_t *)canvas_ptr, local_buffer, dock_w, dock_h);
    submit_damage(registry->windows[1], dock_full_rect(dock_w, dock_h));

    int last_hovered = -1;
    uint32_t last_sig = dock_visual_signature(registry);
    bool last_theme_dark = (registry->theme_mode != GUI_THEME_LIGHT);
    uint32_t last_blur_generation = registry->dk_blur_generation;

    while (1) {
        bool current_theme_dark = (registry->theme_mode != GUI_THEME_LIGHT);
        if (current_theme_dark != last_theme_dark) {
            gui_apply_theme(current_theme_dark ? GUI_THEME_DARK : GUI_THEME_LIGHT);
            last_theme_dark = current_theme_dark;
            draw_dock(&local_canvas, registry, last_hovered);
            gui_copy_to_canvas((volatile uint32_t *)canvas_ptr, local_buffer, dock_w, dock_h);
            submit_damage(registry->windows[1], dock_full_rect(dock_w, dock_h));
        }

        if (registry->dk_blur_generation != last_blur_generation) {
            last_blur_generation = registry->dk_blur_generation;
            g_last_blur_generation = 0;
            draw_dock(&local_canvas, registry, last_hovered);
            gui_copy_to_canvas((volatile uint32_t *)canvas_ptr, local_buffer, dock_w, dock_h);
            submit_damage(registry->windows[1], dock_full_rect(dock_w, dock_h));
        }

        int hovered = get_hovered_icon(registry, dock_w, dock_h);
        uint32_t sig = dock_visual_signature(registry);
        bool hover_changed = (hovered != last_hovered);
        bool visual_changed = (sig != last_sig);
        if (hover_changed || visual_changed) {
            Rect dirty = dock_full_rect(dock_w, dock_h);
            if (!visual_changed) {
                bool has_dirty = false;
                if (last_hovered >= 0) {
                    dirty = dock_item_damage_rect(last_hovered, dock_w, dock_h);
                    has_dirty = true;
                }
                if (hovered >= 0) {
                    Rect hovered_dirty = dock_item_damage_rect(hovered, dock_w, dock_h);
                    dirty = has_dirty ? gui_rect_union(dirty, hovered_dirty) : hovered_dirty;
                    has_dirty = true;
                }
                if (!has_dirty || !gui_clamp_rect_to_canvas(&dirty, (int)dock_w, (int)dock_h))
                    dirty = dock_full_rect(dock_w, dock_h);
            }
            draw_dock(&local_canvas, registry, hovered);
            if (dirty.x == 0 && dirty.y == 0 && dirty.w == (int)dock_w && dirty.h == (int)dock_h)
                gui_copy_to_canvas((volatile uint32_t *)canvas_ptr, local_buffer, dock_w, dock_h);
            else
                gui_copy_rect_to_canvas((volatile uint32_t *)canvas_ptr, dock_w, local_buffer, dock_w, dirty);
            submit_damage(registry->windows[1], dirty);
            last_hovered = hovered;
            last_sig = sig;
        }

        if (registry->dk_clicked) {
            registry->dk_clicked = false;
            asm volatile("sfence" ::: "memory");

            int clicked_icon = get_clicked_icon(registry, dock_w, dock_h);
            if (clicked_icon != -1) {
                int target_slot = find_window_for_item(registry, k_dock_items[clicked_icon], registry->focused_window);
                if (target_slot >= 2 && target_slot < (int)registry->window_count) {
                    WindowEntry &entry = registry->windows[target_slot];
                    if (entry.state == WIN_MINIMIZED || entry.state == WIN_HIDDEN)
                        entry.request_restore = true;
                    entry.request_focus = true;
                    asm volatile("sfence" ::: "memory");
                } else {
                    int pid = fork();
                    if (pid == 0) {
                        exec(k_dock_items[clicked_icon].path);
                        exit(1);
                    }
                }
            }
        }

        reap_exited_children();
        sleep_ms(16);
    }
}
