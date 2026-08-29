#include "wm_input.h"
#include "wm_metrics.h"
#include "wm_present.h"
#include "wm_render.h"
#include "wm_settings.h"
#include "wm_window.h"

static void gui_fill_rounded_rect_clipped(Surface *dst, int x, int y, int w, int h, int r, uint32_t color,
                                          const DirtyRect &clip)
{
    int ix, iy, iw, ih;
    if (!gui_intersect_rect(x, y, w, h, clip.x, clip.y, clip.w, clip.h, &ix, &iy, &iw, &ih))
        return;
    if (rect_contains(clip, {x, y, w, h})) {
        gui_fill_rounded_rect(dst, x, y, w, h, r, color);
    } else {
        gui_fill_rect(dst, ix, iy, iw, ih, color);
    }
}

static void gui_draw_rounded_rect_clipped(Surface *dst, int x, int y, int w, int h, int r, uint32_t color,
                                          const DirtyRect &clip)
{
    int ix, iy, iw, ih;
    if (!gui_intersect_rect(x, y, w, h, clip.x, clip.y, clip.w, clip.h, &ix, &iy, &iw, &ih))
        return;
    gui_draw_rounded_rect(dst, x, y, w, h, r, color);
}

static Surface g_icon_close = {};
static Surface g_icon_minimize = {};
static Surface g_icon_maximize = {};
static int g_icons_scale = -1;

static void scale_surface_alpha(Surface *s, uint8_t scale)
{
    if (!s || !s->buffer || scale == 255)
        return;

    const uint32_t stride = s->pitch / 4;
    for (uint32_t y = 0; y < s->height; ++y) {
        uint32_t *row = &s->buffer[y * stride];
        for (uint32_t x = 0; x < s->width; ++x) {
            uint32_t p = row[x];
            uint8_t a = scale_alpha_u8(static_cast<uint8_t>(p >> 24), scale);
            row[x] = (static_cast<uint32_t>(a) << 24) | (p & 0x00FFFFFFu);
        }
    }
}

static void ensure_button_icons()
{
    int scale = gui_ui_scale_pct();
    if (g_icons_scale == scale && g_icon_close.buffer)
        return;

    if (g_icon_close.buffer)
        gui_destroy_surface(&g_icon_close);
    if (g_icon_minimize.buffer)
        gui_destroy_surface(&g_icon_minimize);
    if (g_icon_maximize.buffer)
        gui_destroy_surface(&g_icon_maximize);

    gui_load_uoic("/usr/share/wm/close.uoic", static_cast<uint32_t>(BTN_SIZE), static_cast<uint32_t>(scale),
                  &g_icon_close);
    gui_load_uoic("/usr/share/wm/minimize.uoic", static_cast<uint32_t>(BTN_SIZE), static_cast<uint32_t>(scale),
                  &g_icon_minimize);
    gui_load_uoic("/usr/share/wm/maximize.uoic", static_cast<uint32_t>(BTN_SIZE), static_cast<uint32_t>(scale),
                  &g_icon_maximize);

    scale_surface_alpha(&g_icon_close, 166);
    scale_surface_alpha(&g_icon_minimize, 166);
    scale_surface_alpha(&g_icon_maximize, 166);

    g_icons_scale = scale;
}

void invalidate_window_decoration_cache(Window &w)
{
    w.decoration_cache_theme_sig = 0;
    w.button_cache_theme_sig = 0;
    w.decoration_cache_w = 0;
    w.decoration_cache_h = 0;
}

uint32_t get_window_app_background(const Window &w)
{
    // During a resize the live backing may be a freshly mmap'd buffer the
    // client hasn't written yet (zeroed pixels). Sample from the snapshot
    // instead — it holds the last committed frame the compositor presents.
    const uint32_t *src = w.buffer;
    int src_w = w.buffer_w;
    int src_h = w.buffer_h;
    if (w.resize_configure_pending && w.resize_snapshot.buffer) {
        src = w.resize_snapshot.buffer;
        src_w = static_cast<int>(w.resize_snapshot.width);
        src_h = static_cast<int>(w.resize_snapshot.height);
    }
    if (src && src_w > 0 && src_h > 0) {
        int sample_y = src_h > 4 ? 4 : 0;
        int sample_x = src_w > 10 ? 10 : 0;
        uint32_t pixel = src[(size_t)sample_y * (size_t)src_w + (size_t)sample_x];
        if ((pixel >> 24) != 0) {
            return 0xFF000000u | (pixel & 0x00FFFFFFu);
        }
    }
    return g_gui_style.app_bg ? g_gui_style.app_bg : g_gui_style.app_surface;
}

