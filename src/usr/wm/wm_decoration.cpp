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
        return;
    }
    if (r <= 0 || w <= r * 2 || h <= r * 2) {
        gui_fill_rect(dst, ix, iy, iw, ih, color);
        return;
    }

    auto overlaps = [](int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
        return !(ax >= bx + bw || ax + aw <= bx || ay >= by + bh || ay + ah <= by);
    };

    bool touches_corner = overlaps(ix, iy, iw, ih, x, y, r, r) || overlaps(ix, iy, iw, ih, x + w - r, y, r, r) ||
                          overlaps(ix, iy, iw, ih, x, y + h - r, r, r) ||
                          overlaps(ix, iy, iw, ih, x + w - r, y + h - r, r, r);

    if (!touches_corner) {
        gui_fill_rect(dst, ix, iy, iw, ih, color);
        return;
    }

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (base_alpha == 0)
        return;
    const uint32_t pitch = dst->pitch / 4;
    const int dst_h = static_cast<int>(dst->height);
    const int dst_w = static_cast<int>(dst->width);
    bool full_opaque = base_alpha == 255;

    for (int py = iy; py < iy + ih; py++) {
        if (py < 0 || py >= dst_h)
            continue;
        uint32_t *row = &dst->buffer[static_cast<size_t>(py) * pitch];
        for (int px = ix; px < ix + iw; px++) {
            if (px < 0 || px >= dst_w)
                continue;
            int local_x = px - x;
            int local_y = py - y;
            uint8_t coverage = 255;
            if (local_x < r && local_y < r)
                coverage = gui_rounded_rect_coverage_local(local_x, local_y, w, h, r, GUI_ROUNDED_EDGE_ALL);
            else if (local_x >= w - r && local_y < r)
                coverage = gui_rounded_rect_coverage_local(local_x, local_y, w, h, r, GUI_ROUNDED_EDGE_ALL);
            else if (local_x < r && local_y >= h - r)
                coverage = gui_rounded_rect_coverage_local(local_x, local_y, w, h, r, GUI_ROUNDED_EDGE_ALL);
            else if (local_x >= w - r && local_y >= h - r)
                coverage = gui_rounded_rect_coverage_local(local_x, local_y, w, h, r, GUI_ROUNDED_EDGE_ALL);
            if (coverage == 255) {
                if (full_opaque)
                    row[px] = color;
                else
                    row[px] = blend_rgb(row[px], color, base_alpha);
            } else if (coverage > 0) {
                row[px] = blend_rgb(row[px], color, coverage);
            }
        }
    }
}

static void gui_draw_rounded_rect_clipped(Surface *dst, int x, int y, int w, int h, int r, uint32_t color,
                                          const DirtyRect &clip)
{
    int ix, iy, iw, ih;
    if (!gui_intersect_rect(x, y, w, h, clip.x, clip.y, clip.w, clip.h, &ix, &iy, &iw, &ih))
        return;
    if (rect_contains(clip, {x, y, w, h})) {
        gui_draw_rounded_rect(dst, x, y, w, h, r, color);
        return;
    }
    // Outline drawing outside the clip would overwrite neighbours already
    // composed this frame; draw the full outline into a scratch view of the
    // cache and blit only the visible intersection.
    // For the common case (decoration frame stroke) the cache path handles
    // this; the direct-draw path during resize falls through to the full
    // gui_draw_rounded_rect which clips to surface bounds only.
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
    w.decoration_cache_w = 0;
    w.decoration_cache_h = 0;
    // Release the locked tint too: every caller invalidates because something
    // legitimately changed the content (resize, buffer remap, theme switch),
    // so the chrome must re-sample the client afterwards.
    w.decoration_bg_locked = false;
    w.decoration_bg_color = 0;
}

static uint32_t sample_window_app_background(const Window &w, bool *opaque)
{
    if (opaque)
        *opaque = false;

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
            if (opaque)
                *opaque = true;
            return 0xFF000000u | (pixel & 0x00FFFFFFu);
        }
    }
    return g_gui_style.app_bg ? g_gui_style.app_bg : g_gui_style.app_surface;
}

uint32_t get_window_app_background(const Window &w)
{
    if (w.decoration_bg_locked)
        return w.decoration_bg_color;
    return sample_window_app_background(w, nullptr);
}

static void lock_window_decoration_background(Window &w)
{
    if (w.decoration_bg_locked)
        return;
    // Right after a theme switch the tint must keep following the live buffer
    // while the app redraws with the new palette; lock only once the client
    // has had time to settle.
    if (w.decoration_resample_until != 0 && get_ticks() < w.decoration_resample_until)
        return;
    w.decoration_resample_until = 0;
    bool opaque = false;
    uint32_t color = sample_window_app_background(w, &opaque);
    if (opaque) {
        w.decoration_bg_color = color;
        w.decoration_bg_locked = true;
    }
}

