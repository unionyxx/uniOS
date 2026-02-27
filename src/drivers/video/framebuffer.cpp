#include <drivers/video/framebuffer.h>
#include <drivers/video/font.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>

static struct limine_framebuffer* framebuffer = nullptr;
static uint32_t* backbuffer = nullptr;   // The RAM buffer (allocated after heap_init)
static uint32_t* frontbuffer = nullptr;  // The VRAM (Screen)
static uint32_t* target_buffer = nullptr; // Pointer to where we are currently drawing
static bool is_double_buffered = false;

// Dirty rectangle tracking - only copy changed pixels to VRAM
static int32_t dirty_min_x = 0;
static int32_t dirty_min_y = 0;
static int32_t dirty_max_x = 0;
static int32_t dirty_max_y = 0;
static bool full_redraw_needed = true;

// Helper to expand dirty rectangle to include a pixel
static inline void mark_dirty(int32_t x, int32_t y) {
    if (full_redraw_needed) return;
    if (x < dirty_min_x) dirty_min_x = x;
    if (y < dirty_min_y) dirty_min_y = y;
    if (x > dirty_max_x) dirty_max_x = x;
    if (y > dirty_max_y) dirty_max_y = y;
}

// Helper to mark a rectangular region as dirty (internal use)
static inline void mark_dirty_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (full_redraw_needed) return;
    if (x < dirty_min_x) dirty_min_x = x;
    if (y < dirty_min_y) dirty_min_y = y;
    if (x + w - 1 > dirty_max_x) dirty_max_x = x + w - 1;
    if (y + h - 1 > dirty_max_y) dirty_max_y = y + h - 1;
}

// Public function for external code that writes directly to the buffer
void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (full_redraw_needed) return;
    // Bounds checks
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    
    if (x < dirty_min_x) dirty_min_x = x;
    if (y < dirty_min_y) dirty_min_y = y;
    if (x + w - 1 > dirty_max_x) dirty_max_x = x + w - 1;
    if (y + h - 1 > dirty_max_y) dirty_max_y = y + h - 1;
}

void gfx_init(struct limine_framebuffer* fb) {
    framebuffer = fb;
    if (fb) {
        frontbuffer = (uint32_t*)fb->address;
        // Default to drawing directly to screen until heap is ready
        target_buffer = frontbuffer;
        backbuffer = nullptr;
        is_double_buffered = false;
        
        // Initialize dirty rect to full screen
        dirty_min_x = 0;
        dirty_min_y = 0;
        dirty_max_x = fb->width - 1;
        dirty_max_y = fb->height - 1;
        full_redraw_needed = true;
    }
}

void gfx_enable_double_buffering() {
    if (!framebuffer) return;
    if (is_double_buffered) return;  // Already enabled
    
    // Use pitch * height to account for hardware padding
    uint64_t bytes = framebuffer->pitch * framebuffer->height;
    
    backbuffer = (uint32_t*)malloc(bytes);
    
    if (backbuffer) {
        uint64_t* dst = (uint64_t*)backbuffer;
        uint64_t* src = (uint64_t*)frontbuffer;
        uint64_t count = bytes / 8;
        for (uint64_t i = 0; i < count; i++) dst[i] = src[i];

        target_buffer = backbuffer;
        is_double_buffered = true;
    }
}

