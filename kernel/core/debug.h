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

// ==============================================================================
// Debug Macros - conditionally compiled based on DEBUG flag
// ==============================================================================

#ifdef DEBUG
    // Debug build: all logging enabled
    // Tags are colored, messages are white
    #define DEBUG_INFO(fmt, ...)  do { kprintf_color(DEBUG_COLOR_INFO, "[INFO] "); kprintf(fmt "\n", ##__VA_ARGS__); } while(0)
    #define DEBUG_WARN(fmt, ...)  do { kprintf_color(DEBUG_COLOR_WARN, "[WARN] "); kprintf(fmt "\n", ##__VA_ARGS__); } while(0)
    #define DEBUG_ERROR(fmt, ...) do { kprintf_color(DEBUG_COLOR_ERROR, "[ERROR] "); kprintf(fmt "\n", ##__VA_ARGS__); } while(0)
    #define DEBUG_LOG(fmt, ...)   do { kprintf_color(DEBUG_COLOR_DEBUG, "[DEBUG] "); kprintf(fmt "\n", ##__VA_ARGS__); } while(0)
#else
    // Release build: no debug output (compiled away to nothing)
    #define DEBUG_INFO(fmt, ...)  ((void)0)
    #define DEBUG_WARN(fmt, ...)  ((void)0)
    #define DEBUG_ERROR(fmt, ...) ((void)0)
    #define DEBUG_LOG(fmt, ...)   ((void)0)
#endif

// Assert macro - always enabled (even in release)
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            extern void panic(const char* message); \
            panic("Assertion failed: " #condition); \
        } \
    } while(0)

// Hex dump memory (only in debug builds)
#ifdef DEBUG
    void debug_hexdump(const void* addr, uint64_t size);
#else
    #define debug_hexdump(addr, size) ((void)0)
#endif