static uint32_t window_decoration_theme_signature(const Window &w)
{
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t value) {
        sig ^= value;
        sig *= 16777619u;
    };
    mix(get_window_app_background(w));
    // The soft shadow is only drawn once the tint is locked (see
    // draw_window_decoration_frame); toggling the lock must rebuild even if
    // the sampled color equals the fallback.
    mix(w.decoration_bg_locked ? 1u : 0u);
    mix(static_cast<uint32_t>(gui_theme_is_light() ? 1u : 0u));
    mix(g_gui_style.border);
    mix(g_gui_style.border_focus);
    mix(g_gui_style.border_hover);
    mix(g_gui_chrome.frame_outline);
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

static void draw_window_decoration_frame(Surface *dst, const Window &w, const DirtyRect &clip)
{
    if (w.transparent)
        return;

    // macOS-style window chrome: a single 1-px semi-transparent hairline on the
    // silhouette plus a soft symmetric shadow, identical to the edge and shadow
    // every floating panel draws. Traffic lights are overlaid on the client AFTER
    // the blit (from compose_rect_clipped). The frame is the same focused or not.
    int radius = gui_radius_xl();
    uint32_t body_color = get_window_app_background(w);

    // When rendering into the decoration cache the window is centred so the soft
    // shadow can spread equally on all four sides; when drawing straight to the
    // backbuffer (active resize) the window is already at its screen position.
    int pad = wm_frame_shadow_offset_y();
    int lx = (dst->buffer != g_backbuffer.buffer) ? pad : w.x;
    int ly = (dst->buffer != g_backbuffer.buffer) ? pad : w.y;
    int sx = lx, sy = ly, sw = w.w, sh = w.h;

    // The soft shadow is the expensive part of the chrome. Skip it until the
    // client's background tint is locked: during launch the buffer is still
    // zeroed/progressive, and rendering the shadow now would be thrown away
    // by the rebuild the first opaque frame triggers. Presenting flat chrome
    // first keeps the compositor responsive while the app starts.
    if (w.decoration_bg_locked)
        gui_draw_panel_shadow(dst, sx, sy, sw, sh, radius);

    // Opaque backing so the translucent hairline reads cleanly; the client blit
    // covers everything but the 1-px edge ring and the corners.
    gui_fill_rounded_rect_clipped(dst, sx, sy, sw, sh, radius, body_color, clip);

    if (sw > 2 && sh > 2)
        gui_draw_rounded_rect_clipped(dst, sx, sy, sw, sh, radius, gui_window_outer_stroke_color(), clip);
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

void draw_window_decoration_buttons_clipped(Surface *dst, const Window &w, const DirtyRect &clip, bool focused,
                                            int hovered_button)
{
    draw_window_decoration_buttons_to(dst, w, 0, 0, &clip, focused, hovered_button);
}

static void ensure_window_decoration_cache(Window &w)
{
    if (w.transparent)
        return;

    // Lock the tint before computing the signature: once the client has
    // committed an opaque frame the signature stops tracking the live buffer,
    // so the progressive frames an app draws while launching no longer
    // rebuild the shadow cache over and over.
    lock_window_decoration_background(w);

    DirtyRect outer = window_outer_bounds(w);
    uint32_t theme_sig = window_decoration_theme_signature(w);

    bool frame_needs_rebuild = !w.decoration_cache.buffer || w.decoration_cache_w != outer.w ||
                               w.decoration_cache_h != outer.h || w.decoration_cache_theme_sig != theme_sig;

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
            local.y = 0;
            DirtyRect full = {0, 0, outer.w, outer.h};
            draw_window_decoration_frame(&view, local, full);

            w.decoration_cache_theme_sig = theme_sig;
        }
    }
}

void draw_window_decoration_clipped(Surface *dst, Window &w, const DirtyRect &clip)
{
    if (!dst || !dst->buffer || w.transparent)
        return;

    bool actively_resizing = g_input.pointer_down && g_input.drag_edges != RESIZE_NONE && g_input.drag_index >= 0 &&
                             g_input.drag_index < g_window_count && g_windows[g_input.drag_index].entry == w.entry;

    if (!actively_resizing) {
        ensure_window_decoration_cache(w);
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
        draw_window_decoration_frame(dst, w, clip);
    }
    // Traffic-light buttons are drawn separately AFTER the client blit (called
    // from compose_rect_clipped) so they overlay the client content.
}