static bool is_color_dark(uint32_t color)
{
    return color_luma(color) < 128;
}

static uint32_t window_decoration_theme_signature(const Window &w)
{
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t value) {
        sig ^= value;
        sig *= 16777619u;
    };
    mix(get_window_app_background(w));
    mix(g_gui_style.border);
    mix(g_gui_style.border_focus);
    mix(g_gui_style.border_hover);
    mix(g_gui_chrome.window_bar_active);
    mix(g_gui_chrome.window_bar_inactive);
    mix(g_gui_chrome.window_bar_hover);
    mix(g_gui_chrome.window_title_active);
    mix(g_gui_chrome.window_title_inactive);
    mix(g_gui_chrome.frame_shadow);
    mix(g_gui_chrome.frame_outline);
    mix(static_cast<uint32_t>(wm_title_bar_h()));
    mix(static_cast<uint32_t>(wm_button_size()));
    mix(static_cast<uint32_t>(wm_button_inset_x()));
    mix(static_cast<uint32_t>(wm_button_inset_y()));
    mix(static_cast<uint32_t>(wm_button_spacing()));
    mix(static_cast<uint32_t>(gui_scaled_metric(12)));
    mix(static_cast<uint32_t>(wm_frame_border()));
    mix(static_cast<uint32_t>(wm_frame_shadow_offset_x()));
    mix(static_cast<uint32_t>(wm_frame_shadow_offset_y()));
    mix(static_cast<uint32_t>(gui_scaled_metric(1)));
    return sig;
}

