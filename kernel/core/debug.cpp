#include "debug.h"
#include "graphics.h"
#include <stdarg.h>

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
}

static void debug_putchar(char c) {
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
}

void kprintf_color(uint32_t color, const char* fmt, ...) {
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
