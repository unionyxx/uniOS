#pragma once
#include <stdint.h>
#include "limine.h"
#include "panic.h"

// ==============================================================================
// Log Levels & Module Filtering
// ==============================================================================

// Log severity levels
enum LogLevel {
    LOG_TRACE = 0,  // Very verbose (every packet/context switch)
    LOG_INFO,       // Normal events (startup, device found)
    LOG_WARN,       // Something weird but recovered
    LOG_ERROR,      // Operation failed
    LOG_FATAL       // System instability imminent
};

// Subsystem modules for filtering
enum LogModule {
    MOD_KERNEL = (1 << 0),
    MOD_SCHED  = (1 << 1),
    MOD_MEM    = (1 << 2),
    MOD_NET    = (1 << 3),
    MOD_FS     = (1 << 4),
    MOD_DRIVER = (1 << 5),
    MOD_USB    = (1 << 6),
    MOD_GFX    = (1 << 7),
    MOD_ALL    = 0xFF
};

// Global log filters (set via shell or in kmain)
extern int g_log_min_level;      // Minimum level to show (default: LOG_INFO)
extern uint32_t g_log_module_mask;  // Bitmask of enabled modules (default: MOD_ALL)

// ==============================================================================
// Debug Output Functions
// ==============================================================================

// Debug output colors
#define DEBUG_COLOR_INFO    0x00FF00  // Green
#define DEBUG_COLOR_WARN    0xFFFF00  // Yellow
#define DEBUG_COLOR_ERROR   0xFF0000  // Red
#define DEBUG_COLOR_DEBUG   0x00FFFF  // Cyan
#define DEBUG_COLOR_TRACE   0x888888  // Gray

// Initialize debug system
void debug_init(struct limine_framebuffer* fb);

// Print formatted string to screen + serial
void kprintf(const char* fmt, ...);

// Print with color
void kprintf_color(uint32_t color, const char* fmt, ...);

// QEMU debugcon output (port 0xE9) - fast, works even in crashes
void qemu_debugcon_puts(const char* str);

// Filtered logging function
void klog(LogModule mod, LogLevel level, const char* func, const char* fmt, ...);

// ==============================================================================
// KLOG Macro - Use this for all kernel logging
// ==============================================================================
#ifdef DEBUG
    #define KLOG(mod, lvl, fmt, ...) klog(mod, lvl, __func__, fmt, ##__VA_ARGS__)
#else
    // Release: only show errors and fatal
    #define KLOG(mod, lvl, fmt, ...) do { if (lvl >= LOG_ERROR) klog(mod, lvl, __func__, fmt, ##__VA_ARGS__); } while(0)
#endif

// ==============================================================================
// Legacy Debug Macros (kept for compatibility)
// ==============================================================================

#ifdef DEBUG
    #define DEBUG_INFO(fmt, ...)  do { kprintf_color(DEBUG_COLOR_INFO, "[INFO] "); kprintf("%s: " fmt "\n", __func__, ##__VA_ARGS__); } while(0)
    #define DEBUG_WARN(fmt, ...)  do { kprintf_color(DEBUG_COLOR_WARN, "[WARN] "); kprintf("%s: " fmt "\n", __func__, ##__VA_ARGS__); } while(0)
    #define DEBUG_ERROR(fmt, ...) do { kprintf_color(DEBUG_COLOR_ERROR, "[ERROR] "); kprintf("%s: " fmt "\n", __func__, ##__VA_ARGS__); } while(0)
    #define DEBUG_LOG(fmt, ...)   do { kprintf_color(DEBUG_COLOR_DEBUG, "[DEBUG] "); kprintf("%s: " fmt "\n", __func__, ##__VA_ARGS__); } while(0)
#else
    #define DEBUG_INFO(fmt, ...)  ((void)0)
    #define DEBUG_WARN(fmt, ...)  ((void)0)
    #define DEBUG_ERROR(fmt, ...) ((void)0)
    #define DEBUG_LOG(fmt, ...)   ((void)0)
#endif

// Assert macro - always enabled
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            extern void panic(const char* message); \
            panic("Assertion failed: " #condition); \
        } \
    } while(0)

#ifdef DEBUG
    void debug_hexdump(const void* addr, uint64_t size);
#else
    #define debug_hexdump(addr, size) ((void)0)
#endif

// Print stack trace for debugging panics and exceptions
void debug_print_stack_trace();