void gfx_swap_buffers() {
    if (!is_double_buffered || !backbuffer || !frontbuffer) return;

    uint32_t pitch_u32 = framebuffer->pitch / 4;
    uint32_t width = framebuffer->width;
    uint32_t height = framebuffer->height;

    if (!full_redraw_needed && dirty_min_x > dirty_max_x) return;

    int32_t x1 = full_redraw_needed ? 0 : dirty_min_x;
    int32_t y1 = full_redraw_needed ? 0 : dirty_min_y;
    int32_t x2 = full_redraw_needed ? (int32_t)width - 1 : dirty_max_x;
    int32_t y2 = full_redraw_needed ? (int32_t)height - 1 : dirty_max_y;

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int32_t)width) x2 = width - 1;
    if (y2 >= (int32_t)height) y2 = height - 1;

    uint32_t copy_width = x2 - x1 + 1;
    if (copy_width == 0) return;

    // Full-width row optimization: uses bulk SSE2 transfer
    if (x1 == 0 && copy_width == width) {
        uint32_t* src = &backbuffer[y1 * pitch_u32];
        uint32_t* dst = &frontbuffer[y1 * pitch_u32];
        uint64_t total_u32 = (uint64_t)(y2 - y1 + 1) * pitch_u32;
        
        while (total_u32 > 0 && ((uint64_t)dst & 0xF)) {
            *dst++ = *src++;
            total_u32--;
        }
        
        while (total_u32 >= 8) {
            asm volatile(
                "prefetchnta 256(%0)\n\t"
                "movdqu   (%0), %%xmm0\n\t"
                "movdqu 16(%0), %%xmm1\n\t"
                "movntdq %%xmm0,   (%1)\n\t"
                "movntdq %%xmm1, 16(%1)\n\t"
                :
                : "r"(src), "r"(dst)
                : "memory"
            );
            src += 8;
            dst += 8;
            total_u32 -= 8;
        }
        
        while (total_u32--) {
            *dst++ = *src++;
        }
    } else {
        // Row-by-row copy for partial updates
        for (int32_t y = y1; y <= y2; y++) {
            uint32_t offset = y * pitch_u32 + x1;
            uint32_t* src = &backbuffer[offset];
            uint32_t* dst = &frontbuffer[offset];
            uint32_t count = copy_width;

            while (count > 0 && ((uint64_t)dst & 0xF)) {
                *dst++ = *src++;
                count--;
            }

            while (count >= 8) {
                asm volatile(
                    "prefetchnta 128(%0)\n\t"
                    "movdqu   (%0), %%xmm0\n\t"
                    "movdqu 16(%0), %%xmm1\n\t"
                    "movntdq %%xmm0,   (%1)\n\t"
                    "movntdq %%xmm1, 16(%1)\n\t"
                    :
                    : "r"(src), "r"(dst)
                    : "memory"
                );
                src += 8;
                dst += 8;
                count -= 8;
            }

            if (count >= 4) {
                asm volatile(
                    "movdqu (%0), %%xmm0\n\t"
                    "movntdq %%xmm0, (%1)\n\t"
                    :
                    : "r"(src), "r"(dst)
                    : "memory"
                );
                src += 4;
                dst += 4;
                count -= 4;
            }

            while (count--) {
                *dst++ = *src++;
            }
        }
    }

    asm volatile("sfence" ::: "memory");

    dirty_min_x = width;
    dirty_min_y = height;
    dirty_max_x = -1;
    dirty_max_y = -1;
    full_redraw_needed = false;
}

uint32_t* gfx_get_buffer() {
    return target_buffer;
}

void gfx_put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (!framebuffer) return;
    if (x < 0 || y < 0) return;
    if (x >= (int32_t)framebuffer->width || y >= (int32_t)framebuffer->height) return;
    
    // Draw to target buffer (RAM backbuffer or direct VRAM)
    uint32_t& pixel = target_buffer[y * (framebuffer->pitch / 4) + x];
    if (pixel == color) return;  // Skip if unchanged (saves dirty rect overhead)
    
    pixel = color;
    mark_dirty(x, y);
}

void gfx_clear(uint32_t color) {
    if (!framebuffer) return;
    
    uint64_t total_u32 = (uint64_t)(framebuffer->pitch / 4) * framebuffer->height;
    uint32_t* buf = target_buffer;
    
    // SSE2 optimization: fill 4 pixels at a time
    uint32_t color_arr[4] __attribute__((aligned(16))) = {color, color, color, color};
    
    // Align to 16 bytes
    while (total_u32 > 0 && ((uint64_t)buf & 0xF)) {
        *buf++ = color;
        total_u32--;
    }
    
    // SSE2 unrolled loop: 8 pixels per iteration
    while (total_u32 >= 8) {
        asm volatile(
            "movdqa (%0), %%xmm0\n\t"
            "movdqu %%xmm0,   (%1)\n\t"
            "movdqu %%xmm0, 16(%1)\n\t"
            :
            : "r"(color_arr), "r"(buf)
            : "memory"
        );
        buf += 8;
        total_u32 -= 8;
    }
    
    // Remaining pixels
    while (total_u32--) {
        *buf++ = color;
    }
    
    // Mark entire screen as needing redraw
    full_redraw_needed = true;
}

void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!framebuffer) return;
    
    // Clip to screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)framebuffer->width) w = framebuffer->width - x;
    if (y + h > (int32_t)framebuffer->height) h = framebuffer->height - y;
    
    if (w <= 0 || h <= 0) return;

    uint32_t pitch = framebuffer->pitch / 4;
    
    // SSE2 optimization: fill 4 pixels at a time
    uint32_t color_arr[4] __attribute__((aligned(16))) = {color, color, color, color};
    
    for (int32_t py = y; py < y + h; py++) {
        uint32_t* row = &target_buffer[py * pitch + x];
        int32_t count = w;
        
        // Align to 16 bytes
        while (count > 0 && ((uint64_t)row & 0xF)) {
            *row++ = color;
            count--;
        }
        
        // SSE2 unrolled loop: 8 pixels per iteration
        while (count >= 8) {
            asm volatile(
                "movdqa (%0), %%xmm0\n\t"
                "movdqu %%xmm0,   (%1)\n\t"
                "movdqu %%xmm0, 16(%1)\n\t"
                :
                : "r"(color_arr), "r"(row)
                : "memory"
            );
            row += 8;
            count -= 8;
        }
        
        // Remaining pixels
        while (count--) {
            *row++ = color;
        }
    }
    
    // Mark this rectangle as dirty
    mark_dirty_rect(x, y, w, h);
}

