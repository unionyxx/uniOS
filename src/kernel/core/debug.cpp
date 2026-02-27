#include <kernel/debug.h>
#include <drivers/video/framebuffer.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <stdarg.h>
#include <stddef.h>

static Spinlock debug_lock;

int g_log_min_level = LOG_INFO;
uint32_t g_log_module_mask = MOD_ALL;
bool g_boot_quiet = true;

// Circular buffer for dmesg
constexpr size_t KLOG_BUFFER_SIZE = 16384;
static char klog_buffer[KLOG_BUFFER_SIZE];
static size_t klog_head = 0;
static size_t klog_total_written = 0;

static struct limine_framebuffer* debug_fb = nullptr;
static uint64_t debug_x = 10;
static uint64_t debug_y = 10;
constexpr int LINE_HEIGHT = 16;
constexpr int MARGIN = 10;
static uint32_t current_color = COLOR_WHITE;

void debug_init(struct limine_framebuffer* fb) {
    debug_fb = fb;
    debug_x = MARGIN;
    debug_y = MARGIN;
}

static void klog_push_char(char c) {
    klog_buffer[klog_head] = c;
    klog_head = (klog_head + 1) % KLOG_BUFFER_SIZE;
    klog_total_written++;
}

static void klog_push_str(const char* s) {
    while (*s) klog_push_char(*s++);
}

static void debug_putchar(char c) {
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
    
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

    if (debug_y >= gfx_get_height() - LINE_HEIGHT) {
        gfx_scroll_up(LINE_HEIGHT, COLOR_BLACK);
        debug_y -= LINE_HEIGHT;
    }
}

static void debug_puts(const char* s) {
    while (*s) debug_putchar(*s++);
}

static void debug_print_uint(uint64_t num, int base) {
    char buf[32]; int i = 0;
    if (num == 0) { debug_putchar('0'); return; }
    while (num > 0) {
        int d = num % base;
        buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
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
                case 'd': case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) { debug_putchar('-'); val = -val; }
                    debug_print_uint(val, 10);
                    break;
                }
                case 'u': debug_print_uint(va_arg(args, unsigned int), 10); break;
                case 'x': debug_print_uint(va_arg(args, unsigned int), 16); break;
                case 'p': debug_puts("0x"); debug_print_uint(va_arg(args, uint64_t), 16); break;
                case 'l':
                    fmt++;
                    if (*fmt == 'x') debug_print_uint(va_arg(args, uint64_t), 16);
                    else if (*fmt == 'u') debug_print_uint(va_arg(args, uint64_t), 10);
                    break;
                case 's': { const char* s = va_arg(args, const char*); debug_puts(s ? s : "(null)"); break; }
                case 'c': debug_putchar((char)va_arg(args, int)); break;
                case '%': debug_putchar('%'); break;
                default: debug_putchar('%'); debug_putchar(*fmt);
            }
        } else debug_putchar(*fmt);
        fmt++;
    }
    va_end(args);
    spinlock_release(&debug_lock);
}

void kprintf_color(uint32_t color, const char* fmt, ...) {
    spinlock_acquire(&debug_lock);
    va_list args;
    va_start(args, fmt);
    uint32_t old = current_color;
    current_color = color;
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd': case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) { debug_putchar('-'); val = -val; }
                    debug_print_uint(val, 10);
                    break;
                }
                case 'u': debug_print_uint(va_arg(args, unsigned int), 10); break;
                case 'x': debug_print_uint(va_arg(args, unsigned int), 16); break;
                case 'p': debug_puts("0x"); debug_print_uint(va_arg(args, uint64_t), 16); break;
                case 'l':
                    fmt++;
                    if (*fmt == 'x') debug_print_uint(va_arg(args, uint64_t), 16);
                    else if (*fmt == 'u') debug_print_uint(va_arg(args, uint64_t), 10);
                    break;
                case 's': { const char* s = va_arg(args, const char*); debug_puts(s ? s : "(null)"); break; }
                case 'c': debug_putchar((char)va_arg(args, int)); break;
                case '%': debug_putchar('%'); break;
            }
        } else debug_putchar(*fmt);
        fmt++;
    }
    current_color = old;
    va_end(args);
    spinlock_release(&debug_lock);
}

