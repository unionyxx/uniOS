#include <drivers/video/font.h>
#include <drivers/video/framebuffer.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <libk/kstring.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_DIRTY_RECTS 128
static GfxRect dirty_rects[MAX_DIRTY_RECTS];
static int num_dirty_rects = 0;
static bool full_redraw_needed = true;

static const BootFramebuffer *framebuffer = nullptr;
static uint32_t *backbuffer = nullptr;    // The RAM buffer (allocated after heap_init)
uint32_t *g_front_buffer = nullptr;       // The VRAM (Screen)
static uint32_t *target_buffer = nullptr; // Pointer to where we are currently drawing
static uint32_t target_width = 0;
static uint32_t target_height = 0;
static uint32_t target_pitch_u32 = 0; // Framebuffer pitch in uint32_t units (handles framebuffer padding)
bool g_gfx_double_buffered = false;

static inline int32_t clamp_i64_to_i32(int64_t value);

static bool framebuffer_geometry_valid(const BootFramebuffer *fb)
{
    if (!fb || fb->bpp != 32 || fb->address == 0 || fb->width == 0 || fb->height == 0)
        return false;
    if (fb->width > (uint64_t)INT32_MAX || fb->height > (uint64_t)INT32_MAX)
        return false;
    if ((fb->pitch & 3u) != 0)
        return false;
    if ((uint64_t)fb->pitch < (uint64_t)fb->width * sizeof(uint32_t))
        return false;
    if ((uint64_t)fb->pitch / sizeof(uint32_t) > (uint64_t)UINT32_MAX)
        return false;
    if ((uint64_t)fb->pitch * (uint64_t)fb->height > (uint64_t)SIZE_MAX)
        return false;
    return true;
}

static inline bool clip_rect_to_bounds(int32_t *x, int32_t *y, int32_t *w, int32_t *h, uint32_t bounds_w,
                                       uint32_t bounds_h)
{
    if (!x || !y || !w || !h || *w <= 0 || *h <= 0 || bounds_w == 0 || bounds_h == 0)
        return false;

    int64_t x1 = *x;
    int64_t y1 = *y;
    int64_t x2 = x1 + (int64_t)*w;
    int64_t y2 = y1 + (int64_t)*h;

    if (x1 < 0)
        x1 = 0;
    if (y1 < 0)
        y1 = 0;
    if (x2 > (int64_t)bounds_w)
        x2 = bounds_w;
    if (y2 > (int64_t)bounds_h)
        y2 = bounds_h;
    if (x2 <= x1 || y2 <= y1)
        return false;

    *x = clamp_i64_to_i32(x1);
    *y = clamp_i64_to_i32(y1);
    *w = clamp_i64_to_i32(x2 - x1);
    *h = clamp_i64_to_i32(y2 - y1);
    return true;
}

static inline bool clip_rect_to_target(int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
    return clip_rect_to_bounds(x, y, w, h, target_width, target_height);
}

static void restore_default_target()
{
    target_buffer = backbuffer ? backbuffer : g_front_buffer;
    if (!framebuffer) {
        target_width = 0;
        target_height = 0;
        target_pitch_u32 = 0;
        return;
    }

    target_width = static_cast<uint32_t>(framebuffer->width);
    target_height = static_cast<uint32_t>(framebuffer->height);
    target_pitch_u32 = static_cast<uint32_t>(framebuffer->pitch / 4);
}

static inline int32_t clamp_i64_to_i32(int64_t value)
{
    if (value < INT32_MIN)
        return INT32_MIN;
    if (value > INT32_MAX)
        return INT32_MAX;
    return (int32_t)value;
}

static inline bool rect_contains(const GfxRect &outer, const GfxRect &inner)
{
    if (outer.w <= 0 || outer.h <= 0 || inner.w <= 0 || inner.h <= 0)
        return false;
    return inner.x >= outer.x && inner.y >= outer.y && (int64_t)inner.x + inner.w <= (int64_t)outer.x + outer.w &&
           (int64_t)inner.y + inner.h <= (int64_t)outer.y + outer.h;
}

static inline bool rect_overlaps_or_touches(const GfxRect &a, const GfxRect &b)
{
    if (a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0)
        return false;
    return (int64_t)a.x <= (int64_t)b.x + b.w && (int64_t)b.x <= (int64_t)a.x + a.w &&
           (int64_t)a.y <= (int64_t)b.y + b.h && (int64_t)b.y <= (int64_t)a.y + a.h;
}

