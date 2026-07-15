#include "wm_core.h"

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

static bool g_menubar_blur_dirty = false;
static bool g_dock_blur_dirty = false;
static uint64_t g_last_blur_vblank = 0;

// Blur source dirty rect tracking - must be declared before first use in mark_shell_blur_dirty
static DirtyRect g_menubar_blur_dirty_rects[MAX_DIRTY_RECTS];
static int g_menubar_blur_dirty_count = 0;
static DirtyRect g_dock_blur_dirty_rects[MAX_DIRTY_RECTS];
static int g_dock_blur_dirty_count = 0;

static void add_blur_dirty_rect(DirtyRect *rects, int *count, const DirtyRect &r)
{
    DirtyRect clip = r;
    if (!clip_dirty_rect_to_screen(clip))
        return;
    if (*count < MAX_DIRTY_RECTS) {
        rects[(*count)++] = clip;
    } else {
        if (*count == 1) {
            rects[0] = rect_union(rects[0], clip);
        }
    }
}

static void clear_blur_dirty_rects(DirtyRect * /*rects*/, int *count)
{
    *count = 0;
}

void invalidate_window_decoration_cache(Window &w)
{
    w.decoration_cache_theme_sig = 0;
    w.button_cache_theme_sig = 0;
    w.decoration_cache_w = 0;
    w.decoration_cache_h = 0;
}

uint32_t mix_rgb(uint32_t a, uint32_t b, uint8_t t)
{
    uint32_t inv = 255u - t;
    uint32_t ar = (a >> 16) & 0xFFu, ag = (a >> 8) & 0xFFu, ab = a & 0xFFu;
    uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    return 0xFF000000u | (div255(ar * inv + br * t) << 16) | (div255(ag * inv + bg * t) << 8) |
           div255(ab * inv + bb * t);
}

uint32_t mix_rgb_keep_alpha(uint32_t base, uint32_t tint, uint8_t t)
{
    return (base & 0xFF000000u) | (mix_rgb(base, tint, t) & 0x00FFFFFFu);
}

int color_luma(uint32_t color)
{
    int r = (color >> 16) & 0xFF;
    int g = (color >> 8) & 0xFF;
    int b = color & 0xFF;
    return (r * 54 + g * 183 + b * 19 + 128) >> 8;
}

uint32_t blend_rgb(uint32_t dst, uint32_t src, uint8_t coverage)
{
    uint32_t src_alpha = (src >> 24);
    uint32_t sa_cov = src_alpha * (uint32_t)coverage + 128u;
    src_alpha = (sa_cov + (sa_cov >> 8)) >> 8;
    if (!src_alpha)
        return dst;
    if (src_alpha == 255)
        return (src & 0x00FFFFFFu) | 0xFF000000u;

    uint32_t dst_alpha = dst >> 24;
    uint32_t inv_sa = 255u - src_alpha;

    uint32_t sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu, sb = src & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu, db = dst & 0xFFu;

    uint32_t da_inv = dst_alpha * inv_sa + 128u;
    uint32_t out_a = src_alpha + ((da_inv + (da_inv >> 8)) >> 8);
    if (!out_a)
        return 0;

    if (dst_alpha == 255) {
        uint32_t rb = (src & 0x00FF00FFu) * src_alpha + (dst & 0x00FF00FFu) * inv_sa + 0x00800080u;
        rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
        uint32_t g_acc = sg * src_alpha + dg * inv_sa + 0x80u;
        uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
        return 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
    }

    uint32_t term_dr = dr * dst_alpha * inv_sa + 128u;
    term_dr = (term_dr + (term_dr >> 8)) >> 8;
    uint32_t out_r = (sr * src_alpha + term_dr + out_a / 2) / out_a;

    uint32_t term_dg = dg * dst_alpha * inv_sa + 128u;
    term_dg = (term_dg + (term_dg >> 8)) >> 8;
    uint32_t out_g = (sg * src_alpha + term_dg + out_a / 2) / out_a;

    uint32_t term_db = db * dst_alpha * inv_sa + 128u;
    term_db = (term_db + (term_db >> 8)) >> 8;
    uint32_t out_b = (sb * src_alpha + term_db + out_a / 2) / out_a;

    if (out_r > 255)
        out_r = 255;
    if (out_g > 255)
        out_g = 255;
    if (out_b > 255)
        out_b = 255;

    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

#include <emmintrin.h>

static void blit_alpha_blend_rect(uint32_t *__restrict__ dst, uint32_t dst_stride, const uint32_t *__restrict__ src,
                                  uint32_t src_stride, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    const __m128i alpha_mask = _mm_set1_epi32(static_cast<int>(0xFF000000u));
    const __m128i zero = _mm_setzero_si128();
    const __m128i v_255 = _mm_set1_epi16(255);
    const __m128i v_128 = _mm_set1_epi16(128);

    for (int y = 0; y < h; ++y) {
        uint32_t *drow = &dst[static_cast<size_t>(y) * dst_stride];
        const uint32_t *srow = &src[static_cast<size_t>(y) * src_stride];
        
        int x = 0;
        for (; x <= w - 4; x += 4) {
            __m128i s_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&srow[x]));
            
            __m128i s_alpha = _mm_and_si128(s_vec, alpha_mask);
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(s_alpha, _mm_setzero_si128())) == 0xFFFF) {
                continue;
            }

            __m128i d_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&drow[x]));
            __m128i d_alpha = _mm_and_si128(d_vec, alpha_mask);
            __m128i trans_mask = _mm_cmpeq_epi32(d_alpha, _mm_setzero_si128());
            __m128i opaque_dst_mask = _mm_cmpeq_epi32(d_alpha, alpha_mask);
            __m128i fast_mask = _mm_or_si128(opaque_dst_mask, trans_mask);

            if (_mm_movemask_epi8(_mm_cmpeq_epi32(fast_mask, _mm_setzero_si128())) != 0) {
                for (int k = 0; k < 4; ++k) {
                    uint32_t s = srow[x + k];
                    uint32_t sa = s >> 24;
                    if (!sa) continue;
                    if (sa == 255) { drow[x + k] = s; continue; }
                    uint32_t d = drow[x + k];
                    uint32_t da = d >> 24;
                    if (da == 255) {
                        uint32_t inv_sa = 255u - sa;
                        uint32_t rb = (s & 0x00FF00FFu) * sa + (d & 0x00FF00FFu) * inv_sa + 0x00800080u;
                        rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                        uint32_t g_acc = ((s >> 8) & 0xFFu) * sa + ((d >> 8) & 0xFFu) * inv_sa + 0x80u;
                        uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
                        drow[x + k] = 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
                    } else {
                        drow[x + k] = blend_rgb(d, s, 255);
                    }
                }
                continue;
            }

            __m128i s_lo = _mm_unpacklo_epi8(s_vec, zero);
            __m128i s_hi = _mm_unpackhi_epi8(s_vec, zero);
            __m128i d_lo = _mm_unpacklo_epi8(d_vec, zero);
            __m128i d_hi = _mm_unpackhi_epi8(d_vec, zero);

            __m128i alpha_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
            alpha_lo = _mm_shufflehi_epi16(alpha_lo, _MM_SHUFFLE(3, 3, 3, 3));
            
            __m128i alpha_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
            alpha_hi = _mm_shufflehi_epi16(alpha_hi, _MM_SHUFFLE(3, 3, 3, 3));

            __m128i inv_alpha_lo = _mm_sub_epi16(v_255, alpha_lo);
            __m128i inv_alpha_hi = _mm_sub_epi16(v_255, alpha_hi);

            __m128i src_part_lo = _mm_mullo_epi16(s_lo, alpha_lo);
            __m128i src_part_hi = _mm_mullo_epi16(s_hi, alpha_hi);

            __m128i dst_part_lo = _mm_mullo_epi16(d_lo, inv_alpha_lo);
            __m128i dst_part_hi = _mm_mullo_epi16(d_hi, inv_alpha_hi);

            __m128i sum_lo = _mm_add_epi16(_mm_add_epi16(src_part_lo, dst_part_lo), v_128);
            __m128i sum_hi = _mm_add_epi16(_mm_add_epi16(src_part_hi, dst_part_hi), v_128);

            __m128i sum_lo_shift = _mm_srli_epi16(sum_lo, 8);
            __m128i sum_hi_shift = _mm_srli_epi16(sum_hi, 8);
            
            __m128i final_lo = _mm_srli_epi16(_mm_add_epi16(sum_lo, sum_lo_shift), 8);
            __m128i final_hi = _mm_srli_epi16(_mm_add_epi16(sum_hi, sum_hi_shift), 8);

            __m128i blended_vec = _mm_packus_epi16(final_lo, final_hi);

            blended_vec = _mm_or_si128(blended_vec, alpha_mask);
            blended_vec = _mm_or_si128(_mm_and_si128(trans_mask, s_vec),
                                       _mm_andnot_si128(trans_mask, blended_vec));

            _mm_storeu_si128(reinterpret_cast<__m128i*>(&drow[x]), blended_vec);
        }

        for (; x < w; ++x) {
            uint32_t s = srow[x];
            uint32_t sa = s >> 24;
            if (!sa)
                continue;
            if (sa == 255) {
                drow[x] = s;
                continue;
            }

            uint32_t d = drow[x];
            uint32_t da = d >> 24;

            if (da == 255) {
                uint32_t inv_sa = 255u - sa;
                uint32_t rb = (s & 0x00FF00FFu) * sa + (d & 0x00FF00FFu) * inv_sa + 0x00800080u;
                rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
                uint32_t g_acc = ((s >> 8) & 0xFFu) * sa + ((d >> 8) & 0xFFu) * inv_sa + 0x80u;
                uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
                drow[x] = 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
            } else if (!da) {
                drow[x] = s;
            } else {
                drow[x] = blend_rgb(d, s, 255);
            }
        }
    }
}

