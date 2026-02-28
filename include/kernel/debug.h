#pragma once
#include <stdint.h>
#include <boot/limine.h>

enum class LogLevel {
    Trace = 0,
    Info,
    Success,
    Warn,
    Error,
    Fatal
};

enum class LogModule : uint32_t {
    Kernel = (1 << 0),
    Sched  = (1 << 1),
    Mem    = (1 << 2),
    Net    = (1 << 3),
    Fs     = (1 << 4),
    Driver = (1 << 5),
    Usb    = (1 << 6),
    Gfx    = (1 << 7),
    Boot   = (1 << 8),
    Hw     = (1 << 9),
    All    = 0xFFFF
};

constexpr uint32_t LOG_COLOR_TIME  = 0xFF5AC8FA;
constexpr uint32_t LOG_COLOR_WHITE = 0xFFFFFFFF;
constexpr uint32_t LOG_COLOR_OK    = 0xFF30D158;
constexpr uint32_t LOG_COLOR_WARN  = 0xFFFFD60A;
constexpr uint32_t LOG_COLOR_ERROR = 0xFFFF453A;
constexpr uint32_t LOG_COLOR_TRACE = 0x555555;

extern LogLevel g_log_min_level;
extern uint32_t g_log_module_mask;
extern bool     g_boot_quiet;

void debug_init(struct limine_framebuffer* fb);
void kprintf(const char* fmt, ...);
void kprintf_color(uint32_t color, const char* fmt, ...);
void qemu_debugcon_puts(const char* str);
void klog(LogModule mod, LogLevel level, const char* func, const char* fmt, ...);

#ifdef DEBUG
    #define KLOG(mod, lvl, fmt, ...) klog(mod, lvl, __func__, fmt, ##__VA_ARGS__)
#else
    #define KLOG(mod, lvl, fmt, ...) do { if (lvl >= LogLevel::Error) klog(mod, lvl, __func__, fmt, ##__VA_ARGS__); } while(0)
#endif

#ifdef DEBUG
    #define DEBUG_INFO(fmt, ...)    KLOG(LogModule::Boot, LogLevel::Info,    fmt, ##__VA_ARGS__)
    #define DEBUG_SUCCESS(fmt, ...) KLOG(LogModule::Boot, LogLevel::Success, fmt, ##__VA_ARGS__)
    #define DEBUG_WARN(fmt, ...)    KLOG(LogModule::Boot, LogLevel::Warn,    fmt, ##__VA_ARGS__)
    #define DEBUG_ERROR(fmt, ...)   KLOG(LogModule::Boot, LogLevel::Error,   fmt, ##__VA_ARGS__)
#else
    #define DEBUG_INFO(fmt, ...)    ((void)0)
    #define DEBUG_SUCCESS(fmt, ...) ((void)0)
    #define DEBUG_WARN(fmt, ...)    ((void)0)
    #define DEBUG_ERROR(fmt, ...)   ((void)0)
#endif

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

void debug_print_stack_trace();