static bool is_success_message(const char* s) {
    if (!s) return false;
    const char* keywords[] = {"ready", "complete", "initialized", "success", "UP", "loaded", "enabled", "mounted", "unmasked", "init:"};
    for (int i = 0; i < 10; i++) {
        const char* k = keywords[i];
        const char* p = s;
        while (*p) {
            const char* p2 = p; const char* k2 = k;
            while (*p2 && *k2) {
                char c1 = *p2; char c2 = *k2;
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) break;
                p2++; k2++;
            }
            if (!*k2) return true;
            p++;
        }
    }
    return false;
}

void klog(LogModule mod, LogLevel level, const char* func, const char* fmt, ...) {
    (void)func;
    if (level < g_log_min_level) return;
    if (!(mod & g_log_module_mask)) return;
    
    spinlock_acquire(&debug_lock);
    
    uint64_t ticks = timer_get_ticks();
    uint64_t s = ticks / 1000; uint64_t m = ticks % 1000;
    
    char time_str[16];
    int ti = 0;
    if (s < 100) time_str[ti++] = ' ';
    if (s < 10)  time_str[ti++] = ' ';
    
    char s_buf[16]; int si = 0; uint64_t st = s;
    if (st == 0) s_buf[si++] = '0';
    while (st > 0) { s_buf[si++] = '0' + (st % 10); st /= 10; }
    while (si > 0) time_str[ti++] = s_buf[--si];
    
    time_str[ti++] = '.';
    time_str[ti++] = '0' + ((m / 100) % 10);
    time_str[ti++] = '0' + ((m / 10) % 10);
    time_str[ti++] = '0' + (m % 10);
    time_str[ti] = '\0';

    const char* tag = " INFO ";
    uint32_t tag_color = COLOR_CYAN;
    
    if (level == LOG_ERROR || level == LOG_FATAL) {
        tag = " FAIL ";
        tag_color = COLOR_RED;
    } else if (level == LOG_WARN) {
        tag = " WARN ";
        tag_color = COLOR_YELLOW;
    } else if (level == LOG_SUCCESS || is_success_message(fmt)) {
        tag = "  OK  ";
        tag_color = COLOR_GREEN;
    } else if (level == LOG_TRACE) {
        tag = "TRACE ";
        tag_color = COLOR_DIM_GRAY;
    }
    
    char buffer[512]; va_list args; va_start(args, fmt);
    int bi = 0;
    while (*fmt && bi < 500) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd': case 'i': {
                    int val = va_arg(args, int);
                    if (val < 0) { buffer[bi++] = '-'; val = -val; }
                    char nt[16]; int ni = 0; if (val == 0) nt[ni++] = '0';
                    while (val > 0) { nt[ni++] = '0' + (val % 10); val /= 10; }
                    while (ni > 0) buffer[bi++] = nt[--ni];
                    break;
                }
                case 'u': {
                    unsigned int val = va_arg(args, unsigned int);
                    char nt[16]; int ni = 0; if (val == 0) nt[ni++] = '0';
                    while (val > 0) { nt[ni++] = '0' + (val % 10); val /= 10; }
                    while (ni > 0) buffer[bi++] = nt[--ni];
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    char nt[16]; int ni = 0; if (val == 0) nt[ni++] = '0';
                    while (val > 0) { int d = val % 16; nt[ni++] = (d < 10) ? ('0' + d) : ('a' + d - 10); val /= 16; }
                    while (ni > 0) buffer[bi++] = nt[--ni];
                    break;
                }
                case 'p': {
                    buffer[bi++] = '0'; buffer[bi++] = 'x';
                    uint64_t val = va_arg(args, uint64_t);
                    char nt[20]; int ni = 0; if (val == 0) nt[ni++] = '0';
                    while (val > 0) { int d = val % 16; nt[ni++] = (d < 10) ? ('0' + d) : ('a' + d - 10); val /= 16; }
                    while (ni > 0) buffer[bi++] = nt[--ni];
                    break;
                }
                case 'l':
                    fmt++;
                    if (*fmt == 'x' || *fmt == 'u') {
                        uint64_t val = va_arg(args, uint64_t);
                        char nt[24]; int ni = 0; int base = (*fmt == 'x') ? 16 : 10;
                        if (val == 0) nt[ni++] = '0';
                        while (val > 0) { int d = val % base; nt[ni++] = (d < 10) ? ('0' + d) : ('a' + d - 10); val /= base; }
                        while (ni > 0) buffer[bi++] = nt[--ni];
                    }
                    break;
                case 's': { const char* s = va_arg(args, const char*); if (!s) s = "(null)"; while (*s && bi < 500) buffer[bi++] = *s++; break; }
                case 'c': buffer[bi++] = (char)va_arg(args, int); break;
                case '%': buffer[bi++] = '%'; break;
                default: buffer[bi++] = '%'; buffer[bi++] = *fmt;
            }
        } else buffer[bi++] = *fmt;
        fmt++;
    }
    if (bi > 0 && buffer[bi-1] != '\n') buffer[bi++] = '\n';
    buffer[bi] = '\0'; va_end(args);
    
    // Write to circular buffer
    klog_push_str(time_str);
    klog_push_str(" | ");
    klog_push_str(tag);
    klog_push_str(" | ");
    klog_push_str(buffer);

    for (int i = 0; time_str[i]; i++) serial_putc(time_str[i]);
    serial_putc(' '); serial_putc('|'); serial_putc(' ');
    for (int i = 0; tag[i]; i++) serial_putc(tag[i]);
    serial_putc(' '); serial_putc('|'); serial_putc(' ');
    for (int i = 0; buffer[i]; i++) { if (buffer[i] == '\n') serial_putc('\r'); serial_putc(buffer[i]); }
    
    spinlock_release(&debug_lock);
    
    if (g_boot_quiet && level < LOG_WARN) return;

    kprintf_color(COLOR_TIMESTAMP, "%s ", time_str);
    kprintf_color(COLOR_GRAY, "| ");
    kprintf_color(tag_color, "%s", tag);
    kprintf_color(COLOR_GRAY, " | ");
    kprintf_color(COLOR_WHITE, "%s", buffer);
}