void copy_surface_rect(Surface *dst, int dst_x, int dst_y, const Surface *src, int src_x, int src_y, int w, int h)
{
    if (!dst || !src || !dst->buffer || !src->buffer || dst->pitch == 0 || src->pitch == 0 || w <= 0 || h <= 0)
        return;

    if (src_x < 0) {
        dst_x -= src_x;
        w += src_x;
        src_x = 0;
    }
    if (src_y < 0) {
        dst_y -= src_y;
        h += src_y;
        src_y = 0;
    }
    if (dst_x < 0) {
        src_x -= dst_x;
        w += dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y -= dst_y;
        h += dst_y;
        dst_y = 0;
    }

    if (w <= 0 || h <= 0)
        return;
    if (static_cast<uint32_t>(src_x) >= src->width || static_cast<uint32_t>(src_y) >= src->height ||
        static_cast<uint32_t>(dst_x) >= dst->width || static_cast<uint32_t>(dst_y) >= dst->height)
        return;

    if (src_x + w > static_cast<int>(src->width))
        w = static_cast<int>(src->width) - src_x;
    if (src_y + h > static_cast<int>(src->height))
        h = static_cast<int>(src->height) - src_y;
    if (dst_x + w > static_cast<int>(dst->width))
        w = static_cast<int>(dst->width) - dst_x;
    if (dst_y + h > static_cast<int>(dst->height))
        h = static_cast<int>(dst->height) - dst_y;

    if (w <= 0 || h <= 0)
        return;

    uint32_t dst_stride = dst->pitch / 4u;
    uint32_t src_stride = src->pitch / 4u;

    const bool overlap = (dst->buffer == src->buffer) && !((dst_x + w) <= src_x || (src_x + w) <= dst_x ||
                                                           (dst_y + h) <= src_y || (src_y + h) <= dst_y);

    int start_y = 0, end_y = h, step_y = 1;
    if (overlap && dst_y > src_y) {
        start_y = h - 1;
        end_y = -1;
        step_y = -1;
    }

    for (int y = start_y; y != end_y; y += step_y) {
        uint32_t *drow = &dst->buffer[static_cast<size_t>(dst_y + y) * dst_stride + dst_x];
        const uint32_t *srow = &src->buffer[static_cast<size_t>(src_y + y) * src_stride + src_x];
        if (overlap)
            memmove(drow, srow, static_cast<size_t>(w) * sizeof(uint32_t));
        else
            memcpy(drow, srow, static_cast<size_t>(w) * sizeof(uint32_t));
    }
}

bool ensure_surface_capacity(Surface *surface, uint32_t width, uint32_t height)
{
    if (!surface)
        return false;
    if (surface->buffer && surface->width >= width && surface->height >= height) {
        surface->width = width;
        surface->height = height;
        return true;
    }

    gui_destroy_surface(surface);
    uint32_t padded_w = (width + 255u) & ~255u;
    uint32_t padded_h = (height + 255u) & ~255u;
    *surface = gui_create_surface(padded_w, padded_h);
    if (surface->buffer) {
        surface->width = width;
        surface->height = height;
        return true;
    }
    return false;
}

static void compose_desktop_for_blur(Surface *dst, const DirtyRect &clip, int offset_x, int offset_y)
{
    DirtyRect shifted_clip = {clip.x - offset_x, clip.y - offset_y, clip.w, clip.h};
    int start_index = -1;
    bool covered = false;

    for (int i = g_window_count - 1; i >= WM_FIRST_USER_WINDOW; --i) {
        if (!g_window_visible_cache[i] || g_windows[i].transparent || !g_windows[i].buffer)
            continue;
        DirtyRect outer = window_occlusion_bounds(g_windows[i]);
        if (clip.x >= outer.x && clip.y >= outer.y && clip.x + clip.w <= outer.x + outer.w &&
            clip.y + clip.h <= outer.y + outer.h) {
            start_index = i;
            covered = true;
            break;
        }
    }

    if (!covered) {
        gui_blit_rect(dst, &g_wallpaper, shifted_clip.x, shifted_clip.y, clip.x, clip.y, clip.w, clip.h);
    }

    start_index = (start_index < WM_FIRST_USER_WINDOW) ? WM_FIRST_USER_WINDOW : start_index;

    for (int i = start_index; i < g_window_count; ++i) {
        if (!g_window_visible_cache[i] || !g_windows[i].buffer || g_windows[i].transparent ||
            !dirty_rects_intersect(clip, g_window_outer_cache[i]))
            continue;
        Window local = g_windows[i];
        local.x -= offset_x;
        local.y -= offset_y;
        draw_window_client_clipped(dst, local, shifted_clip);
    }

    for (int i = start_index; i < g_window_count; ++i) {
        if (!g_window_visible_cache[i] || !g_windows[i].buffer || !g_windows[i].transparent ||
            !dirty_rects_intersect(clip, g_window_outer_cache[i]))
            continue;
        Window local = g_windows[i];
        local.x -= offset_x;
        local.y -= offset_y;
        draw_window_client_clipped(dst, local, shifted_clip);
    }
}

