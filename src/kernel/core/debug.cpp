#include <drivers/video/framebuffer.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>
#include <kernel/terminal.h>
#include <kernel/theme.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>
#include <stdarg.h>
#include <stddef.h>

static Spinlock g_debug_lock;

LogLevel g_log_min_level = LogLevel::Info;
uint32_t g_log_module_mask = static_cast<uint32_t>(LogModule::All);
bool g_boot_quiet = true;
bool g_boot_framebuffer_logging = false;
bool g_boot_user_stdout_to_framebuffer = false;

constexpr size_t KLOG_BUFFER_SIZE = 16384;
static char g_klog_buffer[KLOG_BUFFER_SIZE];
static size_t g_klog_head = 0;
static size_t g_klog_total_written = 0;

static const BootFramebuffer *g_debug_fb = nullptr;
static uint64_t g_debug_x = 10;
static uint64_t g_debug_y = 10;
static constexpr int LINE_HEIGHT = 16;
static constexpr int MARGIN = 10;
static uint32_t g_current_color = 0xFFFFFFFF;

void debug_init(const BootFramebuffer *fb)
{
    g_debug_fb = fb;
    g_debug_x = MARGIN;
    g_debug_y = MARGIN;
    if (g_theme)
        g_current_color = g_theme->text_primary;

#ifdef DEBUG
    g_boot_quiet = false;
    g_boot_framebuffer_logging = true;
    g_boot_user_stdout_to_framebuffer = true;
#else
    g_boot_quiet = true;
    g_boot_framebuffer_logging = false;
    g_boot_user_stdout_to_framebuffer = false;
#endif
}

static void klog_push_char(char c)
{
    g_klog_buffer[g_klog_head] = c;
    g_klog_head = (g_klog_head + 1) % KLOG_BUFFER_SIZE;
    g_klog_total_written++;
}

static void klog_push_str(const char *s)
{
    while (*s)
        klog_push_char(*s++);
}

static void debug_putchar(char c)
{
    if (c == '\n')
        serial_putc('\r');
    serial_putc(c);

    if (!g_debug_fb)
        return;

    if (c == '\n') {
        g_debug_x = MARGIN;
        g_debug_y += LINE_HEIGHT;
    } else {
        gfx_draw_char(g_debug_x, g_debug_y, c, g_current_color);
        g_debug_x += 9;
        if (g_debug_x >= gfx_get_width() - MARGIN) {
            g_debug_x = MARGIN;
            g_debug_y += LINE_HEIGHT;
        }
    }

    if (g_debug_y >= gfx_get_height() - LINE_HEIGHT) {
        gfx_scroll_up(LINE_HEIGHT, g_theme->term_bg);
        g_debug_y -= LINE_HEIGHT;
    }
}

static void debug_puts(const char *s)
{
    while (*s)
        debug_putchar(*s++);
}

static void fb_putchar(char c)
{
    if (!g_debug_fb)
        return;
    if (c == '\n') {
        g_debug_x = MARGIN;
        g_debug_y += LINE_HEIGHT;
    } else {
        gfx_draw_char(g_debug_x, g_debug_y, c, g_current_color);

        extern bool g_gfx_double_buffered;
        extern uint32_t *g_front_buffer;
        if (g_gfx_double_buffered && g_front_buffer) {
            gfx_draw_char_to_buffer(g_front_buffer, g_debug_x, g_debug_y, c, g_current_color);
        }

        g_debug_x += 9;
        if (g_debug_x >= gfx_get_width() - MARGIN) {
            g_debug_x = MARGIN;
            g_debug_y += LINE_HEIGHT;
        }
    }
    if (g_debug_y >= gfx_get_height() - LINE_HEIGHT) {
        gfx_scroll_up(LINE_HEIGHT, g_theme->term_bg);

        extern bool g_gfx_double_buffered;
        extern uint32_t *g_front_buffer;
        if (g_gfx_double_buffered && g_front_buffer) {
            gfx_scroll_up_buffer(g_front_buffer, LINE_HEIGHT, g_theme->term_bg);
        }
        g_debug_y -= LINE_HEIGHT;
    }
}

static void fb_puts(const char *s)
{
    while (*s)
        fb_putchar(*s++);
}

typedef void (*LogSinkFn)(char c, void *ctx);

