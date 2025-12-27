#include "debug.h"
#include "graphics.h"
#include "serial.h"
#include "spinlock.h"
#include <stdarg.h>

// Spinlock for preventing interleaved debug output
static Spinlock debug_lock;

// Global log filter settings
int g_log_min_level = LOG_INFO;           // Default: show INFO and above
uint32_t g_log_module_mask = MOD_ALL;     // Default: all modules enabled

static struct limine_framebuffer* debug_fb = nullptr;
static uint64_t debug_x = 10;
static uint64_t debug_y = 10;
static const int LINE_HEIGHT = 16;
static const int MARGIN = 10;
static uint32_t current_color = COLOR_WHITE;

void debug_init(struct limine_framebuffer* fb) {
    debug_fb = fb;
    debug_x = MARGIN;
    debug_y = MARGIN;
    // debug_lock is static and zero-initialized
}

static void debug_putchar(char c) {
    // Always output to serial for debugging (even if screen not ready)
    if (c == '\n') {
        serial_putc('\r');  // CR before LF for proper terminal display
    }
    serial_putc(c);
    
    // Output to screen if framebuffer is available
    if (!debug_fb) return;
    
    if (c == '\n') {
        debug_x = MARGIN;
        debug_y += LINE_HEIGHT;
    } else {
        gfx_draw_char(debug_x, debug_y, c, current_color);
        debug_x += 9;
        if (debug_x >= gfx_get_width() - MARGIN) {
            debug_x = MARGIN;
            debug_y += LINE_HEIGHT;
        }
    }

    // Scroll if needed
    if (debug_y >= gfx_get_height() - LINE_HEIGHT) {
        gfx_scroll_up(LINE_HEIGHT, COLOR_BLACK);
        debug_y -= LINE_HEIGHT;
    }
}

static void debug_puts(const char* s) {
    while (*s) {
        debug_putchar(*s++);
    }
}

// Simple integer to string
static void debug_print_int(int64_t num, int base) {
    char buf[32];
    int i = 0;
    bool negative = false;
    
    if (num == 0) {
        debug_putchar('0');
        return;
    }
    
    if (num < 0 && base == 10) {
        negative = true;
        num = -num;
    }
    
    uint64_t unum = (uint64_t)num;
    
    while (unum > 0) {
        int digit = unum % base;
        buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        unum /= base;
    }
    
    if (negative) debug_putchar('-');
    while (i > 0) debug_putchar(buf[--i]);
}

static void debug_print_uint(uint64_t num, int base) {
    char buf[32];
    int i = 0;
    
    if (num == 0) {
        debug_putchar('0');
        return;
    }
    
    while (num > 0) {
        int digit = num % base;
        buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
        num /= base;
    }
    
    while (i > 0) debug_putchar(buf[--i]);
}

void kprintf(const char* fmt, ...) {
    spinlock_acquire(&debug_lock);
    
    va_list args;
    va_start(args, fmt);
    
    current_color = COLOR_WHITE;
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd':
                case 'i':
                    debug_print_int(va_arg(args, int), 10);
                    break;
                case 'u':
                    debug_print_uint(va_arg(args, unsigned int), 10);
                    break;
                case 'x':
                    debug_print_uint(va_arg(args, unsigned int), 16);
                    break;
                case 'p':
                    debug_puts("0x");
                    debug_print_uint(va_arg(args, uint64_t), 16);
                    break;
                case 'l':
                    fmt++;
                    if (*fmt == 'x') {
                        debug_print_uint(va_arg(args, uint64_t), 16);
                    } else if (*fmt == 'd') {
                        debug_print_int(va_arg(args, int64_t), 10);
                    } else if (*fmt == 'u') {
                        debug_print_uint(va_arg(args, uint64_t), 10);
                    }
                    break;
                case 's': {
                    const char* s = va_arg(args, const char*);
                    debug_puts(s ? s : "(null)");
                    break;
                }
                case 'c':
                    debug_putchar((char)va_arg(args, int));
                    break;
                case '%':
                    debug_putchar('%');
                    break;
                default:
                    debug_putchar('%');
                    debug_putchar(*fmt);
            }
        } else {
            debug_putchar(*fmt);
        }
        fmt++;
    }
    
    va_end(args);
    spinlock_release(&debug_lock);
}