static void mark_shell_blur_dirty(Registry *registry, const DirtyRect &screen_rect)
{
    if (!registry || !g_backbuffer.buffer)
        return;

    DirtyRect menubar_rect = {0, 0, static_cast<int>(g_screen.width), wm_menubar_h()};
    DirtyRect overlap = {};

    if (g_menubar_blur_source.buffer && rect_intersection(screen_rect, menubar_rect, &overlap)) {
        compose_desktop_for_blur(&g_menubar_blur_source, overlap, 0, 0);
        g_menubar_blur_dirty = true;
        add_blur_dirty_rect(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count, overlap);
    }

    if (g_dock_blur_source.buffer && registry->window_count > 1) {
        DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y, registry->windows[1].w,
                               registry->windows[1].h};
        if (clip_dirty_rect_to_screen(dock_rect) && rect_intersection(screen_rect, dock_rect, &overlap)) {
            compose_desktop_for_blur(&g_dock_blur_source, overlap, dock_rect.x, dock_rect.y);
            g_dock_blur_dirty = true;
            add_blur_dirty_rect(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count, overlap);
        }
    }
}

void recapture_shell_blur_sources(Registry *registry)
{
    if (!registry || !g_backbuffer.buffer)
        return;

    // Re-capture full menubar source
    if (g_menubar_blur_source.buffer) {
        int menubar_h = wm_menubar_h();
        DirtyRect full_menubar = {0, 0, static_cast<int>(g_screen.width), menubar_h};
        compose_desktop_for_blur(&g_menubar_blur_source, full_menubar, 0, 0);
        g_menubar_blur_dirty = true;
        clear_blur_dirty_rects(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count);
    }

    // Re-capture full dock source
    if (g_dock_blur_source.buffer && registry->window_count > 1) {
        DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y,
                               registry->windows[1].w, registry->windows[1].h};
        if (clip_dirty_rect_to_screen(dock_rect)) {
            compose_desktop_for_blur(&g_dock_blur_source, dock_rect, dock_rect.x, dock_rect.y);
            g_dock_blur_dirty = true;
            clear_blur_dirty_rects(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count);
        }
    }
}

static int g_cached_top_r = -1;
static uint8_t g_top_corner_mask_lut[64][64] = {};

static int g_cached_inner_r = -1;
static uint8_t g_bottom_corner_mask_lut[64][64] = {};

static void fill_top_rounded_rect_clipped(Surface *dst, int x, int y, int w, int h, int r, uint32_t color)
{
    if (!dst || !dst->buffer || w <= 0 || h <= 0)
        return;

    if (r < 0)
        r = 0;
    if (r > w / 2)
        r = w / 2;
    if (r > h)
        r = h;

    uint8_t base_alpha = static_cast<uint8_t>(color >> 24);
    if (!base_alpha)
        return;

    const uint32_t pitch = dst->pitch / 4;
    const int dst_w = static_cast<int>(dst->width);
    const int dst_h = static_cast<int>(dst->height);

    int start_y = y < 0 ? 0 : y;
    int end_y = y + h > dst_h ? dst_h : y + h;
    int start_x = x < 0 ? 0 : x;
    int end_x = x + w > dst_w ? dst_w : x + w;

    if (r <= 0) {
        gui_fill_rect(dst, start_x, start_y, end_x - start_x, end_y - start_y, color);
        return;
    }

    static constexpr int kCornerMaskMax = 64;
    int local_r = r > kCornerMaskMax ? kCornerMaskMax : r;
    uint8_t corner_mask[kCornerMaskMax];
    int corner_mask_y = -1;

    const int center_start_x = x + local_r;
    const int center_end_x = x + w - local_r;
    const int top_band_end = y + local_r;
    const bool full_opaque = base_alpha == 255;

    for (int py = start_y; py < end_y; ++py) {
        const int row = py - y;
        uint32_t *dst_row = &dst->buffer[static_cast<size_t>(py) * pitch];

        if (py >= top_band_end) {
            if (full_opaque) {
                int span = end_x - start_x;
                if (span <= 0)
                    continue;
                uint32_t *span_dst = &dst_row[start_x];
                uint32_t v0 = color, v1 = color, v2 = color, v3 = color;
                int i = 0;
                for (; i + 7 < span; i += 8) {
                    span_dst[0] = v0; span_dst[1] = v1; span_dst[2] = v2; span_dst[3] = v3;
                    span_dst[4] = v0; span_dst[5] = v1; span_dst[6] = v2; span_dst[7] = v3;
                    span_dst += 8;
                }
                for (; i < span; ++i)
                    *span_dst++ = v0;
            } else {
                for (int px = start_x; px < end_x; ++px)
                    dst_row[px] = blend_rgb(dst_row[px], color, base_alpha);
            }
            continue;
        }

        if (row != corner_mask_y) {
            if (local_r != g_cached_top_r && local_r <= 64) {
                for (int cy = 0; cy < local_r; ++cy) {
                    for (int cx = 0; cx < local_r; ++cx) {
                        g_top_corner_mask_lut[cy][cx] = gui_rounded_rect_coverage_local(
                            cx, cy, local_r * 2, local_r * 2, local_r, GUI_ROUNDED_EDGE_TOP);
                    }
                }
                g_cached_top_r = local_r;
            }

            if (local_r <= 64 && row < local_r) {
                for (int col = 0; col < local_r; ++col) {
                    corner_mask[col] = g_top_corner_mask_lut[row][col];
                }
            } else {
                for (int col = 0; col < local_r; ++col) {
                    corner_mask[col] = gui_rounded_rect_coverage_local(col, row, local_r * 2, local_r * 2, local_r, GUI_ROUNDED_EDGE_TOP);
                }
            }
            corner_mask_y = row;
        }

        int left_end = center_start_x < end_x ? center_start_x : end_x;
        for (int px = start_x; px < left_end; ++px) {
            int local = px - x;
            uint8_t coverage = corner_mask[local];
            if (!coverage)
                continue;
            if (coverage == 255 && full_opaque)
                dst_row[px] = color;
            else if (coverage)
                dst_row[px] = blend_rgb(dst_row[px], color, coverage);
        }

        int center_lo = start_x > center_start_x ? start_x : center_start_x;
        int center_hi = end_x < center_end_x ? end_x : center_end_x;
        int center_w = center_hi - center_lo;
        if (full_opaque && center_w > 0) {
            uint32_t *center_dst = &dst_row[center_lo];
            uint32_t v0 = color, v1 = color, v2 = color, v3 = color;
            uint32_t *p = center_dst;
            int i = 0;
            for (; i + 7 < center_w; i += 8) {
                p[0] = v0; p[1] = v1; p[2] = v2; p[3] = v3;
                p[4] = v0; p[5] = v1; p[6] = v2; p[7] = v3;
                p += 8;
            }
            for (; i < center_w; ++i)
                *p++ = v0;
        } else if (center_w > 0) {
            for (int px = center_lo; px < center_hi; ++px)
                dst_row[px] = blend_rgb(dst_row[px], color, base_alpha);
        }

        int right_lo = start_x > center_end_x ? start_x : center_end_x;
        for (int px = right_lo; px < end_x; ++px) {
            int local = w - 1 - (px - x);
            uint8_t coverage = corner_mask[local];
            if (!coverage)
                continue;
            if (coverage == 255 && full_opaque)
                dst_row[px] = color;
            else if (coverage)
                dst_row[px] = blend_rgb(dst_row[px], color, coverage);
        }
    }
}