static void emit_module_name(LogSinkFn sink, void *ctx, const char *module_name)
{
    size_t len = kstring::strlen(module_name);
    while (*module_name)
        sink(*module_name++, ctx);
    for (; len < 4; ++len)
        sink(' ', ctx);
}

struct BufferSink
{
    char *data;
    size_t capacity;
    size_t length;
};

static void sink_char(char c, void *ctx)
{
    auto *sink = static_cast<BufferSink *>(ctx);
    if (sink->length + 1 >= sink->capacity)
        return;
    sink->data[sink->length++] = c;
}

static void format_uint_to_sink(uint64_t num, int base, int width, LogSinkFn sink, void *ctx)
{
    char buf[64];
    int i = 0;
    if (num == 0)
        buf[i++] = '0';
    else {
        while (num > 0) {
            int d = num % base;
            buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
            num /= base;
        }
    }
    while (i < width)
        buf[i++] = '0';
    while (i > 0)
        sink(buf[--i], ctx);
}

static size_t kvformat_to_buffer(char *out, size_t capacity, const char *fmt, va_list args)
{
    if (!out || capacity == 0)
        return 0;

    BufferSink sink = {out, capacity, 0};
    while (*fmt) {
        if (*fmt != '%') {
            sink_char(*fmt++, &sink);
            continue;
        }

        fmt++;
        int width = 0;
        if (*fmt == '0') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t val = va_arg(args, int);
                if (val < 0) {
                    sink_char('-', &sink);
                    val = -val;
                }
                format_uint_to_sink(static_cast<uint64_t>(val), 10, width, sink_char, &sink);
                break;
            }
            case 'u':
                format_uint_to_sink(va_arg(args, unsigned int), 10, width, sink_char, &sink);
                break;
            case 'x':
                format_uint_to_sink(va_arg(args, unsigned int), 16, width, sink_char, &sink);
                break;
            case 'p':
                sink_char('0', &sink);
                sink_char('x', &sink);
                format_uint_to_sink(va_arg(args, uint64_t), 16, 16, sink_char, &sink);
                break;
            case 'l':
                fmt++;
                if (*fmt == 'x')
                    format_uint_to_sink(va_arg(args, uint64_t), 16, width, sink_char, &sink);
                else if (*fmt == 'u')
                    format_uint_to_sink(va_arg(args, uint64_t), 10, width, sink_char, &sink);
                else if (*fmt == 'l') {
                    fmt++;
                    if (*fmt == 'x')
                        format_uint_to_sink(va_arg(args, uint64_t), 16, width, sink_char, &sink);
                    else if (*fmt == 'u')
                        format_uint_to_sink(va_arg(args, uint64_t), 10, width, sink_char, &sink);
                } else {
                    sink_char('%', &sink);
                    sink_char('l', &sink);
                    if (*fmt)
                        sink_char(*fmt, &sink);
                }
                break;
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s)
                    s = "(null)";
                while (*s)
                    sink_char(*s++, &sink);
                break;
            }
            case 'c':
                sink_char(static_cast<char>(va_arg(args, int)), &sink);
                break;
            case '%':
                sink_char('%', &sink);
                break;
            default:
                sink_char('%', &sink);
                if (*fmt)
                    sink_char(*fmt, &sink);
                break;
        }

        if (*fmt)
            fmt++;
    }

    out[sink.length] = '\0';
    return sink.length;
}

static void emit_formatted(LogSinkFn sink, void *ctx, const char *fmt, va_list args)
{
    char buffer[512];
    kvformat_to_buffer(buffer, sizeof(buffer), fmt, args);
    for (size_t i = 0; buffer[i] != '\0'; ++i)
        sink(buffer[i], ctx);
}

static void debug_sink_char(char c, void *)
{
    debug_putchar(c);
}

const char *log_module_name(LogModule mod)
{
    switch (mod) {
        case LogModule::Kernel:
            return "kernel";
        case LogModule::Sched:
            return "sched";
        case LogModule::Mem:
            return "mem";
        case LogModule::Net:
            return "net";
        case LogModule::Fs:
            return "fs";
        case LogModule::Driver:
            return "driver";
        case LogModule::Usb:
            return "usb";
        case LogModule::Gfx:
            return "gfx";
        case LogModule::Boot:
            return "boot";
        case LogModule::Hw:
            return "hw";
        case LogModule::All:
        default:
            return "all";
    }
}

