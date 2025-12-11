#pragma once
#include <stdint.h>
#include "limine.h"
#include "panic.h"

// Debug output colors
#define DEBUG_COLOR_INFO    0x00FF00  // Green
#define DEBUG_COLOR_WARN    0xFFFF00  // Yellow
#define DEBUG_COLOR_ERROR   0xFF0000  // Red
#define DEBUG_COLOR_DEBUG   0x00FFFF  // Cyan

// Initialize debug system
void debug_init(struct limine_framebuffer* fb);

// Print formatted string to screen (like printf)
void kprintf(const char* fmt, ...);

// Print with color
void kprintf_color(uint32_t color, const char* fmt, ...);

// Debug macros
#define DEBUG_INFO(fmt, ...)  kprintf_color(DEBUG_COLOR_INFO, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define DEBUG_WARN(fmt, ...)  kprintf_color(DEBUG_COLOR_WARN, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define DEBUG_ERROR(fmt, ...) kprintf_color(DEBUG_COLOR_ERROR, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define DEBUG_LOG(fmt, ...)   kprintf_color(DEBUG_COLOR_DEBUG, "[DEBUG] " fmt "\n", ##__VA_ARGS__)

// Assert macro
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            extern void panic(const char* message); \
            panic("Assertion failed: " #condition); \
        } \
    } while(0)


// Hex dump memory
void debug_hexdump(const void* addr, uint64_t size);