void paint_desktop_base(Surface *surface)
{
    if (!surface || !surface->buffer || surface->pitch == 0)
        return;

    const uint32_t color_top = 0xFF0B1533u;
    const uint32_t color_mid = 0xFF2C1F54u;
    const uint32_t color_bottom = 0xFF140A12u;
    const uint32_t stride = surface->pitch / 4u;
    const uint32_t height = surface->height;
    const uint32_t width = surface->width;

    if (height == 0 || width == 0)
        return;

    uint32_t y_mid = (132u * (height - 1u)) / 255u;
    if (y_mid > height)
        y_mid = height;

    {
        int32_t r_fp = static_cast<int32_t>((color_top >> 16) & 0xFFu) << 16;
        int32_t g_fp = static_cast<int32_t>((color_top >> 8) & 0xFFu) << 16;
        int32_t b_fp = static_cast<int32_t>(color_top & 0xFFu) << 16;

        int32_t step_r = y_mid > 0 ? (((static_cast<int32_t>((color_mid >> 16) & 0xFFu) - static_cast<int32_t>((color_top >> 16) & 0xFFu))) << 16) / static_cast<int32_t>(y_mid) : 0;
        int32_t step_g = y_mid > 0 ? (((static_cast<int32_t>((color_mid >> 8) & 0xFFu) - static_cast<int32_t>((color_top >> 8) & 0xFFu))) << 16) / static_cast<int32_t>(y_mid) : 0;
        int32_t step_b = y_mid > 0 ? (((static_cast<int32_t>(color_mid & 0xFFu) - static_cast<int32_t>(color_top & 0xFFu))) << 16) / static_cast<int32_t>(y_mid) : 0;

        for (uint32_t y = 0; y < y_mid; ++y) {
            uint32_t row_color = 0xFF000000u |
                                 ((static_cast<uint32_t>(r_fp >> 16) & 0xFFu) << 16) |
                                 ((static_cast<uint32_t>(g_fp >> 16) & 0xFFu) << 8) |
                                 (static_cast<uint32_t>(b_fp >> 16) & 0xFFu);
            uint32_t *row = &surface->buffer[static_cast<size_t>(y) * stride];
            for (uint32_t x = 0; x < width; ++x)
                row[x] = row_color;
            r_fp += step_r;
            g_fp += step_g;
            b_fp += step_b;
        }
    }

    {
        uint32_t h2 = height - y_mid;
        int32_t r_fp = static_cast<int32_t>((color_mid >> 16) & 0xFFu) << 16;
        int32_t g_fp = static_cast<int32_t>((color_mid >> 8) & 0xFFu) << 16;
        int32_t b_fp = static_cast<int32_t>(color_mid & 0xFFu) << 16;

        int32_t step_r = h2 > 1 ? (((static_cast<int32_t>((color_bottom >> 16) & 0xFFu) - static_cast<int32_t>((color_mid >> 16) & 0xFFu))) << 16) / static_cast<int32_t>(h2 - 1u) : 0;
        int32_t step_g = h2 > 1 ? (((static_cast<int32_t>((color_bottom >> 8) & 0xFFu) - static_cast<int32_t>((color_mid >> 8) & 0xFFu))) << 16) / static_cast<int32_t>(h2 - 1u) : 0;
        int32_t step_b = h2 > 1 ? (((static_cast<int32_t>(color_bottom & 0xFFu) - static_cast<int32_t>(color_mid & 0xFFu))) << 16) / static_cast<int32_t>(h2 - 1u) : 0;

        for (uint32_t y = y_mid; y < height; ++y) {
            uint32_t row_color = 0xFF000000u |
                                 ((static_cast<uint32_t>(r_fp >> 16) & 0xFFu) << 16) |
                                 ((static_cast<uint32_t>(g_fp >> 16) & 0xFFu) << 8) |
                                 (static_cast<uint32_t>(b_fp >> 16) & 0xFFu);
            uint32_t *row = &surface->buffer[static_cast<size_t>(y) * stride];
            for (uint32_t x = 0; x < width; ++x)
                row[x] = row_color;
            r_fp += step_r;
            g_fp += step_g;
            b_fp += step_b;
        }
    }
}

static void publish_wallpaper_state(Registry *registry, uint32_t status, const char *path)
{
    if (!registry)
        return;
    registry->wallpaper_status = status;
    registry->wallpaper_active[0] = '\0';
    if (path && *path) {
        strncpy(registry->wallpaper_active, path, sizeof(registry->wallpaper_active) - 1);
        registry->wallpaper_active[sizeof(registry->wallpaper_active) - 1] = '\0';
    }
    asm volatile("sfence" ::: "memory");
}

static bool apply_wallpaper_image(const char *path, uint32_t theme_mode)
{
    if (!path || !*path)
        return false;
    Surface image = {};
    if (!gui_load_uowp(path, wallpaper_uowp_variant_for_theme(theme_mode), g_screen.width, g_screen.height, &image))
        return false;
    gui_blit_scaled_cover(&g_wallpaper, &image);
    gui_destroy_surface(&image);
    return true;
}

static uint32_t registry_theme_mode(const Registry *registry)
{
    return (registry && registry->theme_mode == GUI_THEME_LIGHT) ? GUI_THEME_LIGHT : GUI_THEME_DARK;
}

static void copy_resolved_wallpaper_path(const Registry *registry, const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    const char *resolved = wallpaper_resolve_path_for_theme(path, registry_theme_mode(registry));
    if (!resolved || !*resolved)
        return;
    strncpy(out, resolved, out_size - 1);
    out[out_size - 1] = '\0';
}

void init_wallpaper()
{
    g_wallpaper = gui_create_surface(g_screen.width, g_screen.height);
    paint_desktop_base(&g_wallpaper);
}

void reload_wallpaper(Registry *registry, bool prefer_requested)
{
    uint32_t status = WALLPAPER_STATUS_SOLID;
    char requested[256] = {};
    char configured[256] = {};
    const char *active_path = nullptr;

    paint_desktop_base(&g_wallpaper);
    if (prefer_requested && registry && registry->wallpaper_requested[0]) {
        strncpy(requested, registry->wallpaper_requested, sizeof(requested) - 1);
        requested[sizeof(requested) - 1] = '\0';
    } else {
        VNodeStat st = {};
        const char *config_path = (stat(WALLPAPER_CONFIG_PATH, &st) == 0 && !st.is_dir)
                                      ? WALLPAPER_CONFIG_PATH
                                      : WALLPAPER_BOOTSTRAP_CONFIG_PATH;
        if (cfg_read_first_line(config_path, requested, sizeof(requested)) && registry) {
            strncpy(registry->wallpaper_requested, requested, sizeof(registry->wallpaper_requested) - 1);
            registry->wallpaper_requested[sizeof(registry->wallpaper_requested) - 1] = '\0';
        }
    }
    copy_resolved_wallpaper_path(registry, requested, configured, sizeof(configured));

    uint32_t theme_mode = registry_theme_mode(registry);
    if (configured[0] && apply_wallpaper_image(configured, theme_mode)) {
        status = wallpaper_is_default_family_path(configured) ? WALLPAPER_STATUS_DEFAULT : WALLPAPER_STATUS_CUSTOM;
        active_path = configured;
    } else if (apply_wallpaper_image(wallpaper_default_path_for_theme(theme_mode), theme_mode)) {
        status = WALLPAPER_STATUS_DEFAULT;
        active_path = wallpaper_default_path_for_theme(theme_mode);
    }
    publish_wallpaper_state(registry, status, active_path);
}

