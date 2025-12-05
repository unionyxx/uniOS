#include "graphics.h"

static struct limine_framebuffer* framebuffer = nullptr;

void gfx_init(struct limine_framebuffer* fb) {
    framebuffer = fb;
}

void gfx_put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (!framebuffer) return;
    if (x < 0 || y < 0) return;
    if (x >= (int32_t)framebuffer->width || y >= (int32_t)framebuffer->height) return;
    
    uint32_t* fb_ptr = (uint32_t*)framebuffer->address;
    fb_ptr[y * (framebuffer->pitch / 4) + x] = color;
}

void gfx_clear(uint32_t color) {
    if (!framebuffer) return;
    
    uint32_t* fb_ptr = (uint32_t*)framebuffer->address;
    for (uint64_t y = 0; y < framebuffer->height; y++) {
        for (uint64_t x = 0; x < framebuffer->width; x++) {
            fb_ptr[y * (framebuffer->pitch / 4) + x] = color;
        }
    }
}

void gfx_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    for (int32_t py = y; py < y + h; py++) {
        for (int32_t px = x; px < x + w; px++) {
            gfx_put_pixel(px, py, color);
        }
    }
}

void gfx_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    // Top and bottom
    for (int32_t px = x; px < x + w; px++) {
        gfx_put_pixel(px, y, color);
        gfx_put_pixel(px, y + h - 1, color);
    }
    // Left and right
    for (int32_t py = y; py < y + h; py++) {
        gfx_put_pixel(x, py, color);
        gfx_put_pixel(x + w - 1, py, color);
    }
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