void kprintf(const char *fmt, ...)
{
    spinlock_acquire(&g_debug_lock);
    va_list args;
    va_start(args, fmt);
    g_current_color = g_theme->text_primary;
    emit_formatted(debug_sink_char, nullptr, fmt, args);
    va_end(args);
    spinlock_release(&g_debug_lock);
}

void kprintf_color(uint32_t color, const char *fmt, ...)
{
    spinlock_acquire(&g_debug_lock);
    va_list args;
    va_start(args, fmt);
    uint32_t old = g_current_color;
    g_current_color = color;
    emit_formatted(debug_sink_char, nullptr, fmt, args);
    g_current_color = old;
    va_end(args);
    spinlock_release(&g_debug_lock);
}

void klog(LogModule mod, LogLevel level, const char *, const char *fmt, ...)
{
    if (g_boot_quiet && level < LogLevel::Warn)
        return;
    if (level < g_log_min_level || !(static_cast<uint32_t>(mod) & g_log_module_mask))
        return;

    spinlock_acquire(&g_debug_lock);

    uint64_t ticks = timer_get_ticks();
    uint64_t s = ticks / 1000;
    uint64_t m = ticks % 1000;

    char time_str[16];
    int ti = 0;
    char s_buf[16];
    int si = 0;
    uint64_t st = s;
    if (st == 0)
        s_buf[si++] = '0';
    while (st > 0) {
        s_buf[si++] = '0' + (st % 10);
        st /= 10;
    }
    while (si > 0)
        time_str[ti++] = s_buf[--si];

    time_str[ti++] = '.';
    time_str[ti++] = '0' + (static_cast<char>(m / 100) % 10);
    time_str[ti++] = '0' + (static_cast<char>(m / 10) % 10);
    time_str[ti++] = '0' + (static_cast<char>(m % 10));
    time_str[ti] = '\0';

    const char *level_mark = "[i]";
    uint32_t tag_color = g_theme->accent;

    switch (level) {
        case LogLevel::Error:
        case LogLevel::Fatal:
            level_mark = "[-]";
            tag_color = g_theme->btn_close;
            break;
        case LogLevel::Warn:
            level_mark = "[!]";
            tag_color = g_theme->btn_minimize;
            break;
        case LogLevel::Success:
            level_mark = "[+]";
            tag_color = g_theme->btn_maximize;
            break;
        case LogLevel::Trace:
            level_mark = "[.]";
            tag_color = g_theme->text_dim;
            break;
        default:
            break;
    }

    const char *module_name = log_module_name(mod);
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    size_t bi = kvformat_to_buffer(buffer, sizeof(buffer), fmt, args);
    if (bi > 0 && buffer[bi - 1] != '\n' && bi + 1 < sizeof(buffer))
        buffer[bi++] = '\n';
    buffer[bi] = '\0';
    va_end(args);

    klog_push_str("[");
    klog_push_str(time_str);
    klog_push_str("] ");
    char module_buf[8] = {};
    BufferSink module_sink = {module_buf, sizeof(module_buf), 0};
    emit_module_name(sink_char, &module_sink, module_name);
    module_buf[module_sink.length] = '\0';
    klog_push_str(module_buf);
    klog_push_str(" ");
    klog_push_str(level_mark);
    klog_push_str(" ");
    klog_push_str(buffer);

    serial_putc('[');
    serial_puts(time_str);
    serial_puts("] ");
    emit_module_name([](char c, void *) { serial_putc(c); }, nullptr, module_name);
    serial_putc(' ');
    serial_puts(level_mark);
    serial_putc(' ');
    serial_puts(buffer);

    if (g_boot_framebuffer_logging && level >= LogLevel::Info) {
        uint32_t old_color = g_current_color;

        g_current_color = g_theme->text_dim;
        fb_putchar('[');
        fb_puts(time_str);
        fb_puts("] ");

        g_current_color = g_theme->text_dim;
        emit_module_name([](char c, void *) { fb_putchar(c); }, nullptr, module_name);
        fb_putchar(' ');

        g_current_color = tag_color;
        fb_puts(level_mark);

        g_current_color = g_theme->text_dim;
        fb_putchar(' ');

        g_current_color = g_theme->text_primary;
        fb_puts(buffer);

        g_current_color = old_color;
    }

    spinlock_release(&g_debug_lock);
}