void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    gfx_fill_rect(x, y, w, 1, color);             // Top
    gfx_fill_rect(x, y + h - 1, w, 1, color);     // Bottom
    gfx_fill_rect(x, y, 1, h, color);             // Left
    gfx_fill_rect(x + w - 1, y, 1, h, color);     // Right
}

void gfx_draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t top_color, uint32_t bottom_color) {
    if (!framebuffer || h <= 0 || w <= 0) return;
    
    // Clip to screen
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int32_t)framebuffer->width) w = framebuffer->width - x;
    if (y + h > (int32_t)framebuffer->height) h = framebuffer->height - y;
    if (w <= 0 || h <= 0) return;

    // Extract RGB components
    uint32_t tr = (top_color >> 16) & 0xFF;
    uint32_t tg = (top_color >> 8) & 0xFF;
    uint32_t tb = top_color & 0xFF;
    
    uint32_t br = (bottom_color >> 16) & 0xFF;
    uint32_t bg = (bottom_color >> 8) & 0xFF;
    uint32_t bb = bottom_color & 0xFF;
    
    uint32_t pitch = framebuffer->pitch / 4;

    for (int32_t row = 0; row < h; row++) {
        // Linear interpolation
        uint32_t r = tr + ((br - tr) * row) / h;
        uint32_t g = tg + ((bg - tg) * row) / h;
        uint32_t b = tb + ((bb - tb) * row) / h;
        uint32_t color = (r << 16) | (g << 8) | b;
        
        uint32_t* row_ptr = &target_buffer[(y + row) * pitch + x];
        int32_t count = w;

        // Optimized row fill (simple loop but without function call overhead)
        // For gradients, SIMD is harder because color changes every row, 
        // but within a row it is constant.
        uint32_t color_arr[4] __attribute__((aligned(16))) = {color, color, color, color};
        
        while (count > 0 && ((uint64_t)row_ptr & 0xF)) {
            *row_ptr++ = color;
            count--;
        }
        while (count >= 8) {
            asm volatile("movdqa (%0), %%xmm0; movdqu %%xmm0, (%1); movdqu %%xmm0, 16(%1)"
                         :: "r"(color_arr), "r"(row_ptr) : "memory");
            row_ptr += 8; count -= 8;
        }
        while (count--) *row_ptr++ = color;
    }
    
    // Mark the entire gradient area as dirty at once
    mark_dirty_rect(x, y, w, h);
}

// Simple arrow cursor (12x19)
static const uint8_t cursor_data[] = {
    0b10000000, 0b00000000,
    0b11000000, 0b00000000,
    0b11100000, 0b00000000,
    0b11110000, 0b00000000,
    0b11111000, 0b00000000,
    0b11111100, 0b00000000,
    0b11111110, 0b00000000,
    0b11111111, 0b00000000,
    0b11111111, 0b10000000,
    0b11111111, 0b11000000,
    0b11111100, 0b00000000,
    0b11101100, 0b00000000,
    0b11000110, 0b00000000,
    0b10000110, 0b00000000,
    0b00000011, 0b00000000,
    0b00000011, 0b00000000,
    0b00000001, 0b10000000,
    0b00000001, 0b10000000,
    0b00000000, 0b00000000,
};

void gfx_draw_cursor(int32_t x, int32_t y) {
    for (int row = 0; row < 19; row++) {
        uint16_t bits = (cursor_data[row * 2] << 8) | cursor_data[row * 2 + 1];
        for (int col = 0; col < 12; col++) {
            if (bits & (0x8000 >> col)) {
                gfx_put_pixel(x + col, y + row, COLOR_WHITE);
            }
        }
    }
}