#include <kernel/terminal.h>

extern "C" void klog_dump_buffer() {
    spinlock_acquire(&debug_lock);
    
    size_t start = 0;
    size_t length = klog_total_written;
    if (length > KLOG_BUFFER_SIZE) {
        length = KLOG_BUFFER_SIZE;
        start = klog_head;
    }

    for (size_t i = 0; i < length; i++) {
        char c = klog_buffer[(start + i) % KLOG_BUFFER_SIZE];
        g_terminal.put_char(c);
    }
    
    spinlock_release(&debug_lock);
}

void debug_print_stack_trace() {
    struct StackFrame { struct StackFrame* rbp; uint64_t rip; };
    StackFrame* stack;
    asm ("mov %%rbp, %0" : "=r"(stack));
    kprintf_color(COLOR_WHITE, "\nStack Trace:\n");
    int depth = 0;
    while (stack && depth < 20) {
        if ((uint64_t)stack < 0xFFFF800000000000) break;
        kprintf_color(COLOR_WHITE, "[");
        kprintf_color(COLOR_CYAN, "%d", depth);
        kprintf_color(COLOR_WHITE, "] RIP: ");
        kprintf_color(COLOR_CYAN, "0x%lx\n", stack->rip);
        stack = stack->rbp; depth++;
    }
}
