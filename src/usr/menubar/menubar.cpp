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

bool g_menu_open = false;
static Surface g_blur_surface = {};
static uint32_t g_last_blur_generation = 0;
static int g_last_blur_shm_id = -1;

static constexpr uint32_t TRANSPARENT_BG = 0x00000000u;

static void reap_exited_children()
{
    int status = 0;
    while (waitpid_nohang(-1, &status) > 0) {
    }
}

static inline uint8_t menubar_scale_alpha_u8(uint8_t alpha, uint8_t coverage)
{
    return (uint8_t)(((uint32_t)alpha * (uint32_t)coverage + 127u) / 255u);
}

static inline uint32_t menubar_blend_pixel(uint32_t dst, uint32_t src, uint8_t coverage)
{
    uint8_t src_alpha = menubar_scale_alpha_u8((uint8_t)(src >> 24), coverage);
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

static void alpha_fill_rect(Surface *canvas, int x, int y, int w, int h, uint32_t color)
{
    if (!canvas || !canvas->buffer || canvas->pitch == 0 || w <= 0 || h <= 0)
        return;

    int64_t left = x;
    int64_t top = y;
    int64_t right = left + (int64_t)w;
    int64_t bottom = top + (int64_t)h;
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right > canvas->width)
        right = canvas->width;
    if (bottom > canvas->height)
        bottom = canvas->height;
    if (right <= left || bottom <= top)
        return;

    x = (int)left;
    y = (int)top;
    w = (int)(right - left);
    h = (int)(bottom - top);

    uint8_t alpha = (uint8_t)(color >> 24);
    if (alpha == 0)
        return;
    if (alpha == 255) {
        gui_fill_rect(canvas, x, y, w, h, color);
        return;
    }

    uint32_t stride = canvas->pitch / 4u;
    for (int py = y; py < y + h; py++) {
        uint32_t *row = &canvas->buffer[(size_t)py * stride];
        for (int px = x; px < x + w; px++) {
            row[px] = menubar_blend_pixel(row[px], color, 255);
        }
    }
}

static bool ensure_blur_surface(Registry *registry, uint32_t screen_w)
{
    auto reset_blur_surface = []() {
        if (g_blur_surface.buffer && gui_shm_id_is_valid(g_last_blur_shm_id))
            syscall1(SYS_SHM_UNMAP, (uint64_t)g_last_blur_shm_id);
        g_blur_surface = {};
        g_last_blur_generation = 0;
        g_last_blur_shm_id = -1;
    };

    if (!registry || !gui_shm_id_is_valid(registry->mb_blur_shm_id) || registry->mb_blur_generation == 0) {
        reset_blur_surface();
        return false;
    }

    uint32_t target_h = (uint32_t)gui_menubar_h();
    bool needs_remap = !g_blur_surface.buffer || g_last_blur_shm_id != registry->mb_blur_shm_id ||
                       g_blur_surface.width != screen_w || g_blur_surface.height != target_h ||
                       g_blur_surface.pitch != screen_w * 4u;
    if (!needs_remap) {
        g_last_blur_generation = registry->mb_blur_generation;
        return true;
    }

    if (g_blur_surface.buffer && gui_shm_id_is_valid(g_last_blur_shm_id)) {
        syscall1(SYS_SHM_UNMAP, (uint64_t)g_last_blur_shm_id);
        g_blur_surface = {};
        g_last_blur_shm_id = -1;
        g_last_blur_generation = 0;
    }

    uint64_t required_bytes = (uint64_t)screen_w * (uint64_t)target_h * 4u;
    uint64_t shm_bytes = syscall1(SYS_SHM_INFO, (uint64_t)registry->mb_blur_shm_id);
    if (shm_bytes == (uint64_t)-1 || shm_bytes < required_bytes) {
        reset_blur_surface();
        return false;
    }

    uint64_t map = syscall1(SYS_SHM_MAP, (uint64_t)registry->mb_blur_shm_id);
    if (map == 0 || map == (uint64_t)-1) {
        reset_blur_surface();
        return false;
    }

    g_blur_surface.buffer = (uint32_t *)map;
    g_blur_surface.width = screen_w;
    g_blur_surface.height = target_h;
    g_blur_surface.pitch = screen_w * 4u;
    g_blur_surface.owns_buffer = false;
    g_last_blur_generation = registry->mb_blur_generation;
    g_last_blur_shm_id = registry->mb_blur_shm_id;
    return true;
}

