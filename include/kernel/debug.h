#pragma once
#include <stdint.h>
#include <boot/limine.h>
#include <kernel/panic.h>

// Log severity levels
enum LogLevel {
    LOG_TRACE = 0,  // Very verbose (dim gray)
    LOG_INFO,       // Cyan: hardware found, network info
    LOG_SUCCESS,    // Green: "ready", "complete", "initialized"
    LOG_WARN,       // Yellow: non-fatal issues
    LOG_ERROR,      // Red: failures
    LOG_FATAL       // Red: system instability imminent
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
    MOD_BOOT   = (1 << 8),
    MOD_HW     = (1 << 9),
    MOD_ALL    = 0xFFFF
};

// Global log filters (set via shell or in kmain)
extern int g_log_min_level;      // Minimum level to show (default: LOG_INFO)
extern uint32_t g_log_module_mask;  // Bitmask of enabled modules (default: MOD_ALL)
extern bool g_boot_quiet;        // Quiet boot flag

// Debug output colors
#define LOG_COLOR_TIME    0xFF5AC8FA  // Cyan
#define LOG_COLOR_WHITE   0xFFFFFFFF  // White (normal)
#define LOG_COLOR_OK      0xFF30D158  // Green (success)
#define LOG_COLOR_WARN    0xFFFFD60A  // Yellow (warning)
#define LOG_COLOR_ERROR   0xFFFF453A  // Red (error)
#define LOG_COLOR_TRACE   0x555555    // Dim Gray

// For backward compatibility
#define LOG_COLOR_INFO    LOG_COLOR_TIME
#define LOG_COLOR_BOOT    LOG_COLOR_WHITE

#define DEBUG_COLOR_INFO    LOG_COLOR_WHITE
#define DEBUG_COLOR_WARN    LOG_COLOR_WARN
#define DEBUG_COLOR_ERROR   LOG_COLOR_ERROR
#define DEBUG_COLOR_DEBUG   LOG_COLOR_TIME
#define DEBUG_COLOR_TRACE   LOG_COLOR_TRACE

// Initialize debug system
void debug_init(struct limine_framebuffer* fb);

// Print formatted string to screen + serial
void kprintf(const char* fmt, ...);

// Print with color (used internally by klog)
void kprintf_color(uint32_t color, const char* fmt, ...);

// QEMU debugcon output (port 0xE9) - fast, works even in crashes
void qemu_debugcon_puts(const char* str);

// Filtered logging function
void klog(LogModule mod, LogLevel level, const char* func, const char* fmt, ...);

#ifdef DEBUG
    #define KLOG(mod, lvl, fmt, ...) klog(mod, lvl, __func__, fmt, ##__VA_ARGS__)
#else
    // Release: only show errors and fatal
    #define KLOG(mod, lvl, fmt, ...) do { if (lvl >= LOG_ERROR) klog(mod, lvl, __func__, fmt, ##__VA_ARGS__); } while(0)
#endif

#ifdef DEBUG
    #define DEBUG_INFO(fmt, ...)    KLOG(MOD_BOOT,   LOG_INFO,    fmt, ##__VA_ARGS__)
    #define DEBUG_SUCCESS(fmt, ...) KLOG(MOD_BOOT,   LOG_SUCCESS, fmt, ##__VA_ARGS__)
    #define DEBUG_WARN(fmt, ...)    KLOG(MOD_BOOT,   LOG_WARN,    fmt, ##__VA_ARGS__)
    #define DEBUG_ERROR(fmt, ...)   KLOG(MOD_BOOT,   LOG_ERROR,   fmt, ##__VA_ARGS__)
#else
    #define DEBUG_INFO(fmt, ...)    ((void)0)
    #define DEBUG_SUCCESS(fmt, ...) ((void)0)
    #define DEBUG_WARN(fmt, ...)    ((void)0)
    #define DEBUG_ERROR(fmt, ...)   ((void)0)
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