static void draw_window_decoration_frame(Surface *dst, const Window &w, const DirtyRect &clip, bool focused)
{
    if (w.transparent)
        return;

    int title_bar_h = wm_title_bar_h();
    int space_1 = gui_space_1();
    int space_2 = gui_space_2();
    int border = gui_chrome_border();
    int detail_inset = gui_chrome_detail_inset();

    int radius = gui_radius_xl();
    int body_inset = border + detail_inset;
    int frame_radius = radius - border;
    if (frame_radius < 0)
        frame_radius = 0;

    int body_radius = radius - body_inset;
    if (body_radius < 0)
        body_radius = 0;

    uint32_t app_bg_color = get_window_app_background(w);
    uint32_t bar_color = app_bg_color;
    uint32_t body_color = app_bg_color;
    uint32_t title_color;
    if (is_color_dark(app_bg_color)) {
        title_color = focused ? 0xFFF2F2F0u : 0xFF9A9FA7u;
    } else {
        title_color = focused ? 0xFF15181Du : 0xFF6E7580u;
    }
    // Same outline recipe every floating surface uses (gui_draw_chrome_frame).
    GuiChromeFrameColors chrome = gui_chrome_frame_colors(body_color, focused);
    uint32_t outline_color = chrome.outline;
    uint32_t frame_fill_color = chrome.frame_fill;
    uint32_t inner_stroke_color = chrome.inner_stroke;

    int lx = (dst->buffer != g_backbuffer.buffer) ? 0 : w.x;
    int ly = (dst->buffer != g_backbuffer.buffer) ? 0 : w.y - title_bar_h;
    int sx = lx, sy = ly, sw = w.w, sh = w.h + title_bar_h;

    // Multi-layered soft drop shadow: same alpha stack as the shared panel
    // shadow (gui_draw_panel_shadow), fit inside the outer bounds.
    if (focused) {
        gui_fill_rounded_rect_clipped(dst, sx + gui_scaled_metric(1), sy + gui_scaled_metric(3), sw, sh,
                                      radius + gui_scaled_metric(2), 0x08000000u, clip);
        gui_fill_rounded_rect_clipped(dst, sx + gui_scaled_metric(1), sy + gui_scaled_metric(2), sw, sh,
                                      radius + gui_scaled_metric(1), 0x0C000000u, clip);
        gui_fill_rounded_rect_clipped(dst, sx, sy + gui_scaled_metric(1), sw, sh, radius, 0x10000000u, clip);
    } else {
        gui_fill_rounded_rect_clipped(dst, sx, sy + gui_scaled_metric(2), sw, sh, radius + gui_scaled_metric(1),
                                      0x04000000u, clip);
        gui_fill_rounded_rect_clipped(dst, sx, sy + gui_scaled_metric(1), sw, sh, radius, 0x06000000u, clip);
    }

    gui_fill_rounded_rect_clipped(dst, sx, sy, sw, sh, radius, outline_color, clip);
    if (sw > border * 2 && sh > border * 2) {
        gui_fill_rounded_rect_clipped(dst, sx + border, sy + border, sw - border * 2, sh - border * 2, frame_radius,
                                      frame_fill_color, clip);
    }

    if (sw > body_inset * 2 && sh > body_inset * 2) {
        gui_fill_rounded_rect_clipped(dst, sx + body_inset, sy + body_inset, sw - body_inset * 2, sh - body_inset * 2,
                                      body_radius, body_color, clip);
        gui_draw_rounded_rect_clipped(dst, sx + border, sy + border, sw - border * 2, sh - border * 2, frame_radius,
                                      inner_stroke_color, clip);
    }

    int title_fill_x = sx + border;
    int title_fill_y = sy + border;
    int title_fill_w = sw - border * 2;
    int title_fill_h = title_bar_h;

    if (title_fill_w > 0 && title_fill_h > 0) {
        int title_radius = radius - border;
        if (title_radius < 0)
            title_radius = 0;
        if (title_radius > title_fill_w / 2)
            title_radius = title_fill_w / 2;
        if (title_radius > title_fill_h)
            title_radius = title_fill_h;
        fill_top_rounded_rect_clipped(dst, title_fill_x, title_fill_y, title_fill_w, title_fill_h, title_radius,
                                      bar_color);
    }

    const GuiFont *title_font = gui_font_title();
    int title_h = gui_font_line_height(title_font);
    int title_y = gui_align_text_y(title_font, sy + border, title_bar_h - border);
    DirtyRect last_button = window_button_bounds(w, 2);
    int buttons_right = last_button.x + last_button.w;
    int title_left = buttons_right + space_1;
    int title_right = sx + w.w - space_2;
    int available_w = title_right - title_left;

    if (available_w > 0) {
        int raw_title_w = gui_measure_text(title_font, w.title);
        int centered_x;
        if (raw_title_w >= available_w) {
            centered_x = title_left;
        } else {
            centered_x = sx + (w.w - raw_title_w) / 2;
            if (centered_x < title_left)
                centered_x = title_left;
            else if (centered_x + raw_title_w > title_right)
                centered_x = title_right - raw_title_w;
        }

        int ix, iy, iw, ih;
        if (gui_intersect_rect(clip.x, clip.y, clip.w, clip.h, centered_x, title_y, available_w, title_h, &ix, &iy, &iw,
                               &ih)) {
            gui_draw_text_rect_clipped(dst, title_font, centered_x, title_y, available_w, clip.x, clip.y, clip.w,
                                       clip.h, w.title, title_color, bar_color);
        }
    }
}

