#pragma once
#include <stdint.h>
#include "limine.h"

// Basic graphics primitives
void gfx_init(struct limine_framebuffer* fb);
void gfx_clear(uint32_t color);
void gfx_put_pixel(int32_t x, int32_t y, uint32_t color);
void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void gfx_draw_cursor(int32_t x, int32_t y);

// Colors
#define COLOR_BLACK       0x000000
#define COLOR_WHITE       0xFFFFFF
#define COLOR_GRAY        0x808080
#define COLOR_DARK_GRAY   0x404040
#define COLOR_LIGHT_GRAY  0xC0C0C0
#define COLOR_BLUE        0x0000AA
#define COLOR_DARK_BLUE   0x000066
#define COLOR_CYAN        0x00AAAA
#define COLOR_GREEN       0x00AA00
#define COLOR_RED         0xAA0000
#define COLOR_DESKTOP     0x008080