static inline GfxRect rect_union(const GfxRect &a, const GfxRect &b)
{
    int64_t x1 = a.x < b.x ? a.x : b.x;
    int64_t y1 = a.y < b.y ? a.y : b.y;
    int64_t x2 = ((int64_t)a.x + a.w) > ((int64_t)b.x + b.w) ? ((int64_t)a.x + a.w) : ((int64_t)b.x + b.w);
    int64_t y2 = ((int64_t)a.y + a.h) > ((int64_t)b.y + b.h) ? ((int64_t)a.y + a.h) : ((int64_t)b.y + b.h);
    return {clamp_i64_to_i32(x1), clamp_i64_to_i32(y1), clamp_i64_to_i32(x2 - x1), clamp_i64_to_i32(y2 - y1)};
}

static inline void mark_dirty_rect(int32_t x, int32_t y, int32_t w, int32_t h);

// Helper to expand dirty rectangle to include a pixel
static inline void mark_dirty(int32_t x, int32_t y)
{
    mark_dirty_rect(x, y, 1, 1);
}

static inline void mark_dirty_if_backbuffer(int32_t x, int32_t y, int32_t w = 1, int32_t h = 1)
{
    if (target_buffer == backbuffer)
        mark_dirty_rect(x, y, w, h);
}

// Helper to mark a rectangular region as dirty (internal use)
static inline void mark_dirty_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (full_redraw_needed || w <= 0 || h <= 0)
        return;

    if (framebuffer &&
        !clip_rect_to_bounds(&x, &y, &w, &h, (uint32_t)framebuffer->width, (uint32_t)framebuffer->height))
        return;

    GfxRect candidate = {x, y, w, h};
    for (int i = 0; i < num_dirty_rects; i++) {
        if (rect_contains(dirty_rects[i], candidate))
            return;
        if (rect_overlaps_or_touches(dirty_rects[i], candidate)) {
            dirty_rects[i] = rect_union(dirty_rects[i], candidate);
            candidate = dirty_rects[i];
            for (int j = i + 1; j < num_dirty_rects;) {
                if (rect_overlaps_or_touches(dirty_rects[j], candidate)) {
                    candidate = rect_union(candidate, dirty_rects[j]);
                    dirty_rects[i] = candidate;
                    dirty_rects[j] = dirty_rects[num_dirty_rects - 1];
                    num_dirty_rects--;
                    continue;
                }
                j++;
            }
            return;
        }
    }

    if (num_dirty_rects < MAX_DIRTY_RECTS) {
        dirty_rects[num_dirty_rects++] = candidate;
    } else {
        full_redraw_needed = true;
    }
}

// Public function for external code that writes directly to the buffer
void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h)
{
    mark_dirty_rect(x, y, w, h);
}

void gfx_copy_line(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;

    // SSE2 optimized 128-bit copy. The public count is 32-bit pixels, but byte
    // math is kept in size_t so large pitch copies cannot wrap before clipping.
    size_t bytes = (size_t)count * sizeof(uint32_t);
    size_t i = 0;
    while (i + 16 <= bytes) {
        asm volatile("movdqu (%1, %2), %%xmm0\n\t"
                     "movdqu %%xmm0, (%0, %2)\n\t"
                     :
                     : "r"(dst), "r"(src), "r"((uintptr_t)i)
                     : "memory", "xmm0");
        i += 16;
    }
    for (; i < bytes; i++)
        ((uint8_t *)dst)[i] = ((const uint8_t *)src)[i];
}

void gfx_copy_line_nt(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (!dst || !src || count == 0)
        return;

    uintptr_t dst_addr = (uintptr_t)dst;
    uintptr_t src_addr = (uintptr_t)src;
    uint32_t i = 0;

    while (i < count && ((dst_addr + (uintptr_t)i * 4u) & 0x0Fu) != 0) {
        dst[i] = src[i];
        i++;
    }

    uint32_t simd_count = (count - i) / 4u;
    for (uint32_t block = 0; block < simd_count; block++) {
        uintptr_t byte_offset = (uintptr_t)(i + block * 4u) * 4u;
        asm volatile("movdqu (%1), %%xmm0\n\t"
                     "movntdq %%xmm0, (%0)\n\t"
                     :
                     : "r"((void *)(dst_addr + byte_offset)), "r"((const void *)(src_addr + byte_offset))
                     : "memory", "xmm0");
    }
    i += simd_count * 4u;

    while (i < count) {
        dst[i] = src[i];
        i++;
    }
}

static inline void gfx_copy_line_wc_aware(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (count == 0)
        return;
    if (count < 64u) {
        gfx_copy_line(dst, src, count);
    } else {
        gfx_copy_line_nt(dst, src, count);
    }
}

static uint32_t g_font_mask[256][8];
static bool g_font_mask_initialized = false;