static constexpr int LOGO_X = 8;
static constexpr int LOGO_Y = 3;
static constexpr int LOGO_W = 56;
static constexpr int LOGO_H = 24;
static constexpr int MENU_X = 8;
static constexpr int MENU_GAP = 7;
static constexpr int MENU_MIN_W = 220;
static constexpr int MENUBAR_CONTENT_X = 72;
static constexpr int MENUBAR_TITLE_MAX_W = 220;

static inline int menubar_h()
{
    return gui_menubar_h();
}
static inline int logo_x()
{
    return gui_scaled_metric(LOGO_X);
}
static inline int logo_y()
{
    return gui_scaled_metric(LOGO_Y);
}
static inline int logo_w()
{
    return gui_scaled_metric(LOGO_W);
}
static inline int logo_h()
{
    return gui_scaled_metric(LOGO_H);
}
static inline int menu_x()
{
    return gui_scaled_metric(MENU_X);
}
static inline int menu_gap()
{
    return gui_scaled_metric(MENU_GAP);
}
static inline int menu_y()
{
    return menubar_h() + menu_gap();
}
static inline int menu_total_h()
{
    return gui_system_menubar_canvas_h();
}
static inline int menubar_content_x()
{
    return gui_scaled_metric(MENUBAR_CONTENT_X);
}
static inline int menubar_title_max_w()
{
    return gui_scaled_metric(MENUBAR_TITLE_MAX_W);
}

static inline int menubar_scaled_offset(int px)
{
    return px < 0 ? -gui_scaled_metric(-px) : gui_scaled_metric(px);
}

static inline int menubar_text_shadow_outset_x()
{
    return gui_scaled_metric(3);
}

static inline uint32_t menubar_text_shadow_color(bool is_light, uint8_t alpha)
{
    uint32_t scaled_alpha = is_light ? ((uint32_t)alpha * 2u + 2u) / 3u : (uint32_t)alpha;
    if (scaled_alpha > 255u)
        scaled_alpha = 255u;
    return scaled_alpha << 24;
}

struct MenubarTextShadowPass
{
    int8_t dx;
    int8_t dy;
    uint8_t alpha;
};

static constexpr MenubarTextShadowPass k_menubar_text_shadow_passes[] = {
    {-1, 1, 8}, {0, 1, 12}, {1, 1, 8}, {-1, 2, 7}, {0, 2, 10}, {1, 2, 7},
    {-2, 3, 4}, {-1, 3, 5}, {0, 3, 6}, {1, 3, 5},  {2, 3, 4},  {0, 4, 3},
};

static void draw_menubar_text_shadow(Surface *canvas, const GuiFont *font, int32_t x, int32_t y, const char *text,
                                     bool is_light)
{
    if (!canvas || !font || !text || text[0] == '\0')
        return;

    for (size_t i = 0; i < sizeof(k_menubar_text_shadow_passes) / sizeof(k_menubar_text_shadow_passes[0]); i++) {
        const MenubarTextShadowPass &pass = k_menubar_text_shadow_passes[i];
        gui_draw_text(canvas, font, x + menubar_scaled_offset(pass.dx), y + menubar_scaled_offset(pass.dy), text,
                      menubar_text_shadow_color(is_light, pass.alpha), TRANSPARENT_BG);
    }
}

static void draw_menubar_text_shadow_clipped(Surface *canvas, const GuiFont *font, int32_t x, int32_t y,
                                             int32_t max_width, const char *text, bool is_light)
{
    if (!canvas || !font || !text || text[0] == '\0' || max_width <= 0)
        return;

    for (size_t i = 0; i < sizeof(k_menubar_text_shadow_passes) / sizeof(k_menubar_text_shadow_passes[0]); i++) {
        const MenubarTextShadowPass &pass = k_menubar_text_shadow_passes[i];
        gui_draw_text_clipped(canvas, font, x + menubar_scaled_offset(pass.dx), y + menubar_scaled_offset(pass.dy),
                              max_width, text, menubar_text_shadow_color(is_light, pass.alpha), TRANSPARENT_BG);
    }
}

