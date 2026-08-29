#include "wm_metrics.h"
#include "wm_render.h"
#include "wm_window.h"

static int g_cached_inner_r = -1;
static uint8_t g_bottom_corner_mask_lut[64][64] = {};

static inline uint32_t blend_coverage_rgb(uint32_t dst_px, uint32_t src_px, uint8_t coverage)
{
    return gui_blend_straight_opaque_dst_coverage(dst_px, src_px, coverage);
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
            out_mask[col] = gui_rounded_rect_coverage_local(col, inner_r + cy, inner_r * 2, inner_r * 2, inner_r,
                                                            GUI_ROUNDED_EDGE_BOTTOM);
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
    int border = gui_chrome_border();
    int detail_inset = gui_chrome_detail_inset();

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

        // The backing always holds a complete frame that covers the window:
        // clients publish a resized buffer only once it is fully drawn, and
        // the compositor flips the bounds only to the acknowledged size. The
        // valid source region is therefore the mapped buffer itself — no
        // committed-size tracking, no anchoring, no scaling. Scrolled
        // (content-sized) windows read their visible slice via the scroll
        // offset; everything else copies 1:1.
        //
        // During a resize the compositor reads from the WM-owned snapshot
        // (selected below), not the live backing the client may have already
        // reallocated to a smaller size. Clamp against the snapshot
        // dimensions so the blit covers the full visible bounds; without this
        // a shrunk live backing over-clamps copy_w/copy_h and the exposed
        // edges show only the fill color (black rectangles on bottom/right).
        int content_w_px = w.buffer_w;
        int content_h_px = w.buffer_h;
        if (w.resize_configure_pending && w.resize_snapshot.buffer) {
            content_w_px = static_cast<int>(w.resize_snapshot.width);
            content_h_px = w.resize_snapshot_y0 + static_cast<int>(w.resize_snapshot.height);
        }

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
        if (src_x + copy_w > content_w_px)
            copy_w = content_w_px - src_x;
        if (src_y + copy_h > content_h_px)
            copy_h = content_h_px - src_y;
        if (copy_w < 0)
            copy_w = 0;
        if (copy_h < 0)
            copy_h = 0;
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
                        p[0] = fill;
                        p[1] = fill;
                        p[2] = fill;
                        p[3] = fill;
                        p[4] = fill;
                        p[5] = fill;
                        p[6] = fill;
                        p[7] = fill;
                        p += 8;
                    }
                    for (; i < span; ++i)
                        *p++ = fill;
                } else {
                    int left_end = copy_x < rx ? rx : (copy_x > rect_right ? rect_right : copy_x);
                    for (int x = rx; x < left_end; ++x)
                        dst_ptr[x] = fill;
                    int right_start = copy_x + copy_w;
                    if (right_start < rx)
                        right_start = rx;
                    if (right_start > rect_right)
                        right_start = rect_right;
                    for (int x = right_start; x < rect_right; ++x)
                        dst_ptr[x] = fill;
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
                                p[0] = fill;
                                p[1] = fill;
                                p[2] = fill;
                                p[3] = fill;
                                p[4] = fill;
                                p[5] = fill;
                                p[6] = fill;
                                p[7] = fill;
                                p += 8;
                            }
                            for (; i < span; ++i)
                                *p++ = fill;
                        }
                    } else {
                        int left_end = copy_x < rx ? rx : (copy_x > rect_right ? rect_right : copy_x);
                        for (int x = rx; x < left_end; ++x)
                            dst_ptr[x] = fill;
                        int right_start = copy_x + copy_w;
                        if (right_start < rx)
                            right_start = rx;
                        if (right_start > rect_right)
                            right_start = rect_right;
                        for (int x = right_start; x < rect_right; ++x)
                            dst_ptr[x] = fill;
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
                    if (cap_hi < rx)
                        cap_hi = rx;
                    if (cap_hi > left_end)
                        cap_hi = left_end;
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
                            p[0] = fill;
                            p[1] = fill;
                            p[2] = fill;
                            p[3] = fill;
                            p[4] = fill;
                            p[5] = fill;
                            p[6] = fill;
                            p[7] = fill;
                            p += 8;
                        }
                        for (; i < span; ++i)
                            *p++ = fill;
                    }
                } else {
                    int lo = copy_x < center_lo ? center_lo : (copy_x > center_hi ? center_hi : copy_x);
                    for (int x = center_lo; x < lo; ++x)
                        dst_ptr[x] = fill;
                    int hi_start = copy_x + copy_w;
                    if (hi_start < center_lo)
                        hi_start = center_lo;
                    if (hi_start > center_hi)
                        hi_start = center_hi;
                    for (int x = hi_start; x < center_hi; ++x)
                        dst_ptr[x] = fill;
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
                    if (cap_hi < right_lo)
                        cap_hi = right_lo;
                    if (cap_hi > rect_right)
                        cap_hi = rect_right;
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

    // While a resize configure is outstanding the client is overwriting the
    // shared backing; present from the WM-owned snapshot of the last
    // committed frame instead so a partially rendered frame is never shown.
    // Outside a resize the backing is stable (geometry flips only land on
    // the ack), so it is read directly.
    const uint32_t *blit_buffer = w.buffer;
    int blit_w = w.buffer_w;
    int blit_h = w.buffer_h;
    int blit_y0 = 0;
    if (w.resize_configure_pending && w.resize_snapshot.buffer &&
        static_cast<int>(w.resize_snapshot.width) >= src_x + copy_w && src_y >= w.resize_snapshot_y0 &&
        src_y + copy_h <= w.resize_snapshot_y0 + static_cast<int>(w.resize_snapshot.height)) {
        blit_buffer = w.resize_snapshot.buffer;
        blit_w = static_cast<int>(w.resize_snapshot.pitch / 4);
        blit_h = static_cast<int>(w.resize_snapshot.height);
        blit_y0 = w.resize_snapshot_y0;
    }

    if (w.transparent) {
        blit_alpha_blend_rect(&dst->buffer[static_cast<size_t>(copy_y) * dst_stride + copy_x], dst_stride,
                              &w.buffer[static_cast<size_t>(src_y) * w.buffer_w + src_x], w.buffer_w, copy_w, copy_h);
        return;
    }

    if (inner_r <= 0) {
        Surface src_surface = {const_cast<uint32_t *>(blit_buffer),
                               static_cast<uint32_t>(blit_w),
                               static_cast<uint32_t>(blit_h),
                               static_cast<uint32_t>(blit_w) * 4,
                               false,
                               0};
        copy_surface_rect(dst, copy_x, copy_y, &src_surface, src_x, src_y - blit_y0, copy_w, copy_h);
        return;
    }

    const int copy_right = copy_x + copy_w;
    for (int py = 0; py < copy_h; ++py) {
        const int dst_y = copy_y + py;
        const int src_row_base = src_y + py - blit_y0;
        if (dst_y < 0 || dst_y >= dst_height_int || src_row_base < 0 || src_row_base >= blit_h)
            continue;

        uint32_t *dst_ptr = &dst->buffer[static_cast<size_t>(dst_y) * dst_stride];
        const uint32_t *src_ptr = &blit_buffer[static_cast<size_t>(src_row_base) * blit_w];

        if (dst_y < rounded_start_y) {
            memcpy(&dst_ptr[copy_x], &src_ptr[src_x], static_cast<size_t>(copy_w) * sizeof(uint32_t));
            continue;
        }

        refresh_corner_mask(dst_y - inner_top);

        int left_end = center_start_x < copy_right ? center_start_x : copy_right;
        for (int x = copy_x; x < left_end; ++x) {
            int local = x - inner_left;
            int src_col = src_x + (x - copy_x);
            if (static_cast<unsigned>(src_col) >= static_cast<unsigned>(blit_w))
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
            if (src_col_start + center_w > blit_w)
                center_w = blit_w - src_col_start;
            if (center_w > 0) {
                memcpy(&dst_ptr[center_lo], &src_ptr[src_col_start], static_cast<size_t>(center_w) * sizeof(uint32_t));
            }
        }

        int right_lo = copy_x > center_end_x ? copy_x : center_end_x;
        for (int x = right_lo; x < copy_right; ++x) {
            int local = inner_w - 1 - (x - inner_left);
            int src_col = src_x + (x - copy_x);
            if (static_cast<unsigned>(src_col) >= static_cast<unsigned>(blit_w))
                continue;

            uint8_t coverage = corner_mask[local];
            if (coverage == 255)
                dst_ptr[x] = src_ptr[src_col];
            else if (coverage > 0)
                dst_ptr[x] = blend_coverage_rgb(dst_ptr[x], src_ptr[src_col], coverage);
        }
    }
}