static void init_font_mask()
{
    if (g_font_mask_initialized)
        return;
    for (int i = 0; i < 256; i++) {
        g_font_mask[i][0] = (i & 0x80) ? 0xFFFFFFFF : 0;
        g_font_mask[i][1] = (i & 0x40) ? 0xFFFFFFFF : 0;
        g_font_mask[i][2] = (i & 0x20) ? 0xFFFFFFFF : 0;
        g_font_mask[i][3] = (i & 0x10) ? 0xFFFFFFFF : 0;
        g_font_mask[i][4] = (i & 0x08) ? 0xFFFFFFFF : 0;
        g_font_mask[i][5] = (i & 0x04) ? 0xFFFFFFFF : 0;
        g_font_mask[i][6] = (i & 0x02) ? 0xFFFFFFFF : 0;
        g_font_mask[i][7] = (i & 0x01) ? 0xFFFFFFFF : 0;
    }
    g_font_mask_initialized = true;
}

void gfx_init(const BootFramebuffer *fb)
{
    if (backbuffer) {
        free(backbuffer);
        backbuffer = nullptr;
    }
    g_gfx_double_buffered = false;

    if (!framebuffer_geometry_valid(fb)) {
        framebuffer = nullptr;
        g_front_buffer = nullptr;
        target_buffer = nullptr;
        target_width = 0;
        target_height = 0;
        target_pitch_u32 = 0;
        num_dirty_rects = 0;
        full_redraw_needed = true;
        return;
    }

    init_font_mask();

    framebuffer = fb;
    g_front_buffer = (uint32_t *)fb->address;
    restore_default_target();

    num_dirty_rects = 0;
    full_redraw_needed = true;
}

void gfx_enable_double_buffering()
{
    if (!framebuffer || !g_front_buffer || g_gfx_double_buffered)
        return;

    uint64_t bytes64 = (uint64_t)framebuffer->pitch * (uint64_t)framebuffer->height;
    if (bytes64 == 0 || bytes64 > (uint64_t)SIZE_MAX)
        return;

    uint32_t pitch_u32 = (uint32_t)(framebuffer->pitch / sizeof(uint32_t));
    uint32_t *new_backbuffer = (uint32_t *)malloc((size_t)bytes64);
    if (!new_backbuffer)
        return;

    for (uint32_t y = 0; y < (uint32_t)framebuffer->height; y++)
        gfx_copy_line(new_backbuffer + (size_t)y * pitch_u32, g_front_buffer + (size_t)y * pitch_u32, pitch_u32);

    backbuffer = new_backbuffer;
    g_gfx_double_buffered = true;
    restore_default_target();
    full_redraw_needed = true;
}

void gfx_swap_buffers(bool force)
{
    if (!g_gfx_double_buffered || !framebuffer || !backbuffer || !g_front_buffer)
        return;

    uint32_t pitch_u32 = framebuffer->pitch / 4;
    uint32_t width = framebuffer->width;
    uint32_t height = framebuffer->height;

    bool full = full_redraw_needed || force;
    if (!full && num_dirty_rects == 0)
        return;

    if (full) {
        for (uint32_t y = 0; y < height; y++) {
            size_t offset = (size_t)y * pitch_u32;
            gfx_copy_line_wc_aware(&g_front_buffer[offset], &backbuffer[offset], width);
        }
    } else {
        for (int i = 0; i < num_dirty_rects; i++) {
            GfxRect &r = dirty_rects[i];
            int32_t x1 = r.x;
            int32_t y1 = r.y;
            int32_t x2 = r.x + r.w - 1;
            int32_t y2 = r.y + r.h - 1;

            if (x1 < 0)
                x1 = 0;
            if (y1 < 0)
                y1 = 0;
            if (x2 >= (int32_t)width)
                x2 = width - 1;
            if (y2 >= (int32_t)height)
                y2 = height - 1;

            int32_t copy_width = x2 - x1 + 1;
            if (copy_width <= 0)
                continue;

            for (int32_t y = y1; y <= y2; y++) {
                size_t offset = (size_t)y * pitch_u32 + (size_t)x1;
                gfx_copy_line_wc_aware(&g_front_buffer[offset], &backbuffer[offset], copy_width);
            }
        }
    }

    // Compiler memory barrier and hardware fence for Write-Combining memory
    asm volatile("sfence" ::: "memory");

    num_dirty_rects = 0;
    full_redraw_needed = false;
}

uint32_t *gfx_get_buffer()
{
    return target_buffer;
}

uint32_t *gfx_get_backbuffer()
{
    return backbuffer;
}

bool gfx_is_double_buffered()
{
    return g_gfx_double_buffered;
}

void gfx_set_target_buffer(uint32_t *buf)
{
    if (buf == nullptr || !framebuffer) {
        restore_default_target();
        return;
    }

    target_buffer = buf;
    target_width = (uint32_t)framebuffer->width;
    target_height = (uint32_t)framebuffer->height;
    target_pitch_u32 = (uint32_t)(framebuffer->pitch / sizeof(uint32_t));
}