bool init_shell_blur_buffers(Registry *registry, uint32_t dock_w, uint32_t dock_h)
{
    if (!registry || !gui_shm_id_is_valid(registry->mb_blur_shm_id) || !gui_shm_id_is_valid(registry->dk_blur_shm_id))
        return false;

    int menubar_h = wm_menubar_h();
    if (menubar_h <= 0 || dock_w == 0 || dock_h == 0)
        return false;

    uint64_t mb_map = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(registry->mb_blur_shm_id));
    uint64_t dk_map = syscall1(SYS_SHM_MAP, static_cast<uint64_t>(registry->dk_blur_shm_id));

    if (mb_map == 0 || mb_map == static_cast<uint64_t>(-1) || dk_map == 0 || dk_map == static_cast<uint64_t>(-1)) {
        if (mb_map != 0 && mb_map != static_cast<uint64_t>(-1))
            syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->mb_blur_shm_id));
        if (dk_map != 0 && dk_map != static_cast<uint64_t>(-1))
            syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->dk_blur_shm_id));
        return false;
    }

    gui_destroy_surface(&g_menubar_blur_source);
    gui_destroy_surface(&g_dock_blur_source);
    g_menubar_blur_source = gui_create_surface(g_screen.width, static_cast<uint32_t>(menubar_h));
    g_dock_blur_source = gui_create_surface(dock_w, dock_h);

    if (!g_menubar_blur_source.buffer || !g_dock_blur_source.buffer) {
        gui_destroy_surface(&g_menubar_blur_source);
        gui_destroy_surface(&g_dock_blur_source);
        syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->mb_blur_shm_id));
        syscall1(SYS_SHM_UNMAP, static_cast<uint64_t>(registry->dk_blur_shm_id));
        g_menubar_blur = {};
        g_dock_blur = {};
        return false;
    }

    g_menubar_blur.buffer = reinterpret_cast<uint32_t *>(mb_map);
    g_menubar_blur.width = g_screen.width;
    g_menubar_blur.height = static_cast<uint32_t>(menubar_h);
    g_menubar_blur.pitch = g_screen.width * 4u;
    g_menubar_blur.owns_buffer = false;

    g_dock_blur.buffer = reinterpret_cast<uint32_t *>(dk_map);
    g_dock_blur.width = dock_w;
    g_dock_blur.height = dock_h;
    g_dock_blur.pitch = dock_w * 4u;
    g_dock_blur.owns_buffer = false;

    memset(g_menubar_blur.buffer, 0, static_cast<size_t>(g_menubar_blur.pitch) * g_menubar_blur.height);
    memset(g_dock_blur.buffer, 0, static_cast<size_t>(g_dock_blur.pitch) * g_dock_blur.height);
    memset(g_menubar_blur_source.buffer, 0,
           static_cast<size_t>(g_menubar_blur_source.pitch) * g_menubar_blur_source.height);
    memset(g_dock_blur_source.buffer, 0, static_cast<size_t>(g_dock_blur_source.pitch) * g_dock_blur_source.height);

    g_menubar_blur_dirty = true;
    g_dock_blur_dirty = true;
    return true;
}

void capture_shell_backdrop_for_rect(const DirtyRect &rect, Registry *registry)
{
    mark_shell_blur_dirty(registry, rect);
}

static inline bool rect_touch_or_overlap(const DirtyRect &a, const DirtyRect &b)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    return a.x <= b.x + b.w && a.x + a.w >= b.x &&
           a.y <= b.y + b.h && a.y + a.h >= b.y;
}

void flush_shell_blur_updates(Registry *registry)
{
    if (!registry)
        return;

    if (registry->transparency_level >= 255) {
        registry->mb_blur_generation = 0;
        registry->dk_blur_generation = 0;
        g_menubar_blur_dirty = false;
        g_dock_blur_dirty = false;
        clear_blur_dirty_rects(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count);
        clear_blur_dirty_rects(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count);
        asm volatile("sfence" ::: "memory");
        return;
    }

    bool active_drag = g_input.pointer_down && g_input.drag_index >= 2;
    bool hover_resize = g_input.hover_resize_edges != RESIZE_NONE;

    static bool s_was_dragging = false;
    if (active_drag || hover_resize) {
        s_was_dragging = true;
        return;
    }
    if (s_was_dragging) {
        s_was_dragging = false;

        int menubar_h = wm_menubar_h();
        DirtyRect menubar_rect = {0, 0, static_cast<int>(g_screen.width), menubar_h};

        DirtyRect dock_rect = {};
        bool has_dock = false;
        if (g_dock_blur_source.buffer && registry->window_count > 1) {
            const WindowEntry &we1 = registry->windows[1];
            dock_rect = {we1.x, we1.y, we1.w, we1.h};
            has_dock = true;
        }

        bool intersects_menubar = false;
        bool intersects_dock = false;
        for (int i = WM_FIRST_USER_WINDOW; i < g_window_count; i++) {
            if (is_window_visible(g_windows[i])) {
                DirtyRect w_rect = window_outer_bounds(g_windows[i]);

                if (rect_touch_or_overlap(w_rect, menubar_rect)) {
                    intersects_menubar = true;
                }
                if (has_dock && rect_touch_or_overlap(w_rect, dock_rect)) {
                    intersects_dock = true;
                }
            }
        }

        if (intersects_menubar) {
            g_menubar_blur_dirty = true;
            add_blur_dirty_rect(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count, menubar_rect);
        }
        if (intersects_dock) {
            g_dock_blur_dirty = true;
            add_blur_dirty_rect(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count, dock_rect);
        }
    }

    g_last_blur_vblank = g_display_queue.vblank_count;
    bool is_light = registry->theme_mode == GUI_THEME_LIGHT;

    // Always process both surfaces every frame - no stagger
    if (g_menubar_blur_dirty && g_menubar_blur.buffer && g_menubar_blur_source.buffer) {
        if (g_menubar_blur_dirty_count > 0) {
            // Recompose only dirty regions
            for (int i = 0; i < g_menubar_blur_dirty_count; i++) {
                compose_desktop_for_blur(&g_menubar_blur_source, g_menubar_blur_dirty_rects[i], 0, 0);
            }
            clear_blur_dirty_rects(g_menubar_blur_dirty_rects, &g_menubar_blur_dirty_count);
        } else {
            // Full recomposition
            int menubar_h = wm_menubar_h();
            DirtyRect full = {0, 0, static_cast<int>(g_screen.width), menubar_h};
            compose_desktop_for_blur(&g_menubar_blur_source, full, 0, 0);
        }
        blur_surface_material(&g_menubar_blur_source, &g_menubar_blur, 48.0f, is_light ? 85 : 80, is_light ? 8 : 12);
        registry->mb_blur_generation = registry->mb_blur_generation + 1u;
        g_menubar_blur_dirty = false;
    }

    if (g_dock_blur_dirty && g_dock_blur.buffer && g_dock_blur_source.buffer) {
        if (g_dock_blur_dirty_count > 0) {
            // Recompose only dirty regions
            DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y,
                                   registry->windows[1].w, registry->windows[1].h};
            clip_dirty_rect_to_screen(dock_rect);
            for (int i = 0; i < g_dock_blur_dirty_count; i++) {
                compose_desktop_for_blur(&g_dock_blur_source, g_dock_blur_dirty_rects[i], dock_rect.x, dock_rect.y);
            }
            clear_blur_dirty_rects(g_dock_blur_dirty_rects, &g_dock_blur_dirty_count);
        } else {
            // Full recomposition
            DirtyRect dock_rect = {registry->windows[1].x, registry->windows[1].y,
                                   registry->windows[1].w, registry->windows[1].h};
            clip_dirty_rect_to_screen(dock_rect);
            compose_desktop_for_blur(&g_dock_blur_source, dock_rect, dock_rect.x, dock_rect.y);
        }
        blur_surface_material(&g_dock_blur_source, &g_dock_blur, 36.0f, is_light ? 82 : 78, is_light ? 8 : 10);
        registry->dk_blur_generation = registry->dk_blur_generation + 1u;
        g_dock_blur_dirty = false;
    }

    asm volatile("sfence" ::: "memory");
}