static void draw_menubar_text(Surface *canvas, const GuiFont *font, int32_t x, int32_t y, const char *text, uint32_t fg,
                              bool is_light)
{
    draw_menubar_text_shadow(canvas, font, x, y, text, is_light);
    gui_draw_text(canvas, font, x, y, text, fg, TRANSPARENT_BG);
}

static void draw_menubar_text_clipped(Surface *canvas, const GuiFont *font, int32_t x, int32_t y, int32_t max_width,
                                      const char *text, uint32_t fg, bool is_light)
{
    draw_menubar_text_shadow_clipped(canvas, font, x, y, max_width, text, is_light);
    gui_draw_text_clipped(canvas, font, x, y, max_width, text, fg, TRANSPARENT_BG);
}

static Rect menubar_focus_dirty_rect(uint32_t screen_w)
{
    int left = menubar_content_x() - menubar_text_shadow_outset_x();
    int right = (int)screen_w - gui_scaled_metric(220) - gui_scaled_metric(24) + menubar_text_shadow_outset_x();
    if (right < left)
        right = left;
    return {left, 0, right - left, menubar_h()};
}

struct FocusedWindowInfo
{
    int slot;
    uint32_t state;
    char title[64];
    bool valid;
};

struct SystemMenuModel
{
    GuiMenuItem items[9];
    char close_item[96];
    char mini_item[96];
    char max_item[96];
    int count;
    int width;
    int height;
};