static void draw_window_decoration_buttons_to(Surface *dst, const Window &w, int origin_x, int origin_y,
                                              const DirtyRect *clip, bool focused, int hovered_button)
{
    if (w.transparent)
        return;

    ensure_button_icons();

    uint32_t bar_color = get_window_app_background(w);
    uint32_t button_colors[3] = {g_gui_chrome.button_close, g_gui_chrome.button_minimize, g_gui_chrome.button_maximize};
    uint32_t button_outline = focused ? 0x65000000u : 0x38000000u;
    int button_size = wm_button_size();
    int r = button_size / 2;

    Surface *icons[3] = {&g_icon_close, &g_icon_minimize, &g_icon_maximize};

    for (int i = 0; i < 3; i++) {
        int cx = 0, cy = 0;
        window_button_center(w, i, &cx, &cy);
        if (clip &&
            (cx - r >= clip->x + clip->w || cx + r <= clip->x || cy - r >= clip->y + clip->h || cy + r <= clip->y)) {
            continue;
        }
        cx -= origin_x;
        cy -= origin_y;

        uint32_t button_fill = focused ? button_colors[i] : mix_rgb(button_colors[i], bar_color, 138);
        if (hovered_button == i) {
            button_fill = 0xFF000000u | (mix_rgb(button_colors[i], 0xFFFFFFFFu, focused ? 22 : 16) & 0x00FFFFFFu);
        }

        gui_fill_circle(dst, cx, cy, r, button_fill);
        gui_draw_circle_stroke(dst, cx, cy, r, 1, button_outline);

        if (icons[i]->buffer) {
            int ix = cx - static_cast<int>(icons[i]->width) / 2;
            int iy = cy - static_cast<int>(icons[i]->height) / 2;
            gui_blit_alpha(dst, icons[i], ix, iy);
        }
    }
}

static void draw_window_decoration_buttons(Surface *dst, const Window &w, bool focused, int hovered_button)
{
    DirtyRect b0 = window_button_bounds(w, 0);
    draw_window_decoration_buttons_to(dst, w, b0.x, b0.y, nullptr, focused, hovered_button);
}

static void draw_window_decoration_buttons_clipped(Surface *dst, const Window &w, const DirtyRect &clip, bool focused,
                                                   int hovered_button)
{
    draw_window_decoration_buttons_to(dst, w, 0, 0, &clip, focused, hovered_button);
}

static void ensure_window_decoration_cache(Window &w, bool focused, bool hovered_frame, int hovered_button)
{
    (void)hovered_frame;
    if (w.transparent)
        return;

    DirtyRect outer = window_outer_bounds(w);
    uint32_t theme_sig = window_decoration_theme_signature(w);

    bool frame_needs_rebuild = !w.decoration_cache.buffer || w.decoration_cache_w != outer.w ||
                               w.decoration_cache_h != outer.h || w.decoration_cache_theme_sig != theme_sig ||
                               w.decoration_cache_focused != focused || strcmp(w.decoration_cache_title, w.title) != 0;

    if (frame_needs_rebuild) {
        bool needs_alloc =
            !w.decoration_cache.buffer || outer.w > w.decoration_cache_alloc_w || outer.h > w.decoration_cache_alloc_h;
        if (needs_alloc) {
            gui_destroy_surface(&w.decoration_cache);
            int aw = (outer.w + 63) & ~63;
            int ah = (outer.h + 31) & ~31;
            w.decoration_cache = gui_create_surface(static_cast<uint32_t>(aw), static_cast<uint32_t>(ah));
            w.decoration_cache_alloc_w = aw;
            w.decoration_cache_alloc_h = ah;
        }

        w.decoration_cache_w = outer.w;
        w.decoration_cache_h = outer.h;

        if (w.decoration_cache.buffer) {
            Surface view = w.decoration_cache;
            view.width = static_cast<uint32_t>(outer.w);
            view.height = static_cast<uint32_t>(outer.h);
            gui_fill_rect(&view, 0, 0, outer.w, outer.h, 0);

            Window local = w;
            local.x = 0;
            local.y = wm_title_bar_h();
            DirtyRect full = {0, 0, outer.w, outer.h};
            draw_window_decoration_frame(&view, local, full, focused);

            w.decoration_cache_theme_sig = theme_sig;
            w.decoration_cache_focused = focused;
            strncpy(w.decoration_cache_title, w.title, sizeof(w.decoration_cache_title) - 1);
            w.decoration_cache_title[sizeof(w.decoration_cache_title) - 1] = '\0';
        }
    }

    DirtyRect b0 = window_button_bounds(w, 0);
    DirtyRect b2 = window_button_bounds(w, 2);
    int buttons_w = (b2.x + b2.w) - b0.x;
    int buttons_h = b0.h;

    bool buttons_needs_rebuild = !w.button_cache.buffer || w.button_cache_w != buttons_w ||
                                 w.button_cache_h != buttons_h || w.button_cache_theme_sig != theme_sig ||
                                 w.button_cache_focused != focused || w.button_cache_hovered_button != hovered_button;

    if (buttons_needs_rebuild) {
        bool needs_alloc =
            !w.button_cache.buffer || buttons_w > w.button_cache_alloc_w || buttons_h > w.button_cache_alloc_h;
        if (needs_alloc) {
            gui_destroy_surface(&w.button_cache);
            int aw = (buttons_w + 15) & ~15;
            int ah = (buttons_h + 15) & ~15;
            w.button_cache = gui_create_surface(static_cast<uint32_t>(aw), static_cast<uint32_t>(ah));
            w.button_cache_alloc_w = aw;
            w.button_cache_alloc_h = ah;
        }
        w.button_cache_w = buttons_w;
        w.button_cache_h = buttons_h;

        if (w.button_cache.buffer) {
            Surface view = w.button_cache;
            view.width = static_cast<uint32_t>(buttons_w);
            view.height = static_cast<uint32_t>(buttons_h);
            gui_fill_rect(&view, 0, 0, buttons_w, buttons_h, 0);
            draw_window_decoration_buttons(&view, w, focused, hovered_button);
            w.button_cache_theme_sig = theme_sig;
            w.button_cache_focused = focused;
            w.button_cache_hovered_button = hovered_button;
        }
    }
}