extern "C" void klog_dump_buffer()
{
    spinlock_acquire(&g_debug_lock);
    size_t length = (g_klog_total_written > KLOG_BUFFER_SIZE) ? KLOG_BUFFER_SIZE : g_klog_total_written;
    size_t start = (g_klog_total_written > KLOG_BUFFER_SIZE) ? g_klog_head : 0;
    for (size_t i = 0; i < length; i++)
        g_terminal.put_char(g_klog_buffer[(start + i) % KLOG_BUFFER_SIZE]);
    spinlock_release(&g_debug_lock);
}

#include <kernel/mm/vmm.h>

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

extern "C" void debug_dump_pte(uint64_t *pml4, uint64_t vaddr)
{
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;

    kprintf("PTE for vaddr 0x%lx:\n", vaddr);

    uint64_t pml4e = pml4[pml4_idx];
    if (!(pml4e & 1)) {
        kprintf("  PML4E not present!\n");
        return;
    }

    uint64_t *pdpt = reinterpret_cast<uint64_t *>(vmm_phys_to_virt(pml4e & 0x000FFFFFFFFFF000ULL));
    uint64_t pdpte = pdpt[pdpt_idx];
    if (!(pdpte & 1)) {
        kprintf("  PDPTE not present!\n");
        return;
    }
    if (pdpte & (1 << 7)) {
        kprintf("  1GB page, PCD=%d PWT=%d\n", !!(pdpte & (1 << 4)), !!(pdpte & (1 << 3)));
        return;
    }

    uint64_t *pd = reinterpret_cast<uint64_t *>(vmm_phys_to_virt(pdpte & 0x000FFFFFFFFFF000ULL));
    uint64_t pde = pd[pd_idx];
    if (!(pde & 1)) {
        kprintf("  PDE not present!\n");
        return;
    }
    if (pde & (1 << 7)) {
        kprintf("  2MB page, PCD=%d PWT=%d PAT=%d\n", !!(pde & (1 << 4)), !!(pde & (1 << 3)), !!(pde & (1 << 12)));
        return;
    }

    uint64_t *pt = reinterpret_cast<uint64_t *>(vmm_phys_to_virt(pde & 0x000FFFFFFFFFF000ULL));
    uint64_t pte = pt[pt_idx];
    if (!(pte & 1)) {
        kprintf("  PTE not present!\n");
        return;
    }

    int pcd = !!(pte & (1 << 4));
    int pwt = !!(pte & (1 << 3));
    int pat = !!(pte & (1 << 7));

    kprintf("  PTE=0x%lx (P=%d W=%d U=%d)\n", pte, !!(pte & 1), !!(pte & 2), !!(pte & 4));
    kprintf("  PCD=%d PWT=%d PAT=%d\n", pcd, pwt, pat);

    int pat_index = (pat << 2) | (pcd << 1) | pwt;
    uint64_t pat_msr = rdmsr(0x277);
    uint8_t mem_type = (pat_msr >> (pat_index * 8)) & 0x07;

    const char *type_names[] = {"UC", "WC", "??", "??", "WT", "WP", "WB", "UC-"};
    kprintf("  PAT Index=%d, Memory type: %s (%d)\n", pat_index, type_names[mem_type], mem_type);
}

void debug_print_stack_trace()
{
    struct StackFrame
    {
        struct StackFrame *rbp;
        uint64_t rip;
    };
    StackFrame *stack;
    asm("mov %%rbp, %0" : "=r"(stack));
    kprintf_color(g_theme->text_primary, "\nStack Trace:\n");
    int depth = 0;
    while (stack && depth < 20) {
        if (reinterpret_cast<uint64_t>(stack) < 0xFFFF800000000000)
            break;
        kprintf_color(g_theme->text_primary, "[");
        kprintf_color(g_theme->accent, "%d", depth);
        kprintf_color(g_theme->text_primary, "] RIP: ");
        kprintf_color(g_theme->accent, "0x%lx\n", stack->rip);
        stack = stack->rbp;
        depth++;
    }
}