static void clear_rect(Surface *canvas, const Rect &r)
{
    gui_fill_rect(canvas, r.x, r.y, r.w, r.h, 0x00000000);
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

static int pointer_local_x(Registry *reg)
{
    return (int)reg->mouse_x - reg->windows[0].x;
}
static int pointer_local_y(Registry *reg)
{
    return (int)reg->mouse_y - reg->windows[0].y;
}
static int click_local_x(Registry *reg)
{
    return (int)reg->mb_click_x - reg->windows[0].x;
}
static int click_local_y(Registry *reg)
{
    return (int)reg->mb_click_y - reg->windows[0].y;
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

static bool window_entry_title_equals(const WindowEntry *entry, const char *title)
{
    if (!entry || !title)
        return false;

    size_t i = 0;
    for (; i < sizeof(entry->title); i++) {
        char entry_ch = entry->title[i];
        char title_ch = title[i];
        if (entry_ch != title_ch)
            return false;
        if (entry_ch == '\0')
            return true;
    }
    return title[i] == '\0';
}

static WindowEntry *window_entry_by_slot(Registry *reg, int slot, bool allow_hidden = false)
{
    if (!reg || slot < 2 || slot >= (int)reg->window_count || slot >= MAX_WINDOWS)
        return nullptr;
    WindowEntry *entry = &reg->windows[slot];
    if (allow_hidden ? !window_entry_present(entry) : !window_entry_usable(entry))
        return nullptr;
    return entry;
}

static bool snapshot_focused_window(Registry *reg, FocusedWindowInfo *out)
{
    if (!out)
        return false;
    out->slot = -1;
    out->state = 0xFFFFFFFFu;
    out->title[0] = '\0';
    out->valid = false;
    if (!reg)
        return false;

    int slot = reg->focused_window;
    WindowEntry *entry = window_entry_by_slot(reg, slot);
    if (!entry)
        return false;

    out->slot = slot;
    out->state = entry->state;
    strncpy(out->title, entry->title, sizeof(out->title) - 1);
    out->title[sizeof(out->title) - 1] = '\0';
    out->valid = true;
    return true;
}

static SystemMenuModel build_system_menu_model(Registry *reg)
{
    SystemMenuModel model = {};
    FocusedWindowInfo focus = {};
    snapshot_focused_window(reg, &focus);

    if (focus.valid) {
        snprintf(model.close_item, sizeof(model.close_item), "Close %s", focus.title);
        snprintf(model.mini_item, sizeof(model.mini_item), "Minimize %s", focus.title);
        snprintf(model.max_item, sizeof(model.max_item), "%s %s", focus.state == WIN_MAXIMIZED ? "Restore" : "Maximize",
                 focus.title);
    } else {
        snprintf(model.close_item, sizeof(model.close_item), "No Active Window");
        snprintf(model.mini_item, sizeof(model.mini_item), "No Active Window");
        snprintf(model.max_item, sizeof(model.max_item), "No Active Window");
    }

    auto push = [&](const char *label, bool enabled, bool separator) {
        if (model.count >= (int)(sizeof(model.items) / sizeof(model.items[0])))
            return;
        model.items[model.count++] = {label, enabled, separator};
    };

    push("About uniOS", true, false);
    push("Settings", true, false);
    push(nullptr, false, true);
    push(model.close_item, focus.valid, false);
    push(model.mini_item, focus.valid, false);
    push(model.max_item, focus.valid, false);
    push(nullptr, false, true);
    push("Restart", true, false);
    push("Shut Down", true, false);

    model.width = gui_popup_menu_width(model.items, model.count, gui_scaled_metric(MENU_MIN_W));
    model.height = gui_popup_menu_height(model.items, model.count);
    return model;
}

static void request_window_action(Registry *reg, int slot, int action)
{
    WindowEntry *entry = window_entry_by_slot(reg, slot, true);
    if (!entry)
        return;

    if (action == 0)
        entry->request_close = true;
    else if (action == 1)
        entry->request_minimize = true;
    else if (action == 2) {
        if (entry->state == WIN_MAXIMIZED)
            entry->request_restore = true;
        else
            entry->request_maximize = true;
    }
    asm volatile("sfence" ::: "memory");
}

static WindowEntry *find_window_by_title(Registry *reg, const char *title)
{
    uint32_t count = reg->window_count;
    if (count > MAX_WINDOWS)
        count = MAX_WINDOWS;

    WindowEntry *first_hidden_match = nullptr;
    for (uint32_t i = 2; i < count; i++) {
        WindowEntry *entry = &reg->windows[i];
        if (!window_entry_present(entry))
            continue;
        if (!window_entry_title_equals(entry, title))
            continue;
        if (entry->state != WIN_HIDDEN)
            return entry;
        if (!first_hidden_match)
            first_hidden_match = entry;
    }
    return first_hidden_match;
}

static void launch_about(Registry *reg)
{
    WindowEntry *entry = find_window_by_title(reg, "About uniOS");
    if (entry) {
        if (entry->state == WIN_MINIMIZED || entry->state == WIN_HIDDEN)
            entry->request_restore = true;
        entry->request_focus = true;
        asm volatile("sfence" ::: "memory");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        exec("/bin/about.elf");
        exit(1);
    }
}

static void launch_preferences(Registry *reg)
{
    WindowEntry *entry = find_window_by_title(reg, "Settings");
    if (entry) {
        if (entry->state == WIN_MINIMIZED || entry->state == WIN_HIDDEN)
            entry->request_restore = true;
        entry->request_focus = true;
        asm volatile("sfence" ::: "memory");
        return;
    }
    int pid = fork();
    if (pid == 0) {
        exec("/bin/preferences.elf");
        exit(1);
    }
}

static void request_system_power_action(Registry *reg, int syscall_number)
{
    if (reg) {
        g_menu_open = false;
        reg->mb_clicked = false;
        reg->mb_menu_dismiss_requested = false;
        asm volatile("sfence" ::: "memory");
    }
    syscall0(syscall_number);
}

void draw_menu(Surface *canvas, Registry *reg, int mx, int my)
{
    SystemMenuModel model = build_system_menu_model(reg);
    int menu_x0 = menu_x();
    int menu_y0 = menu_y();
    int hovered_index = gui_popup_menu_hit_test(model.items, model.count, menu_x0, menu_y0, model.width, mx, my);
    gui_draw_popup_menu(canvas, menu_x0, menu_y0, model.width, model.items, model.count, hovered_index);
}

void draw_menubar(Surface *canvas, Registry *reg)
{
    int bar_h = menubar_h();
    int logo_x0 = logo_x();
    int logo_w0 = logo_w();
    const GuiFont *app_font = gui_font_title();
    const GuiFont *menu_font = gui_font_default();
    bool blur_ready = ensure_blur_surface(reg, canvas->width);

    bool is_light = reg->theme_mode == GUI_THEME_LIGHT;

    uint32_t tint_color = is_light ? 0x70F7F9FCu : 0x60141820u;
    uint32_t divider_color = is_light ? 0x16000000u : 0x14FFFFFFu;
    uint32_t fallback_color = is_light ? 0xFFF7F9FCu : 0xFF141820u;

    if (blur_ready) {
        gui_blit_rect(canvas, &g_blur_surface, 0, 0, 0, 0, canvas->width, bar_h);
    } else {
        gui_fill_rect(canvas, 0, 0, canvas->width, bar_h, fallback_color);
    }

    alpha_fill_rect(canvas, 0, 0, canvas->width, bar_h, tint_color);
    gui_fill_rect(canvas, 0, bar_h - 1, canvas->width, 1, divider_color);

    int app_text_y = gui_align_text_y(app_font, 0, bar_h);
    int menu_text_y = gui_align_text_y(menu_font, 0, bar_h);
    uint32_t text_color = g_gui_style.text;

    bool logo_hot = false;
    if (reg) {
        int mx = pointer_local_x(reg);
        int my = pointer_local_y(reg);
        logo_hot =
            g_menu_open || (mx >= logo_x0 && mx < logo_x0 + logo_w0 && my >= logo_y() && my < logo_y() + logo_h());
    }
    if (logo_hot) {
        uint32_t logo_fill = is_light ? 0x1E000000u : 0x22FFFFFFu;
        gui_fill_rounded_rect(canvas, logo_x0, logo_y(), logo_w0, logo_h(), gui_scaled_metric(6), logo_fill);
    }
    int text_w = gui_measure_text(app_font, "uniOS");
    int center_x = logo_x0 + (logo_w0 - text_w) / 2;
    draw_menubar_text_clipped(canvas, app_font, center_x, app_text_y, logo_w0, "uniOS", text_color, is_light);

    int x = menubar_content_x();
    FocusedWindowInfo focus = {};
    if (snapshot_focused_window(reg, &focus)) {
        int title_w = gui_measure_text(app_font, focus.title);
        if (title_w > menubar_title_max_w())
            title_w = menubar_title_max_w();
        draw_menubar_text_clipped(canvas, app_font, x, app_text_y, menubar_title_max_w(), focus.title, g_gui_style.text,
                                  is_light);
        x += title_w + gui_scaled_metric(24);
    }

    const char *menus[] = {"File", "Edit", "View", "Go", "Window", "Help"};
    for (int i = 0; i < 6; i++) {
        draw_menubar_text(canvas, menu_font, x, menu_text_y, menus[i], g_gui_style.text, is_light);
        x += gui_measure_text(menu_font, menus[i]) + gui_scaled_metric(22);
    }

    struct SysTime t;
    if (get_time(&t) == 0) {
        char time_str[64];
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const char *month_name = (t.month >= 1 && t.month <= 12) ? months[t.month - 1] : "???";

        if ((reg->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0) {
            snprintf(time_str, sizeof(time_str), "%s %d  %02d:%02d:%02d", month_name, t.day, t.hour, t.minute,
                     t.second);
        } else {
            snprintf(time_str, sizeof(time_str), "%s %d  %02d:%02d", month_name, t.day, t.hour, t.minute);
        }
        uint32_t text_w = (uint32_t)gui_measure_text(menu_font, time_str);
        int time_x = canvas->width - (int)text_w - gui_scaled_metric(18);
        draw_menubar_text(canvas, menu_font, time_x, menu_text_y, time_str, g_gui_style.text, is_light);
    }

    if (g_menu_open)
        draw_menu(canvas, reg, pointer_local_x(reg), pointer_local_y(reg));
}

int get_hovered_item(Registry *reg)
{
    int mx = pointer_local_x(reg);
    int my = pointer_local_y(reg);

    if (g_menu_open) {
        SystemMenuModel model = build_system_menu_model(reg);
        return gui_popup_menu_hit_test(model.items, model.count, menu_x(), menu_y(), model.width, mx, my);
    } else {
        if (mx >= logo_x() && mx < logo_x() + logo_w() && my >= logo_y() && my < logo_y() + logo_h()) {
            return 99; // uniOS Logo
        }
    }
    return -1;
}

extern "C" int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint32_t info[4];
    fb_info(info);
    uint32_t screen_w = info[0];
    int canvas_h = gui_system_menubar_canvas_h();

    uint64_t reg_ptr = syscall1(SYS_SHM_MAP, 0);
    while (reg_ptr == (uint64_t)-1 || reg_ptr == 0) {
        sleep_ms(50);
        reg_ptr = syscall1(SYS_SHM_MAP, 0);
    }
    Registry *registry = (Registry *)reg_ptr;

    while (registry->magic != 0x52454749 || registry->mb_shm_id <= 0) {
        asm volatile("" ::: "memory");
        sleep_ms(50);
    }

    gui_apply_theme((registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK);

    uint64_t canvas_map = syscall1(SYS_SHM_MAP, (uint64_t)registry->mb_shm_id);
    if (canvas_map == 0 || canvas_map == (uint64_t)-1)
        return 1;
    uint32_t *canvas_ptr = (uint32_t *)canvas_map;

    uint32_t *local_buffer = (uint32_t *)malloc((size_t)screen_w * (size_t)canvas_h * sizeof(uint32_t));
    if (!local_buffer)
        return 1;
    Surface local_canvas = {local_buffer, screen_w, (uint32_t)canvas_h, screen_w * 4, true};

    registry->windows[0].x = 0;
    registry->windows[0].y = 0;
    registry->windows[0].w = (int)screen_w;
    registry->windows[0].h = menubar_h();
    asm volatile("sfence" ::: "memory");

    memset(local_buffer, 0, (size_t)screen_w * (size_t)canvas_h * sizeof(uint32_t));
    draw_menubar(&local_canvas, registry);

    gui_copy_to_canvas((volatile uint32_t *)canvas_ptr, local_buffer, screen_w, (uint32_t)menubar_h());
    submit_damage(registry->windows[0], {0, 0, (int)screen_w, menubar_h()});

    int last_min = -1;
    int last_hovered = -1;
    bool last_menu_open = g_menu_open;
    int last_focused_slot = -2;
    uint32_t last_focused_state = 0xFFFFFFFFu;
    uint32_t last_settings_generation = registry->settings_generation;
    uint32_t last_wallpaper_generation = registry->mb_blur_generation;
    char last_focused_title[64];
    memset(last_focused_title, 0, sizeof(last_focused_title));

    while (true) {
        int current_hovered = get_hovered_item(registry);
        bool hover_changed = (current_hovered != last_hovered);

        struct SysTime t;
        bool have_time = (get_time(&t) == 0);
        bool time_changed = false;
        if (have_time) {
            time_changed = ((registry->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0) ? (t.second != last_min)
                                                                                            : (t.minute != last_min);
        }
        FocusedWindowInfo focus = {};
        snapshot_focused_window(registry, &focus);
        int focused_slot = focus.valid ? focus.slot : -1;
        uint32_t focused_state = focus.valid ? focus.state : 0xFFFFFFFFu;
        bool focus_changed = (focused_slot != last_focused_slot) || (focused_state != last_focused_state) ||
                             (focus.valid && strcmp(last_focused_title, focus.title) != 0) ||
                             (!focus.valid && last_focused_title[0] != '\0');

        bool clicked = registry->mb_clicked;
        bool dismiss_requested = registry->mb_menu_dismiss_requested;
        if (dismiss_requested) {
            g_menu_open = false;
            registry->mb_menu_dismiss_requested = false;
            asm volatile("sfence" ::: "memory");
        }
        if (clicked) {
            if (g_menu_open) {
                SystemMenuModel model = build_system_menu_model(registry);
                int mx = click_local_x(registry);
                int my = click_local_y(registry);
                int item = gui_popup_menu_hit_test(model.items, model.count, menu_x(), menu_y(), model.width, mx, my);
                if (item >= 0) {
                    if (item == 0)
                        launch_about(registry);
                    else if (item == 1)
                        launch_preferences(registry);
                    else if (item == 3)
                        request_window_action(registry, focused_slot, 0);
                    else if (item == 4)
                        request_window_action(registry, focused_slot, 1);
                    else if (item == 5)
                        request_window_action(registry, focused_slot, 2);
                    else if (item == 7)
                        request_system_power_action(registry, SYS_REBOOT);
                    else if (item == 8)
                        request_system_power_action(registry, SYS_POWEROFF);
                    g_menu_open = false;
                } else {
                    g_menu_open = false;
                }
            } else {
                int mx = click_local_x(registry);
                int my = click_local_y(registry);
                if (mx >= logo_x() && mx < logo_x() + logo_w() && my >= logo_y() && my < logo_y() + logo_h()) {
                    g_menu_open = true;
                }
            }
            registry->mb_clicked = false;
            asm volatile("sfence" ::: "memory");
        }

        bool theme_changed = false;
        if (registry->settings_generation != last_settings_generation) {
            last_settings_generation = registry->settings_generation;
            gui_sync_theme_from_registry();
            theme_changed = true;
            hover_changed = true;
            time_changed = true;
        }
        bool blur_changed = false;
        if (registry->mb_blur_generation != last_wallpaper_generation) {
            last_wallpaper_generation = registry->mb_blur_generation;
            g_last_blur_generation = 0;
            blur_changed = true;
        }

        if (clicked || theme_changed || blur_changed || time_changed || hover_changed ||
            g_menu_open != last_menu_open || focus_changed) {
            int target_h = g_menu_open ? menu_total_h() : menubar_h();
            if (registry->windows[0].h != target_h) {
                registry->windows[0].h = target_h;
                asm volatile("sfence" ::: "memory");
            }
            Rect dirty = {0, 0, 0, 0};
            bool has_dirty = false;
            bool menu_state_changed = (g_menu_open != last_menu_open);

            if (hover_changed) {
                SystemMenuModel model = build_system_menu_model(registry);
                Rect hover_rect = g_menu_open ? Rect{menu_x(), menu_y(), model.width, model.height}
                                              : Rect{logo_x(), logo_y(), logo_w(), logo_h()};
                dirty = hover_rect;
                has_dirty = true;
            }
            if (clicked || menu_state_changed) {
                SystemMenuModel model = build_system_menu_model(registry);
                Rect menu_rect = gui_rect_union({logo_x(), logo_y(), logo_w(), logo_h()},
                                                {menu_x(), menu_y(), model.width, model.height});
                dirty = has_dirty ? gui_rect_union(dirty, menu_rect) : menu_rect;
                has_dirty = true;
            }
            if (time_changed) {
                Rect time_rect = {(int)screen_w - gui_scaled_metric(220), 0, gui_scaled_metric(220), menubar_h()};
                dirty = has_dirty ? gui_rect_union(dirty, time_rect) : time_rect;
                has_dirty = true;
            }
            if (focus_changed) {
                Rect focus_rect = menubar_focus_dirty_rect(screen_w);
                dirty = has_dirty ? gui_rect_union(dirty, focus_rect) : focus_rect;
                has_dirty = true;
                if (g_menu_open) {
                    SystemMenuModel model = build_system_menu_model(registry);
                    Rect menu_rect = {menu_x(), menu_y(), model.width, model.height};
                    dirty = gui_rect_union(dirty, menu_rect);
                }
            }
            if (theme_changed || blur_changed) {
                dirty = {0, 0, (int)screen_w, target_h};
                has_dirty = true;
            }
            if (!has_dirty) {
                dirty = {0, 0, (int)screen_w, target_h};
            }
            if (!gui_clamp_rect_to_canvas(&dirty, (int)screen_w, canvas_h)) {
                dirty = {0, 0, (int)screen_w, menubar_h()};
            }

            clear_rect(&local_canvas, dirty);
            draw_menubar(&local_canvas, registry);

            gui_copy_rect_to_canvas((volatile uint32_t *)canvas_ptr, screen_w, local_buffer, screen_w, dirty);
            submit_damage(registry->windows[0], dirty);

            if (time_changed && have_time) {
                last_min = ((registry->system_flags & SYSTEM_FLAG_CLOCK_SHOW_SECONDS) != 0) ? t.second : t.minute;
            }
            last_hovered = current_hovered;
            last_menu_open = g_menu_open;
            last_focused_slot = focused_slot;
            last_focused_state = focused_state;
            if (focus.valid) {
                strncpy(last_focused_title, focus.title, sizeof(last_focused_title) - 1);
                last_focused_title[sizeof(last_focused_title) - 1] = '\0';
            } else {
                last_focused_title[0] = '\0';
            }
        }
        reap_exited_children();
        if (g_menu_open || current_hovered != -1)
            sleep_ms(4);
        else
            sleep_ms(20);
    }
    return 0;
}