void gfx_set_target_surface(uint32_t *buf, uint32_t w, uint32_t h)
{
    if (buf == nullptr || w == 0 || h == 0 || w > (uint32_t)INT32_MAX || h > (uint32_t)INT32_MAX ||
        (uint64_t)w * (uint64_t)h > ((uint64_t)SIZE_MAX / sizeof(uint32_t))) {
        restore_default_target();
        return;
    }

    target_buffer = buf;
    target_width = w;
    target_height = h;
    target_pitch_u32 = w;
}

void gfx_put_pixel(int32_t x, int32_t y, uint32_t color)
{
    if (!target_buffer || x < 0 || y < 0 || x >= (int32_t)target_width || y >= (int32_t)target_height)
        return;
    uint32_t &pixel = target_buffer[(size_t)y * target_pitch_u32 + (size_t)x];
    if (pixel == color)
        return;
    pixel = color;
    if (target_buffer == backbuffer)
        mark_dirty(x, y);
}

void gfx_clear(uint32_t color)
{
    if (!target_buffer || target_width == 0 || target_height == 0)
        return;
    gfx_fill_rect(0, 0, (int32_t)target_width, (int32_t)target_height, color);

    if (target_buffer == backbuffer)
        full_redraw_needed = true;
}

void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!target_buffer || !clip_rect_to_target(&x, &y, &w, &h))
        return;

    uint32_t pitch = target_pitch_u32;
    uint64_t color64 = ((uint64_t)color << 32) | color;

    for (int32_t py = y; py < y + h; py++) {
        uint32_t *row = &target_buffer[static_cast<uint32_t>(py) * pitch + static_cast<uint32_t>(x)];
        int32_t i = 0;

        // Align row start to 8-byte boundary for 64-bit stores
        if ((uintptr_t)row & 7) {
            row[i++] = color;
        }

        for (; i + 1 < w; i += 2) {
            kstring::memcpy(&row[i], &color64, 8);
        }
        for (; i < w; i++) {
            row[i] = color;
        }
    }
    if (target_buffer == backbuffer)
        mark_dirty_rect(x, y, w, h);
}

void gfx_fill_rect_to_buffer(uint32_t *buf, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (!buf || !framebuffer ||
        !clip_rect_to_bounds(&x, &y, &w, &h, (uint32_t)framebuffer->width, (uint32_t)framebuffer->height))
        return;

    uint32_t pitch = static_cast<uint32_t>(framebuffer->pitch / 4);
    uint64_t color64 = ((uint64_t)color << 32) | color;

    for (int32_t py = y; py < y + h; py++) {
        uint32_t *row = &buf[static_cast<uint32_t>(py) * pitch + static_cast<uint32_t>(x)];
        int32_t i = 0;
        if ((uintptr_t)row & 7)
            row[i++] = color;
        for (; i + 1 < w; i += 2)
            kstring::memcpy(&row[i], &color64, 8);
        for (; i < w; i++)
            row[i] = color;
    }
}

void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    int32_t right = clamp_i64_to_i32((int64_t)x + w - 1);
    int32_t bottom = clamp_i64_to_i32((int64_t)y + h - 1);
    gfx_fill_rect(x, y, w, 1, color);
    if (h > 1)
        gfx_fill_rect(x, bottom, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    if (w > 1)
        gfx_fill_rect(right, y, 1, h, color);
}

static inline uint32_t blend_rgb888(uint32_t dst, uint32_t src, uint8_t alpha)
{
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

    uint32_t r = (dr * inv + sr * alpha + 127u) / 255u;
    uint32_t g = (dg * inv + sg * alpha + 127u) / 255u;
    uint32_t b = (db * inv + sb * alpha + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// Integer square root (digit-by-digit). The kernel is built with
// -mgeneral-regs-only: no FPU/SSE in kernel code, so all rounded-corner math
// is fixed-point.
static inline uint64_t gfx_isqrt_u64(uint64_t value)
{
    if (value == 0)
        return 0;

    uint64_t op = value;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;
    while (one > op)
        one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res += one << 1;
        }
        res >>= 1;
        one >>= 2;
    }
    return res;
}

// Fixed-point disk coverage: returns 0-255 coverage for a disk of radius r,
// where the pixel center is at squared distance dist_sq_fp (in (1/256 pixel)^2
// units) from the disk center. Early-out skips the isqrt for fully-interior
// and fully-exterior pixels. 256-level analytic AA — no supersampling.
static uint8_t disk_coverage_fp(int64_t dist_sq_fp, int32_t r)
{
    int64_t r_fp = (int64_t)r * 256;
    int64_t r_in_fp = r_fp - 128;
    int64_t r_out_fp = r_fp + 128;

    if (dist_sq_fp <= r_in_fp * r_in_fp)
        return 255;
    if (dist_sq_fp >= r_out_fp * r_out_fp)
        return 0;

    int64_t dist_fp = (int64_t)gfx_isqrt_u64((uint64_t)dist_sq_fp);
    int64_t cov_fp = r_fp + 128 - dist_fp;
    if (cov_fp <= 0)
        return 0;
    if (cov_fp >= 256)
        return 255;
    return (uint8_t)((cov_fp * 255 + 128) / 256);
}