void kprintf_color(uint32_t color, const char* fmt, ...) {
    spinlock_acquire(&debug_lock);
    
    va_list args;
    va_start(args, fmt);
    
    uint32_t old_color = current_color;
    current_color = color;
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd':
                case 'i':
                    debug_print_int(va_arg(args, int), 10);
                    break;
                case 'u':
                    debug_print_uint(va_arg(args, unsigned int), 10);
                    break;
                case 'x':
                    debug_print_uint(va_arg(args, unsigned int), 16);
                    break;
                case 'p':
                    debug_puts("0x");
                    debug_print_uint(va_arg(args, uint64_t), 16);
                    break;
                case 'l':
                    fmt++;
                    if (*fmt == 'x') {
                        debug_print_uint(va_arg(args, uint64_t), 16);
                    } else if (*fmt == 'd') {
                        debug_print_int(va_arg(args, int64_t), 10);
                    } else if (*fmt == 'u') {
                        debug_print_uint(va_arg(args, uint64_t), 10);
                    }
                    break;
                case 's': {
                    const char* s = va_arg(args, const char*);
                    debug_puts(s ? s : "(null)");
                    break;
                }
                case 'c':
                    debug_putchar((char)va_arg(args, int));
                    break;
                case '%':
                    debug_putchar('%');
                    break;
            }
        } else {
            debug_putchar(*fmt);
        }
        fmt++;
    }
    
    current_color = old_color;
    va_end(args);
    spinlock_release(&debug_lock);
}


#ifdef DEBUG
void debug_hexdump(const void* addr, uint64_t size) {
    const uint8_t* p = (const uint8_t*)addr;
    
    for (uint64_t i = 0; i < size; i += 16) {
        kprintf("%p: ", (uint64_t)(p + i));
        
        for (uint64_t j = 0; j < 16 && i + j < size; j++) {
            kprintf("%x ", p[i + j]);
        }
        
        kprintf("\n");
    }
}
#endif

// Stack frame structure for walking the call stack
struct StackFrame {
    struct StackFrame* rbp;
    uint64_t rip;
};

void debug_print_stack_trace() {
    StackFrame* stack;
    asm ("mov %%rbp, %0" : "=r"(stack));
    
    kprintf_color(0x00FFFF, "\n--- Stack Trace ---\n");
    
    int depth = 0;
    while (stack && depth < 20) {
        // Validate pointer to avoid GPF during panic (must be in kernel space)
        if ((uint64_t)stack < 0xFFFF800000000000) break;
        
        kprintf("[%d] RIP: 0x%lx\n", depth, stack->rip);
        
        stack = stack->rbp;
        depth++;
    }
    kprintf_color(0x00FFFF, "-------------------\n");
}

// ==============================================================================
// klog() - Filtered kernel logging with serial output
// ==============================================================================

// Module name lookup table
static const char* module_names[] = {
    "KERNEL", "SCHED", "MEM", "NET", "FS", "DRIVER", "USB", "GFX"
};

// Level name lookup table
static const char* level_names[] = {
    "TRACE", "INFO", "WARN", "ERROR", "FATAL"
};

// Level color lookup table
static const uint32_t level_colors[] = {
    DEBUG_COLOR_TRACE,  // TRACE - gray
    DEBUG_COLOR_INFO,   // INFO  - green
    DEBUG_COLOR_WARN,   // WARN  - yellow
    DEBUG_COLOR_ERROR,  // ERROR - red
    DEBUG_COLOR_ERROR   // FATAL - red
};