static void gfx_draw_char_no_dirty(int32_t x, int32_t y, char c, uint32_t color) {
    if (!framebuffer) return;
    
    // Bounds check - skip if completely off-screen
    if (x >= (int32_t)framebuffer->width || y >= (int32_t)framebuffer->height) return;
    if (x + 8 <= 0 || y + 16 <= 0) return;
    
    const uint8_t* glyph = font8x16[(uint8_t)c];
    uint32_t pitch_u32 = framebuffer->pitch / 4;
    uint32_t* row_ptr = target_buffer + (y * pitch_u32) + x;
    
    // Optimization: If the character is fully on-screen, use a faster loop
    bool fully_on_screen = (x >= 0 && y >= 0 && 
                            x + 8 <= (int32_t)framebuffer->width && 
                            y + 16 <= (int32_t)framebuffer->height);

    if (fully_on_screen) {
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            if (bits) {
                if (bits & 0x80) row_ptr[0] = color;
                if (bits & 0x40) row_ptr[1] = color;
                if (bits & 0x20) row_ptr[2] = color;
                if (bits & 0x10) row_ptr[3] = color;
                if (bits & 0x08) row_ptr[4] = color;
                if (bits & 0x04) row_ptr[5] = color;
                if (bits & 0x02) row_ptr[6] = color;
                if (bits & 0x01) row_ptr[7] = color;
            }
            row_ptr += pitch_u32;
        }
    } else {
        // Clipped version (slower but safe)
        for (int row = 0; row < 16; row++) {
            if (y + row >= 0 && y + row < (int32_t)framebuffer->height) {
                uint8_t bits = glyph[row];
                if (bits) {
                    for (int col = 0; col < 8; col++) {
                        if (x + col >= 0 && x + col < (int32_t)framebuffer->width) {
                            if ((bits >> (7 - col)) & 1) {
                                row_ptr[col] = color;
                            }
                        }
                    }
                }
            }
            row_ptr += pitch_u32;
        }
    }
}

void gfx_draw_char(int32_t x, int32_t y, char c, uint32_t color) {
    gfx_draw_char_no_dirty(x, y, c, color);
    mark_dirty_rect(x, y, 9, 16);
}

void gfx_clear_char(int32_t x, int32_t y, uint32_t bg_color) {
    gfx_fill_rect(x, y, 9, 16, bg_color);
}

void gfx_draw_string(int32_t x, int32_t y, const char *str, uint32_t color) {
    if (!str || !*str) return;
    
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    int32_t max_x = x;
    int32_t max_y = y + 16;
    
    while (*str) {
        if (*str == '\n') {
            cursor_x = x;
            cursor_y += 18;
            if (cursor_y + 16 > max_y) max_y = cursor_y + 16;
        } else {
            gfx_draw_char_no_dirty(cursor_x, cursor_y, *str, color);
            cursor_x += 9;
            if (cursor_x > max_x) max_x = cursor_x;
            if (cursor_y + 16 > max_y) max_y = cursor_y + 16;
        }
        str++;
    }
    
    mark_dirty_rect(x, y, max_x - x, max_y - y);
}

void gfx_draw_centered_text(const char* text, uint32_t color) {
    if (!framebuffer) return;
    
    int text_len = 0;
    const char* p = text;
    while (*p++) text_len++;
    
    int char_width = 9;
    int text_width = text_len * char_width;
    int center_x = (framebuffer->width - text_width) / 2;
    int center_y = (framebuffer->height - 16) / 2;
    
    gfx_draw_string(center_x, center_y, text, color);
}

void gfx_scroll_up(int pixels, uint32_t fill_color) {
    if (!framebuffer || pixels <= 0) return;
    
    uint64_t pitch = framebuffer->pitch / 4;
    uint64_t height = framebuffer->height;
    
    if ((uint64_t)pixels >= height) {
        gfx_clear(fill_color);
        return;
    }
    
    uint64_t rows_to_move = height - pixels;
    
    uint32_t* dst = target_buffer;
    uint32_t* src = target_buffer + (pixels * pitch);
    uint64_t total_u32 = rows_to_move * pitch;
    
    while (total_u32 >= 8) {
        asm volatile(
            "prefetchnta 256(%0)\n\t"
            "movdqu   (%0), %%xmm0\n\t"
            "movdqu 16(%0), %%xmm1\n\t"
            "movdqu %%xmm0,   (%1)\n\t"
            "movdqu %%xmm1, 16(%1)\n\t"
            :
            : "r"(src), "r"(dst)
            : "memory"
        );
        src += 8;
        dst += 8;
        total_u32 -= 8;
    }
    
    while (total_u32--) {
        *dst++ = *src++;
    }
    
    uint32_t* fill_start = target_buffer + (rows_to_move * pitch);
    uint64_t fill_count = pixels * pitch;
    
    uint32_t color_arr[4] __attribute__((aligned(16))) = {fill_color, fill_color, fill_color, fill_color};
    
    while (fill_count >= 8) {
        asm volatile(
            "movdqa (%0), %%xmm0\n\t"
            "movdqu %%xmm0,   (%1)\n\t"
            "movdqu %%xmm0, 16(%1)\n\t"
            :
            : "r"(color_arr), "r"(fill_start)
            : "memory"
        );
        fill_start += 8;
        fill_count -= 8;
    }
    
    while (fill_count--) {
        *fill_start++ = fill_color;
    }
    
    gfx_mark_dirty(0, 0, framebuffer->width, framebuffer->height);
}

uint64_t gfx_get_width() {
    return framebuffer ? framebuffer->width : 0;
}

uint64_t gfx_get_height() {
    return framebuffer ? framebuffer->height : 0;
}