void gfx_fill_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (!target_buffer || w <= 0 || h <= 0) {
        return;
    }
    if (r <= 0) {
        gfx_fill_rect(x, y, w, h, color);
        return;
    }
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;

    uint32_t pitch = target_pitch_u32;
    int32_t row_start = y < 0 ? -y : 0;
    int32_t row_end = h;
    if ((int64_t)y + row_end > (int64_t)target_height)
        row_end = (int32_t)target_height - y;
    if (row_start < 0)
        row_start = 0;
    if (row_end > h)
        row_end = h;
    if (row_start >= row_end)
        return;

    for (int32_t row = row_start; row < row_end; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= (int32_t)target_height)
            continue;

        int32_t local_row;
        if (row < r) {
            local_row = row;
        } else if (row >= h - r) {
            local_row = h - 1 - row;
        } else {
            gfx_fill_rect(x, py, w, 1, color);
            continue;
        }

        // Corner row: fill middle at full opacity, per-pixel AA for corners.
        int32_t mid_start = r;
        int32_t mid_end = w - r;
        if (mid_end > mid_start)
            gfx_fill_rect(x + mid_start, py, mid_end - mid_start, 1, color);

        int64_t dy_fp = (int64_t)r * 256 - (int64_t)local_row * 256 - 128;
        int64_t dy_sq_fp = dy_fp * dy_fp;

        for (int32_t side = 0; side < 2; side++) {
            int32_t col_start = (side == 0) ? 0 : (w - r);
            int32_t col_end = (side == 0) ? r : w;
            if (col_start < 0)
                col_start = 0;
            if (col_end > w)
                col_end = w;

            for (int32_t col = col_start; col < col_end; col++) {
                int32_t local_col = (side == 0) ? col : (w - 1 - col);
                int64_t dx_fp = (int64_t)r * 256 - (int64_t)local_col * 256 - 128;
                int64_t dist_sq_fp = dx_fp * dx_fp + dy_sq_fp;

                uint8_t coverage = disk_coverage_fp(dist_sq_fp, r);
                if (coverage == 0)
                    continue;

                int32_t px = x + col;
                if (px < 0 || px >= (int32_t)target_width)
                    continue;

                uint32_t &dst = target_buffer[(size_t)py * pitch + (size_t)px];
                if (coverage == 255)
                    dst = color;
                else
                    dst = blend_rgb888(dst, color, coverage);
                mark_dirty_if_backbuffer(px, py);
            }
        }
    }
}

void gfx_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color)
{
    if (!target_buffer || w <= 0 || h <= 0) {
        return;
    }
    if (r <= 0 || w <= 2 || h <= 2) {
        gfx_draw_rect(x, y, w, h, color);
        return;
    }
    if (r > w / 2)
        r = w / 2;
    if (r > h / 2)
        r = h / 2;

    uint32_t pitch = target_pitch_u32;
    int32_t row_start = y < 0 ? -y : 0;
    int32_t row_end = h;
    if ((int64_t)y + row_end > (int64_t)target_height)
        row_end = (int32_t)target_height - y;
    if (row_start < 0)
        row_start = 0;
    if (row_end > h)
        row_end = h;
    if (row_start >= row_end)
        return;

    int32_t inner_r = r - 1;
    if (inner_r < 0)
        inner_r = 0;

    for (int32_t row = row_start; row < row_end; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= (int32_t)target_height)
            continue;

        int32_t local_row;
        if (row < r) {
            local_row = row;
        } else if (row >= h - r) {
            local_row = h - 1 - row;
        } else {
            // Non-corner row: left and right edge pixels only.
            int32_t px_l = x;
            int32_t px_r = x + w - 1;
            if (px_l >= 0 && px_l < (int32_t)target_width) {
                target_buffer[(size_t)py * pitch + (size_t)px_l] = color;
                mark_dirty_if_backbuffer(px_l, py);
            }
            if (px_r >= 0 && px_r < (int32_t)target_width) {
                target_buffer[(size_t)py * pitch + (size_t)px_r] = color;
                mark_dirty_if_backbuffer(px_r, py);
            }
            continue;
        }

        // Corner row: per-pixel stroke coverage (outer - inner disk).
        int64_t dy_fp = (int64_t)r * 256 - (int64_t)local_row * 256 - 128;
        int64_t dy_sq_fp = dy_fp * dy_fp;

        for (int32_t col = 0; col < w; col++) {
            int32_t px = x + col;
            if (px < 0 || px >= (int32_t)target_width)
                continue;

            uint8_t outer_cov;
            uint8_t inner_cov;

            int32_t local_col = (col < r) ? col : (w - 1 - col);
            if (local_col < r) {
                int64_t dx_fp = (int64_t)r * 256 - (int64_t)local_col * 256 - 128;
                int64_t dist_sq_fp = dx_fp * dx_fp + dy_sq_fp;
                outer_cov = disk_coverage_fp(dist_sq_fp, r);
                inner_cov = (inner_r > 0) ? disk_coverage_fp(dist_sq_fp, inner_r) : 0;
            } else {
                outer_cov = 255;
                inner_cov = (row >= 1 && row < h - 1) ? 255 : 0;
            }

            uint8_t coverage = (outer_cov > inner_cov) ? (uint8_t)(outer_cov - inner_cov) : 0;
            if (coverage == 0)
                continue;

            uint32_t &dst = target_buffer[(size_t)py * pitch + (size_t)px];
            if (coverage == 255)
                dst = color;
            else
                dst = blend_rgb888(dst, color, coverage);
            mark_dirty_if_backbuffer(px, py);
        }
    }
}