void draw_window_decoration_clipped(Surface *dst, Window &w, const DirtyRect &clip, bool focused, bool hovered_frame,
                                    int hovered_button)
{
    if (!dst || !dst->buffer || w.transparent)
        return;

    bool actively_resizing = g_input.pointer_down && g_input.drag_edges != RESIZE_NONE && g_input.drag_index >= 0 &&
                             g_input.drag_index < g_window_count && g_windows[g_input.drag_index].entry == w.entry;

    if (!actively_resizing) {
        ensure_window_decoration_cache(w, focused, hovered_frame, hovered_button);
    }
    DirtyRect outer = window_outer_bounds(w);

    if (w.decoration_cache.buffer && !actively_resizing) {
        DirtyRect visible = {};
        if (rect_intersection(outer, clip, &visible)) {
            int src_x = visible.x - outer.x, src_y = visible.y - outer.y;
            uint32_t cache_stride = w.decoration_cache.pitch / 4;
            blit_alpha_blend_rect(&dst->buffer[static_cast<size_t>(visible.y) * (dst->pitch / 4) + visible.x],
                                  dst->pitch / 4,
                                  &w.decoration_cache.buffer[static_cast<size_t>(src_y) * cache_stride + src_x],
                                  cache_stride, visible.w, visible.h);
        }
    } else {
        // Active resize or no cache: draw frame directly for the dirty rect
        draw_window_decoration_frame(dst, w, clip, focused);
    }

    if (w.button_cache.buffer && !actively_resizing) {
        DirtyRect b0 = window_button_bounds(w, 0);
        DirtyRect buttons_rect = {b0.x, b0.y, w.button_cache_w, w.button_cache_h};
        DirtyRect visible = {};
        if (rect_intersection(buttons_rect, clip, &visible)) {
            int src_x = visible.x - buttons_rect.x, src_y = visible.y - buttons_rect.y;
            uint32_t cache_stride = w.button_cache.pitch / 4;
            blit_alpha_blend_rect(&dst->buffer[static_cast<size_t>(visible.y) * (dst->pitch / 4) + visible.x],
                                  dst->pitch / 4,
                                  &w.button_cache.buffer[static_cast<size_t>(src_y) * cache_stride + src_x],
                                  cache_stride, visible.w, visible.h);
        }
    } else if (actively_resizing) {
        draw_window_decoration_buttons_clipped(dst, w, clip, focused, hovered_button);
    }
}
