#pragma once
#include <stdint.h>
#include <boot/limine.h>

// Basic graphics primitives
void gfx_init(struct limine_framebuffer* fb);
void gfx_clear(uint32_t color);
void gfx_put_pixel(int32_t x, int32_t y, uint32_t color);
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t top_color, uint32_t bottom_color);
void gfx_draw_cursor(int32_t x, int32_t y);
void gfx_draw_char(int32_t x, int32_t y, char c, uint32_t color);
void gfx_clear_char(int32_t x, int32_t y, uint32_t bg_color);
void gfx_draw_string(int32_t x, int32_t y, const char *str, uint32_t color);
void gfx_draw_centered_text(const char* text, uint32_t color);
void gfx_scroll_up(int pixels, uint32_t fill_color);

// Double buffering
void gfx_enable_double_buffering();  // Call after heap_init
void gfx_swap_buffers();             // Copy backbuffer to screen
uint32_t* gfx_get_buffer();          // Get current drawing target
void gfx_mark_dirty(int32_t x, int32_t y, int32_t w, int32_t h);  // Mark region as needing redraw

uint64_t gfx_get_width();
uint64_t gfx_get_height();

// Colors - Modern Palette
#define COLOR_BLACK           0x000000
#define COLOR_WHITE           0xFFFFFFFF
#define COLOR_GRAY            0xFF6C7086
#define COLOR_DIM_GRAY        0x555555
#define COLOR_CYAN            0xFF89B4FA
#define COLOR_GREEN           0xFFA6E3A1
#define COLOR_YELLOW          0xFFF9E2AF
#define COLOR_RED             0xFFF38BA8
#define COLOR_PURPLE          0xFFCBA6F7

// UI Aliases
#define COLOR_BG              COLOR_BLACK
#define COLOR_TEXT            COLOR_WHITE
#define COLOR_MUTED           COLOR_GRAY
#define COLOR_ACCENT          COLOR_CYAN
#define COLOR_SUCCESS         COLOR_GREEN
#define COLOR_WARNING         COLOR_YELLOW
#define COLOR_ERROR           COLOR_RED

// Component Specific
#define COLOR_TIMESTAMP       COLOR_GRAY
#define COLOR_HELP_HEADER     COLOR_PURPLE
#define COLOR_PROMPT_USER     COLOR_WHITE
#define COLOR_PROMPT_HOST     COLOR_CYAN
#define COLOR_PROMPT_PATH     COLOR_WHITE

// Modern UI - Windows & Desktop
#define COLOR_DESKTOP_TOP     0x1a1a2e
#define COLOR_DESKTOP_BOTTOM  0x16213e
#define COLOR_TASKBAR         0x11111b
#define COLOR_TASKBAR_HOVER   0x2a2a4a
#define COLOR_INACTIVE_TITLE  0x313244