void gfx_draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t top_color, uint32_t bottom_color)
{
    if (!target_buffer || h <= 0 || w <= 0)
        return;

    int32_t original_y = y;
    int32_t original_h = h;
    if (!clip_rect_to_target(&x, &y, &w, &h))
        return;

    int32_t tr = (top_color >> 16) & 0xFF, tg = (top_color >> 8) & 0xFF, tb = top_color & 0xFF;
    int32_t br = (bottom_color >> 16) & 0xFF, bg = (bottom_color >> 8) & 0xFF, bb = bottom_color & 0xFF;

    int32_t steps = original_h > 1 ? (original_h - 1) : 1;
    int32_t dr = ((br - tr) << 16) / steps, dg = ((bg - tg) << 16) / steps, db = ((bb - tb) << 16) / steps;

    uint32_t pitch = target_pitch_u32;
    for (int32_t row = 0; row < h; row++) {
        int32_t source_row = (y - original_y) + row;
        int32_t r_fixed = (tr << 16) + dr * source_row;
        int32_t g_fixed = (tg << 16) + dg * source_row;
        int32_t b_fixed = (tb << 16) + db * source_row;
        uint32_t color = (static_cast<uint32_t>(0xFF) << 24) | (static_cast<uint32_t>(r_fixed >> 16) << 16) |
                         (static_cast<uint32_t>(g_fixed >> 16) << 8) | static_cast<uint32_t>(b_fixed >> 16);
        uint32_t *row_ptr = &target_buffer[static_cast<uint32_t>(y + row) * pitch + static_cast<uint32_t>(x)];
        uint64_t color64 = ((uint64_t)color << 32) | color;

        int32_t i = 0;
        if ((uintptr_t)row_ptr & 7)
            row_ptr[i++] = color;
        for (; i + 1 < w; i += 2)
            kstring::memcpy(&row_ptr[i], &color64, 8);
        for (; i < w; i++)
            row_ptr[i] = color;
    }

    if (target_buffer == backbuffer)
        mark_dirty_rect(x, y, w, h);
}

void gfx_draw_cursor(int32_t x, int32_t y)
{
    if (!target_buffer)
        return;
    // 0 = Transparent, 1 = White (Outline), 2 = Black (Fill)
    static const uint8_t cursor_data[16][16] = {
        {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0},
        {1, 2, 2, 2, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 2, 2, 1, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0},
        {1, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0}};

    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 16; col++) {
            uint8_t pixel = cursor_data[row][col];
            if (pixel == 1)
                gfx_put_pixel(x + col, y + row, COLOR_WHITE);
            else if (pixel == 2)
                gfx_put_pixel(x + col, y + row, COLOR_BLACK);
        }
    }
}

static void gfx_draw_char_no_dirty(int32_t x, int32_t y, char c, uint32_t color)
{
    gfx_draw_char_to_buffer(target_buffer, x, y, c, color);
}

void gfx_draw_char_to_buffer(uint32_t *buf, int32_t x, int32_t y, char c, uint32_t color)
{
    if (!buf || target_width == 0 || target_height == 0 || target_pitch_u32 == 0 || x >= (int32_t)target_width ||
        y >= (int32_t)target_height || (int64_t)x + 8 <= 0 || (int64_t)y + 16 <= 0)
        return;

    init_font_mask();

    const uint8_t *glyph = font8x16[(uint8_t)c];
    uint32_t pitch_u32 = target_pitch_u32;

    for (int row = 0; row < 16; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= (int32_t)target_height)
            continue;

        uint32_t *row_ptr = buf + (static_cast<uint32_t>(py) * pitch_u32);
        uint8_t bits = glyph[row];
        if (!bits)
            continue;

        if (x >= 0 && x + 8 <= (int32_t)target_width) {
            const uint32_t *mask = g_font_mask[bits];
            uint32_t *dst = row_ptr + static_cast<uint32_t>(x);
            if (mask[0])
                dst[0] = color;
            if (mask[1])
                dst[1] = color;
            if (mask[2])
                dst[2] = color;
            if (mask[3])
                dst[3] = color;
            if (mask[4])
                dst[4] = color;
            if (mask[5])
                dst[5] = color;
            if (mask[6])
                dst[6] = color;
            if (mask[7])
                dst[7] = color;
            continue;
        }

        for (int col = 0; col < 8; col++) {
            int32_t px = x + col;
            if (px >= 0 && px < (int32_t)target_width && ((bits >> (7 - col)) & 1u))
                row_ptr[static_cast<uint32_t>(px)] = color;
        }
    }
}