void klog(LogModule mod, LogLevel level, const char* func, const char* fmt, ...) {
    // Filter check: skip if level is below minimum
    if (level < g_log_min_level) return;
    
    // Filter check: skip if module is not in mask
    if (!(mod & g_log_module_mask)) return;
    
    // Acquire lock before building/outputting message
    spinlock_acquire(&debug_lock);
    
    // Build prefix: [MODULE][LEVEL] func: 
    char prefix[64];
    int pi = 0;
    
    // Find module index (log2 of mod)
    int mod_idx = 0;
    uint32_t m = mod;
    while (m > 1) { m >>= 1; mod_idx++; }
    if (mod_idx >= 8) mod_idx = 0;  // Safety clamp
    
    // Build prefix string
    prefix[pi++] = '[';
    const char* mn = module_names[mod_idx];
    while (*mn) prefix[pi++] = *mn++;
    prefix[pi++] = ']';
    prefix[pi++] = '[';
    const char* ln = level_names[level];
    while (*ln) prefix[pi++] = *ln++;
    prefix[pi++] = ']';
    prefix[pi++] = ' ';
    while (*func && pi < 50) prefix[pi++] = *func++;
    prefix[pi++] = ':';
    prefix[pi++] = ' ';
    prefix[pi] = '\0';
    
    // Format the message
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    
    int bi = 0;
    while (*fmt && bi < 500) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd':
                case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) { buffer[bi++] = '-'; val = -val; }
                    char numtmp[16]; int ni = 0;
                    if (val == 0) numtmp[ni++] = '0';
                    while (val > 0) { numtmp[ni++] = '0' + (val % 10); val /= 10; }
                    while (ni > 0) buffer[bi++] = numtmp[--ni];
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    char numtmp[16]; int ni = 0;
                    if (val == 0) numtmp[ni++] = '0';
                    while (val > 0) { numtmp[ni++] = '0' + (val % 10); val /= 10; }
                    while (ni > 0) buffer[bi++] = numtmp[--ni];
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    char numtmp[16]; int ni = 0;
                    if (val == 0) numtmp[ni++] = '0';
                    while (val > 0) {
                        int digit = val % 16;
                        numtmp[ni++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                        val /= 16;
                    }
                    while (ni > 0) buffer[bi++] = numtmp[--ni];
                    break;
                }
                case 'p': {
                    buffer[bi++] = '0'; buffer[bi++] = 'x';
                    uint64_t val = va_arg(args, uint64_t);
                    char numtmp[20]; int ni = 0;
                    if (val == 0) numtmp[ni++] = '0';
                    while (val > 0) {
                        int digit = val % 16;
                        numtmp[ni++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                        val /= 16;
                    }
                    while (ni > 0) buffer[bi++] = numtmp[--ni];
                    break;
                }
                case 'l':
                    fmt++;
                    if (*fmt == 'x' || *fmt == 'u') {
                        uint64_t val = va_arg(args, uint64_t);
                        char numtmp[24]; int ni = 0;
                        int base = (*fmt == 'x') ? 16 : 10;
                        if (val == 0) numtmp[ni++] = '0';
                        while (val > 0) {
                            int digit = val % base;
                            numtmp[ni++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                            val /= base;
                        }
                        while (ni > 0) buffer[bi++] = numtmp[--ni];
                    }
                    break;
                case 's': {
                    const char* s = va_arg(args, const char*);
                    if (!s) s = "(null)";
                    while (*s && bi < 500) buffer[bi++] = *s++;
                    break;
                }
                case 'c':
                    buffer[bi++] = (char)va_arg(args, int);
                    break;
                case '%':
                    buffer[bi++] = '%';
                    break;
                default:
                    buffer[bi++] = '%';
                    buffer[bi++] = *fmt;
            }
        } else {
            buffer[bi++] = *fmt;
        }
        fmt++;
    }
    buffer[bi++] = '\n';
    buffer[bi] = '\0';
    
    va_end(args);
    
    // Output to serial (same as existing DEBUG_* macros)
    for (int i = 0; prefix[i]; i++) {
        if (prefix[i] == '\n') serial_putc('\r');
        serial_putc(prefix[i]);
    }
    for (int i = 0; buffer[i]; i++) {
        if (buffer[i] == '\n') serial_putc('\r');
        serial_putc(buffer[i]);
    }
    
    spinlock_release(&debug_lock);
    
    // Only show ERROR and FATAL on framebuffer (keep screen clean)
    // kprintf_color has its own lock, so we call it after releasing ours
    if (level >= LOG_ERROR) {
        kprintf_color(level_colors[level], "%s%s", prefix, buffer);
    }
}
