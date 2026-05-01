#pragma once
#include <boot/boot_info.h>
#include <stdint.h>

struct GfxRect
{
    int32_t x, y, w, h;
};

// Basic graphics primitives
void gfx_init(const BootFramebuffer *fb);
void gfx_clear(uint32_t color);
void gfx_put_pixel(int32_t x, int32_t y, uint32_t color);
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_fill_rect_to_buffer(uint32_t *buf, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_fill_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
void gfx_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
void gfx_draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t top_color, uint32_t bottom_color);
void gfx_draw_cursor(int32_t x, int32_t y);
void gfx_draw_char(int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_char_to_buffer(uint32_t *buf, int32_t x, int32_t y, char c, uint32_t color);
void gfx_draw_char_fixed(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg);
void gfx_clear_char(int32_t x, int32_t y, uint32_t bg_color);
void gfx_draw_string(int32_t x, int32_t y, const char *str, uint32_t color);
void gfx_draw_centered_text(const char *text, uint32_t color);
void gfx_scroll_up(int pixels, uint32_t fill_color);
void gfx_scroll_up_buffer(uint32_t *buf, int pixels, uint32_t fill_color);
void gfx_scroll_up_rect(int32_t x, int32_t y, int32_t w, int32_t h, int pixels, uint32_t fill_color);

// Double buffering
void gfx_enable_double_buffering();                                 // Call after heap_init
void gfx_swap_buffers(bool force = false);                          // Copy backbuffer to screen
uint32_t *gfx_get_buffer();                                         // Get current drawing target
uint32_t *gfx_get_backbuffer();                                     // Get global backbuffer
bool gfx_is_double_buffered();                                      // Check if double-buffering is active
void gfx_set_target_buffer(uint32_t *buf);                          // Set drawing target to screen-sized buffer
void gfx_set_target_surface(uint32_t *buf, uint32_t w, uint32_t h); // Set drawing target to arbitrary surface
void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h);    // Mark region as needing redraw

// High-performance line copy
void gfx_copy_line(uint32_t *dst, const uint32_t *src, uint32_t count);
void gfx_copy_line_nt(uint32_t *dst, const uint32_t *src, uint32_t count); // Non-temporal (for VRAM)
void gfx_copy_rect(uint32_t *dst, uint32_t dst_pitch, int32_t dx, int32_t dy, const uint32_t *src, uint32_t src_pitch,
                   int32_t sx, int32_t sy, int32_t w, int32_t h);
void gfx_copy_rect_nt(uint32_t *dst, uint32_t dst_pitch, int32_t dx, int32_t dy, const uint32_t *src,
                      uint32_t src_pitch, int32_t sx, int32_t sy, int32_t w, int32_t h);

uint64_t gfx_get_width();
uint64_t gfx_get_height();

// UI Colors
#include <kernel/theme.h>