void gfx_draw_char(int32_t x, int32_t y, char c, uint32_t color)
{
    if (!target_buffer)
        return;
    gfx_draw_char_no_dirty(x, y, c, color);
    if (target_buffer == backbuffer)
        mark_dirty_rect(x, y, 8, 16);
}

void gfx_draw_char_fixed(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg)
{
    if (!target_buffer || target_width == 0 || target_height == 0 || target_pitch_u32 == 0 ||
        x >= (int32_t)target_width || y >= (int32_t)target_height || (int64_t)x + 8 <= 0 || (int64_t)y + 16 <= 0)
        return;

    init_font_mask();

    const uint8_t *glyph = font8x16[(uint8_t)c];
    uint32_t pitch_u32 = target_pitch_u32;

    for (int row = 0; row < 16; row++) {
        int32_t py = y + row;
        if (py < 0 || py >= (int32_t)target_height)
            continue;

        uint32_t *row_ptr = target_buffer + (static_cast<uint32_t>(py) * pitch_u32);
        uint8_t bits = glyph[row];
        if (x >= 0 && x + 8 <= (int32_t)target_width) {
            const uint32_t *mask = g_font_mask[bits];
            uint32_t *dst = row_ptr + static_cast<uint32_t>(x);
            dst[0] = (mask[0] & fg) | (~mask[0] & bg);
            dst[1] = (mask[1] & fg) | (~mask[1] & bg);
            dst[2] = (mask[2] & fg) | (~mask[2] & bg);
            dst[3] = (mask[3] & fg) | (~mask[3] & bg);
            dst[4] = (mask[4] & fg) | (~mask[4] & bg);
            dst[5] = (mask[5] & fg) | (~mask[5] & bg);
            dst[6] = (mask[6] & fg) | (~mask[6] & bg);
            dst[7] = (mask[7] & fg) | (~mask[7] & bg);
            continue;
        }

        for (int col = 0; col < 8; col++) {
            int32_t px = x + col;
            if (px >= 0 && px < (int32_t)target_width)
                row_ptr[static_cast<uint32_t>(px)] = ((bits >> (7 - col)) & 1u) ? fg : bg;
        }
    }

    if (target_buffer == backbuffer)
        mark_dirty_rect(x, y, 8, 16);
}

void gfx_clear_char(int32_t x, int32_t y, uint32_t bg_color)
{
    gfx_fill_rect(x, y, 8, 16, bg_color);
}

void gfx_draw_string(int32_t x, int32_t y, const char *str, uint32_t color)
{
    if (!target_buffer || !str || !*str)
        return;

    int64_t cx = x;
    int64_t cy = y;
    int64_t mx = x;
    int64_t my = (int64_t)y + 16;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            cy += 18;
        } else {
            if (cx >= INT32_MIN && cx <= INT32_MAX && cy >= INT32_MIN && cy <= INT32_MAX)
                gfx_draw_char_no_dirty((int32_t)cx, (int32_t)cy, *str, color);
            cx += 9;
            if (cx > mx)
                mx = cx;
        }
        if (cy + 16 > my)
            my = cy + 16;
        if (cx > (int64_t)INT32_MAX + 1024 || cy > (int64_t)INT32_MAX + 1024 || cx < (int64_t)INT32_MIN - 1024 ||
            cy < (int64_t)INT32_MIN - 1024)
            break;
        str++;
    }
    if (target_buffer == backbuffer) {
        int32_t dx = clamp_i64_to_i32(mx - x);
        int32_t dy = clamp_i64_to_i32(my - y);
        mark_dirty_rect(x, y, dx, dy);
    }
}

void gfx_draw_centered_text(const char *text, uint32_t color)
{
    if (!target_buffer || !text)
        return;
    int32_t len = 0;
    const char *p = text;
    while (*p && len < (INT32_MAX / 9)) {
        len++;
        p++;
    }
    int32_t cx = (static_cast<int32_t>(target_width) - (len * 9)) / 2;
    int32_t cy = (static_cast<int32_t>(target_height) - 16) / 2;
    gfx_draw_string(cx, cy, text, color);
}

