#pragma once
#include <boot/boot_info.h>
#include <stdint.h>

enum class LogLevel
{
    Trace = 0,
    Info,
    Success,
    Warn,
    Error,
    Fatal
};

enum class LogModule : uint32_t
{
    Kernel = (1 << 0),
    Sched = (1 << 1),
    Mem = (1 << 2),
    Net = (1 << 3),
    Fs = (1 << 4),
    Driver = (1 << 5),
    Usb = (1 << 6),
    Gfx = (1 << 7),
    Boot = (1 << 8),
    Hw = (1 << 9),
    All = 0xFFFF
};

constexpr uint32_t LOG_COLOR_TIME = 0xFF5AC8FA;
constexpr uint32_t LOG_COLOR_WHITE = 0xFFFFFFFF;
constexpr uint32_t LOG_COLOR_OK = 0xFF30D158;
constexpr uint32_t LOG_COLOR_WARN = 0xFFFFD60A;
constexpr uint32_t LOG_COLOR_ERROR = 0xFFFF453A;
constexpr uint32_t LOG_COLOR_TRACE = 0x555555;

extern LogLevel g_log_min_level;
extern uint32_t g_log_module_mask;
extern bool g_boot_quiet;
extern bool g_boot_framebuffer_logging;
extern bool g_boot_user_stdout_to_framebuffer;

void debug_init(const BootFramebuffer *fb);
void kprintf(const char *fmt, ...);
void kprintf_color(uint32_t color, const char *fmt, ...);
void qemu_debugcon_puts(const char *str);
void klog(LogModule mod, LogLevel level, const char *func, const char *fmt, ...);
const char *log_module_name(LogModule mod);

#ifdef DEBUG
#define KLOG(mod, lvl, fmt, ...) klog(mod, lvl, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define KLOG(mod, lvl, fmt, ...)                                                                                       \
    do {                                                                                                               \
        if (lvl >= LogLevel::Error)                                                                                    \
            klog(mod, lvl, __func__, fmt __VA_OPT__(, ) __VA_ARGS__);                                                  \
    } while (0)
#endif

#define DEBUG_TRACE(fmt, ...) klog(LogModule::Boot, LogLevel::Trace, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)
#define DEBUG_INFO(fmt, ...) klog(LogModule::Boot, LogLevel::Info, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)
#define DEBUG_SUCCESS(fmt, ...) klog(LogModule::Boot, LogLevel::Success, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)
#define DEBUG_WARN(fmt, ...) klog(LogModule::Boot, LogLevel::Warn, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)
#define DEBUG_ERROR(fmt, ...) klog(LogModule::Boot, LogLevel::Error, __func__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define BOOT_LOG(fmt, ...) DEBUG_INFO(fmt __VA_OPT__(, ) __VA_ARGS__)
#define BOOT_SUCCESS(fmt, ...) DEBUG_SUCCESS(fmt __VA_OPT__(, ) __VA_ARGS__)
#define BOOT_WARN(fmt, ...) DEBUG_WARN(fmt __VA_OPT__(, ) __VA_ARGS__)
#define BOOT_ERROR(fmt, ...) DEBUG_ERROR(fmt __VA_OPT__(, ) __VA_ARGS__)

#define ASSERT(condition)                                                                                              \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            extern void panic_with_details(const char *message, const char *file, int line, const char *func);         \
            panic_with_details("Assertion failed: " #condition, __FILE__, __LINE__, __func__);                         \
        }                                                                                                              \
    } while (0)

#define STATIC_ASSERT(condition, message) static_assert(condition, message)

#ifdef DEBUG
void debug_hexdump(const void *addr, uint64_t size);
#else
#define debug_hexdump(addr, size) ((void)0)
#endif

void debug_print_stack_trace();