static uint32_t get_window_app_background(const Window &w)
{
    if (w.buffer && w.buffer_w > 0 && w.buffer_h > 0) {
        int sample_y = w.buffer_h > 4 ? 4 : 0;
        int sample_x = w.buffer_w > 10 ? 10 : 0;
        uint32_t pixel = w.buffer[(size_t)sample_y * (size_t)w.buffer_w + (size_t)sample_x];
        if ((pixel >> 24) != 0) {
            return 0xFF000000u | (pixel & 0x00FFFFFFu);
        }
    }
    return g_gui_style.app_bg ? g_gui_style.app_bg : 0xFF15171Au;
}

static bool is_color_dark(uint32_t color)
{
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return (r * 299 + g * 587 + b * 114) < 128000;
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
    int border = wm_frame_border();
    int detail_inset = gui_scaled_metric(1);
    if (detail_inset < 1)
        detail_inset = 1;

    int radius = gui_radius_xl();
    int body_inset = border + detail_inset;
    int frame_radius = radius - border;
    if (frame_radius < 0)
        frame_radius = 0;

    int body_radius = radius - body_inset;
    if (body_radius < 0)
        body_radius = 0;

    uint32_t outline_color = focused ? g_gui_style.border_hover : g_gui_style.border;
    uint32_t app_bg_color = get_window_app_background(w);
    uint32_t bar_color = app_bg_color;
    uint32_t body_color = app_bg_color;
    uint32_t title_color;
    if (is_color_dark(app_bg_color)) {
        title_color = focused ? 0xFFF2F2F0u : 0xFF9A9FA7u;
    } else {
        title_color = focused ? 0xFF15181Du : 0xFF6E7580u;
    }
    uint32_t frame_fill_color = mix_rgb(outline_color, body_color, focused ? 236 : 242);
    uint32_t inner_stroke_color = mix_rgb(body_color, 0xFFFFFFFFu, focused ? 18 : 12);
    
    int lx = (dst->buffer != g_backbuffer.buffer) ? 0 : w.x;
    int ly = (dst->buffer != g_backbuffer.buffer) ? 0 : w.y - title_bar_h;
    int sx = lx, sy = ly, sw = w.w, sh = w.h + title_bar_h;

    // Multi-layered soft drop shadow styling that fits exactly inside outer.w/outer.h bounds
    if (focused) {
        // Layer 1: Ambient soft blur shadow
        gui_fill_rounded_rect_clipped(dst, sx + gui_scaled_metric(1), sy + gui_scaled_metric(3), sw, sh,
                                      radius + gui_scaled_metric(2), 0x08000000u, clip);
        // Layer 2: Mid-range ambient shadow
        gui_fill_rounded_rect_clipped(dst, sx + gui_scaled_metric(1), sy + gui_scaled_metric(2), sw, sh,
                                      radius + gui_scaled_metric(1), 0x0C000000u, clip);
        // Layer 3: Direct key shadow
        gui_fill_rounded_rect_clipped(dst, sx, sy + gui_scaled_metric(1), sw, sh,
                                      radius, 0x14000000u, clip);
    } else {
        // Layer 1: Soft ambient shadow
        gui_fill_rounded_rect_clipped(dst, sx, sy + gui_scaled_metric(2), sw, sh,
                                      radius + gui_scaled_metric(1), 0x06000000u, clip);
        // Layer 2: Direct key shadow
        gui_fill_rounded_rect_clipped(dst, sx, sy + gui_scaled_metric(1), sw, sh,
                                      radius, 0x0A000000u, clip);
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

static void draw_window_decoration_buttons(Surface *dst, const Window &w, bool focused, int hovered_button)
{
    if (w.transparent)
        return;

    ensure_button_icons();

    uint32_t bar_color = get_window_app_background(w);
    uint32_t button_colors[3] = {g_gui_chrome.button_close, g_gui_chrome.button_minimize, g_gui_chrome.button_maximize};
    uint32_t button_outline = focused ? 0x65000000u : 0x38000000u;
    int button_size = wm_button_size();

    DirtyRect b0 = window_button_bounds(w, 0);
    int offset_x = b0.x, offset_y = b0.y;
    Surface *icons[3] = {&g_icon_close, &g_icon_minimize, &g_icon_maximize};

    for (int i = 0; i < 3; i++) {
        int cx = 0, cy = 0;
        window_button_center(w, i, &cx, &cy);
        cx -= offset_x;
        cy -= offset_y;
        int r = button_size / 2;

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

static void draw_window_decoration_buttons_clipped(Surface *dst, const Window &w, const DirtyRect &clip,
                                                   bool focused, int hovered_button)
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

        // Check if this button intersects the clip rect
        if (cx - r >= clip.x + clip.w || cx + r <= clip.x ||
            cy - r >= clip.y + clip.h || cy + r <= clip.y) {
            continue;
        }

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

    bool actively_resizing = g_input.pointer_down && g_input.drag_edges != RESIZE_NONE &&
                             g_input.drag_index >= 0 && g_input.drag_index < g_window_count &&
                             g_windows[g_input.drag_index].entry == w.entry;

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

static inline uint32_t blend_coverage_rgb(uint32_t dst_px, uint32_t src_px, uint8_t coverage)
{
    if (!coverage)
        return dst_px;
    if (coverage == 255)
        return src_px;
    uint32_t inv = 255u - coverage;
    uint32_t rb = (src_px & 0x00FF00FFu) * coverage + (dst_px & 0x00FF00FFu) * inv + 0x00800080u;
    rb = (rb + ((rb >> 8) & 0x00FF00FFu)) >> 8;
    uint32_t g_acc = ((src_px >> 8) & 0xFFu) * coverage + ((dst_px >> 8) & 0xFFu) * inv + 0x80u;
    uint32_t g = (g_acc + (g_acc >> 8)) >> 8;
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g << 8);
}

static void compute_bottom_corner_row(int local_y, int /*inner_w*/, int inner_h, int inner_r, uint8_t *out_mask)
{
    if (inner_r <= 0 || !out_mask)
        return;

    if (inner_r != g_cached_inner_r && inner_r <= 64) {
        for (int y = 0; y < inner_r; ++y) {
            for (int x = 0; x < inner_r; ++x) {
                g_bottom_corner_mask_lut[y][x] = gui_rounded_rect_coverage_local(
                    x, inner_r + y, inner_r * 2, inner_r * 2, inner_r, GUI_ROUNDED_EDGE_BOTTOM);
            }
        }
        g_cached_inner_r = inner_r;
    }

    int cy = local_y - (inner_h - inner_r);
    if (inner_r <= 64 && cy >= 0 && cy < inner_r) {
        for (int col = 0; col < inner_r; ++col) {
            out_mask[col] = g_bottom_corner_mask_lut[cy][col];
        }
    } else {
        for (int col = 0; col < inner_r; ++col) {
            out_mask[col] =
                gui_rounded_rect_coverage_local(col, inner_r + cy, inner_r * 2, inner_r * 2, inner_r, GUI_ROUNDED_EDGE_BOTTOM);
        }
    }
}

void draw_window_client_clipped(Surface *dst, const Window &w, const DirtyRect &clip)
{
    int ix, iy, iw, ih;
    if (!dst || !dst->buffer ||
        !gui_intersect_rect(w.x, w.y, w.w, w.h, clip.x, clip.y, clip.w, clip.h, &ix, &iy, &iw, &ih))
        return;

    const uint32_t dst_stride = dst->pitch / 4;
    int radius = gui_radius_xl();
    int border = wm_frame_border();
    int detail_inset = gui_scaled_metric(1);
    if (detail_inset < 1)
        detail_inset = 1;

    int body_inset = border + detail_inset;
    int inner_r = radius - body_inset;
    int inner_left = w.x + border;
    int inner_top = w.y;
    int inner_w = w.w - border * 2;
    int inner_h = w.h - border;

    if (inner_w <= 0 || inner_h <= 0) {
        inner_left = w.x;
        inner_top = w.y;
        inner_w = w.w;
        inner_h = w.h;
        inner_r = 0;
    }

    if (inner_r > inner_w / 2)
        inner_r = inner_w / 2;
    if (inner_r > inner_h / 2)
        inner_r = inner_h / 2;
    if (inner_r < 0)
        inner_r = 0;

    static constexpr int kCornerMaskMax = 64;
    if (inner_r > kCornerMaskMax)
        inner_r = kCornerMaskMax;

    int rx = 0, ry = 0, rw = 0, rh = 0;
    if (!gui_intersect_rect(ix, iy, iw, ih, inner_left, inner_top, inner_w, inner_h, &rx, &ry, &rw, &rh))
        return;

    const int rounded_start_y = inner_top + inner_h - inner_r;
    const int center_start_x = inner_left + inner_r;
    const int center_end_x = inner_left + inner_w - inner_r;
    const int dst_height_int = static_cast<int>(dst->height);

    uint8_t corner_mask[kCornerMaskMax];
    int corner_mask_y = -1;
    auto refresh_corner_mask = [&](int local_y) {
        if (inner_r <= 0 || local_y == corner_mask_y)
            return;
        compute_bottom_corner_row(local_y, inner_w, inner_h, inner_r, corner_mask);
        corner_mask_y = local_y;
    };

    int copy_x = 0, copy_y = 0, copy_w = 0, copy_h = 0;
    int src_x = 0, src_y = 0;
    bool has_buffer = (w.buffer_w > 0 && w.buffer_h > 0 && w.buffer != nullptr);
    if (has_buffer) {
        copy_x = w.transparent ? ix : rx;
        copy_y = w.transparent ? iy : ry;
        copy_w = w.transparent ? iw : rw;
        copy_h = w.transparent ? ih : rh;
        int client_left = w.transparent ? w.x : inner_left;
        int client_top = w.transparent ? w.y : inner_top;
        src_x = copy_x - client_left + w.scroll_x;
        src_y = copy_y - client_top + w.scroll_y;

        if (src_x < 0) {
            int delta = -src_x;
            copy_x += delta;
            copy_w -= delta;
            src_x = 0;
        }
        if (src_y < 0) {
            int delta = -src_y;
            copy_y += delta;
            copy_h -= delta;
            src_y = 0;
        }
        if (src_x + copy_w > w.buffer_w)
            copy_w = w.buffer_w - src_x;
        if (src_y + copy_h > w.buffer_h)
            copy_h = w.buffer_h - src_y;
        if (copy_w < 0) copy_w = 0;
        if (copy_h < 0) copy_h = 0;
    }

    if (!w.transparent) {
        const uint32_t fill = get_window_app_background(w);
        const int rect_right = rx + rw;
        if (inner_r <= 0) {
            for (int py = 0; py < rh; ++py) {
                const int dst_y = ry + py;
                if (dst_y < 0 || dst_y >= dst_height_int)
                    continue;
                uint32_t *dst_ptr = &dst->buffer[static_cast<size_t>(dst_y) * dst_stride];
                bool row_in_blit = (copy_w > 0 && copy_h > 0 && dst_y >= copy_y && dst_y < copy_y + copy_h);
                if (!row_in_blit) {
                    int span = rect_right - rx;
                    if (span <= 0)
                        continue;
                    uint32_t *p = &dst_ptr[rx];
                    int i = 0;
                    for (; i + 7 < span; i += 8) {
                        p[0] = fill; p[1] = fill; p[2] = fill; p[3] = fill;
                        p[4] = fill; p[5] = fill; p[6] = fill; p[7] = fill;
                        p += 8;
                    }
                    for (; i < span; ++i) *p++ = fill;
                } else {
                    int left_end = copy_x < rx ? rx : (copy_x > rect_right ? rect_right : copy_x);
                    for (int x = rx; x < left_end; ++x) dst_ptr[x] = fill;
                    int right_start = copy_x + copy_w;
                    if (right_start < rx) right_start = rx;
                    if (right_start > rect_right) right_start = rect_right;
                    for (int x = right_start; x < rect_right; ++x) dst_ptr[x] = fill;
                }
            }
        } else {
            for (int py = 0; py < rh; ++py) {
                const int dst_y = ry + py;
                if (dst_y < 0 || dst_y >= dst_height_int)
                    continue;

                uint32_t *dst_ptr = &dst->buffer[static_cast<size_t>(dst_y) * dst_stride];
                bool row_in_blit = (copy_w > 0 && copy_h > 0 && dst_y >= copy_y && dst_y < copy_y + copy_h);

                if (dst_y < rounded_start_y) {
                    if (!row_in_blit) {
                        int span = rect_right - rx;
                        if (span > 0) {
                            uint32_t *p = &dst_ptr[rx];
                            int i = 0;
                            for (; i + 7 < span; i += 8) {
                                p[0] = fill; p[1] = fill; p[2] = fill; p[3] = fill;
                                p[4] = fill; p[5] = fill; p[6] = fill; p[7] = fill;
                                p += 8;
                            }
                            for (; i < span; ++i) *p++ = fill;
                        }
                    } else {
                        int left_end = copy_x < rx ? rx : (copy_x > rect_right ? rect_right : copy_x);
                        for (int x = rx; x < left_end; ++x) dst_ptr[x] = fill;
                        int right_start = copy_x + copy_w;
                        if (right_start < rx) right_start = rx;
                        if (right_start > rect_right) right_start = rect_right;
                        for (int x = right_start; x < rect_right; ++x) dst_ptr[x] = fill;
                    }
                    continue;
                }

                refresh_corner_mask(dst_y - inner_top);

                int left_end = center_start_x < rect_right ? center_start_x : rect_right;
                if (!row_in_blit) {
                    for (int x = rx; x < left_end; ++x) {
                        int local = x - inner_left;
                        uint8_t coverage = corner_mask[local];
                        if (coverage == 255)
                            dst_ptr[x] = fill;
                        else if (coverage > 0)
                            dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                    }
                } else {
                    int cap_lo = copy_x < rx ? rx : (copy_x > rect_right ? rect_right : copy_x);
                    for (int x = rx; x < cap_lo; ++x) {
                        int local = x - inner_left;
                        uint8_t coverage = corner_mask[local];
                        if (coverage == 255)
                            dst_ptr[x] = fill;
                        else if (coverage > 0)
                            dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                    }
                    int cap_hi = copy_x + copy_w;
                    if (cap_hi < rx) cap_hi = rx;
                    if (cap_hi > left_end) cap_hi = left_end;
                    for (int x = cap_hi; x < left_end; ++x) {
                        int local = x - inner_left;
                        uint8_t coverage = corner_mask[local];
                        if (coverage == 255)
                            dst_ptr[x] = fill;
                        else if (coverage > 0)
                            dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                    }
                }

                int center_lo = rx > center_start_x ? rx : center_start_x;
                int center_hi = rect_right < center_end_x ? rect_right : center_end_x;
                if (!row_in_blit) {
                    int span = center_hi - center_lo;
                    if (span > 0) {
                        uint32_t *p = &dst_ptr[center_lo];
                        int i = 0;
                        for (; i + 7 < span; i += 8) {
                            p[0] = fill; p[1] = fill; p[2] = fill; p[3] = fill;
                            p[4] = fill; p[5] = fill; p[6] = fill; p[7] = fill;
                            p += 8;
                        }
                        for (; i < span; ++i) *p++ = fill;
                    }
                } else {
                    int lo = copy_x < center_lo ? center_lo : (copy_x > center_hi ? center_hi : copy_x);
                    for (int x = center_lo; x < lo; ++x) dst_ptr[x] = fill;
                    int hi_start = copy_x + copy_w;
                    if (hi_start < center_lo) hi_start = center_lo;
                    if (hi_start > center_hi) hi_start = center_hi;
                    for (int x = hi_start; x < center_hi; ++x) dst_ptr[x] = fill;
                }

                int right_lo = rx > center_end_x ? rx : center_end_x;
                if (!row_in_blit) {
                    for (int x = right_lo; x < rect_right; ++x) {
                        int local = inner_w - 1 - (x - inner_left);
                        uint8_t coverage = corner_mask[local];
                        if (coverage == 255)
                            dst_ptr[x] = fill;
                        else if (coverage > 0)
                            dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                    }
                } else {
                    int cap_lo = copy_x < right_lo ? right_lo : (copy_x > rect_right ? rect_right : copy_x);
                    for (int x = right_lo; x < cap_lo; ++x) {
                        int local = inner_w - 1 - (x - inner_left);
                        uint8_t coverage = corner_mask[local];
                        if (coverage == 255)
                            dst_ptr[x] = fill;
                        else if (coverage > 0)
                            dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                    }
                    int cap_hi = copy_x + copy_w;
                    if (cap_hi < right_lo) cap_hi = right_lo;
                    if (cap_hi > rect_right) cap_hi = rect_right;
                    for (int x = cap_hi; x < rect_right; ++x) {
                        int local = inner_w - 1 - (x - inner_left);
                        uint8_t coverage = corner_mask[local];
                        if (coverage == 255)
                            dst_ptr[x] = fill;
                        else if (coverage > 0)
                            dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], fill, coverage);
                    }
                }
            }
        }
    }

    if (!has_buffer || copy_w <= 0 || copy_h <= 0)
        return;

    if (w.transparent) {
        blit_alpha_blend_rect(&dst->buffer[static_cast<size_t>(copy_y) * dst_stride + copy_x], dst_stride,
                              &w.buffer[static_cast<size_t>(src_y) * w.buffer_w + src_x], w.buffer_w, copy_w, copy_h);
        return;
    }

    if (inner_r <= 0) {
        Surface src_surface = {w.buffer,
                               static_cast<uint32_t>(w.buffer_w),
                               static_cast<uint32_t>(w.buffer_h),
                               static_cast<uint32_t>(w.buffer_w) * 4,
                               false,
                               0};
        copy_surface_rect(dst, copy_x, copy_y, &src_surface, src_x, src_y, copy_w, copy_h);
        return;
    }

    const int copy_right = copy_x + copy_w;
    for (int py = 0; py < copy_h; ++py) {
        const int dst_y = copy_y + py;
        const int src_row_base = src_y + py;
        if (dst_y < 0 || dst_y >= dst_height_int || src_row_base < 0 || src_row_base >= w.buffer_h)
            continue;

        uint32_t *dst_ptr = &dst->buffer[static_cast<size_t>(dst_y) * dst_stride];
        const uint32_t *src_ptr = &w.buffer[static_cast<size_t>(src_row_base) * w.buffer_w];

        if (dst_y < rounded_start_y) {
            memcpy(&dst_ptr[copy_x], &src_ptr[src_x], static_cast<size_t>(copy_w) * sizeof(uint32_t));
            continue;
        }

        refresh_corner_mask(dst_y - inner_top);

        int left_end = center_start_x < copy_right ? center_start_x : copy_right;
        for (int x = copy_x; x < left_end; ++x) {
            int local = x - inner_left;
            int src_col = src_x + (x - copy_x);
            if (static_cast<unsigned>(src_col) >= static_cast<unsigned>(w.buffer_w))
                continue;

            uint8_t coverage = corner_mask[local];
            if (coverage == 255)
                dst_ptr[x] = src_ptr[src_col];
            else if (coverage > 0)
                dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], src_ptr[src_col], coverage);
        }

        int center_lo = copy_x > center_start_x ? copy_x : center_start_x;
        int center_hi = copy_right < center_end_x ? copy_right : center_end_x;
        if (center_hi > center_lo) {
            int src_col_start = src_x + (center_lo - copy_x);
            int center_w = center_hi - center_lo;
            if (src_col_start < 0) {
                center_lo += -src_col_start;
                center_w -= -src_col_start;
                src_col_start = 0;
            }
            if (src_col_start + center_w > w.buffer_w)
                center_w = w.buffer_w - src_col_start;
            if (center_w > 0) {
                memcpy(&dst_ptr[center_lo], &src_ptr[src_col_start], static_cast<size_t>(center_w) * sizeof(uint32_t));
            }
        }

        int right_lo = copy_x > center_end_x ? copy_x : center_end_x;
        for (int x = right_lo; x < copy_right; ++x) {
            int local = inner_w - 1 - (x - inner_left);
            int src_col = src_x + (x - copy_x);
            if (static_cast<unsigned>(src_col) >= static_cast<unsigned>(w.buffer_w))
                continue;

            uint8_t coverage = corner_mask[local];
            if (coverage == 255)
                dst_ptr[x] = src_ptr[src_col];
            else if (coverage > 0)
                dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], src_ptr[src_col], coverage);
        }
    }
}