void gfx_scroll_up_rect(int32_t x, int32_t y, int32_t w, int32_t h, int pixels, uint32_t fill_color)
{
    if (!target_buffer || pixels <= 0 || w <= 0 || h <= 0)
        return;
    if (!clip_rect_to_target(&x, &y, &w, &h))
        return;
    if (pixels >= h) {
        gfx_fill_rect(x, y, w, h, fill_color);
        return;
    }

    uint32_t pitch = target_pitch_u32;
    for (int32_t py = y; py < y + h - pixels; py++) {
        uint32_t *dst = &target_buffer[(size_t)py * pitch + (size_t)x];
        uint32_t *src = &target_buffer[(size_t)(py + pixels) * pitch + (size_t)x];
        // The scrolled region overlaps in-place, so this must be memmove rather than memcpy.
        kstring::memmove(dst, src, (size_t)w * sizeof(uint32_t));
    }
    gfx_fill_rect(x, y + h - pixels, w, pixels, fill_color);
    if (target_buffer == backbuffer)
        mark_dirty_rect(x, y, w, h);
}

void gfx_scroll_up(int pixels, uint32_t fill_color)
{
    if (!target_buffer || pixels <= 0)
        return;

    uint64_t pitch = target_pitch_u32;
    uint64_t height = target_height;
    if ((uint64_t)pixels >= height) {
        gfx_clear(fill_color);
        return;
    }

    uint64_t rows = height - static_cast<uint64_t>(pixels);
    uint64_t move_words = rows * pitch;
    if (pitch == 0 || move_words > ((uint64_t)SIZE_MAX / sizeof(uint32_t)))
        return;
    uint32_t *dst = target_buffer;
    uint32_t *src = target_buffer + (static_cast<uint64_t>(pixels) * pitch);

    kstring::memmove(dst, src, (size_t)(move_words * sizeof(uint32_t)));
    gfx_fill_rect(0, static_cast<int32_t>(rows), static_cast<int32_t>(target_width), pixels, fill_color);

    if (target_buffer == backbuffer)
        full_redraw_needed = true;
}

void gfx_scroll_up_buffer(uint32_t *buf, int pixels, uint32_t fill_color)
{
    if (!buf || !framebuffer || pixels <= 0)
        return;
    uint32_t pitch = static_cast<uint32_t>(framebuffer->pitch / 4);
    uint32_t height = static_cast<uint32_t>(framebuffer->height);
    if (static_cast<uint32_t>(pixels) >= height) {
        uint64_t total = (uint64_t)pitch * height;
        if (total > ((uint64_t)SIZE_MAX / sizeof(uint32_t)))
            return;
        for (uint64_t i = 0; i < total; i++)
            buf[i] = fill_color;
        return;
    }
    uint32_t rows = height - static_cast<uint32_t>(pixels);
    // Row-wise WC-aware copy: the boot-log front buffer lives in
    // write-combined VRAM, where non-temporal stores beat a plain memmove.
    // Rows are copied forward; dst < src keeps each row copy disjoint.
    for (uint32_t row = 0; row < rows; row++) {
        gfx_copy_line_wc_aware(buf + (size_t)row * pitch, buf + ((size_t)row + pixels) * pitch, pitch);
    }
    gfx_fill_rect_to_buffer(buf, 0, (int32_t)rows, (int32_t)framebuffer->width, pixels, fill_color);
}

void gfx_copy_rect(uint32_t *dst, uint32_t dst_pitch, int32_t dx, int32_t dy, const uint32_t *src, uint32_t src_pitch,
                   int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    if (!dst || !src || dst_pitch == 0 || src_pitch == 0 || w <= 0 || h <= 0 || dx < 0 || dy < 0 || sx < 0 || sy < 0)
        return;
    for (int32_t row = 0; row < h; row++) {
        uint32_t *d = &dst[(size_t)(dy + row) * dst_pitch + (size_t)dx];
        const uint32_t *s = &src[(size_t)(sy + row) * src_pitch + (size_t)sx];
        gfx_copy_line(d, s, static_cast<uint32_t>(w));
    }
}

void gfx_copy_rect_nt(uint32_t *dst, uint32_t dst_pitch, int32_t dx, int32_t dy, const uint32_t *src,
                      uint32_t src_pitch, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    if (!dst || !src || dst_pitch == 0 || src_pitch == 0 || w <= 0 || h <= 0 || dx < 0 || dy < 0 || sx < 0 || sy < 0)
        return;
    for (int32_t row = 0; row < h; row++) {
        uint32_t *d = &dst[(size_t)(dy + row) * dst_pitch + (size_t)dx];
        const uint32_t *s = &src[(size_t)(sy + row) * src_pitch + (size_t)sx];
        gfx_copy_line_nt(d, s, static_cast<uint32_t>(w));
    }
}

uint64_t gfx_get_width()
{
    return framebuffer ? framebuffer->width : 0;
}
uint64_t gfx_get_height()
{
    return framebuffer ? framebuffer->height : 0;
}
