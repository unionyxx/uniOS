#include <drivers/video/framebuffer.h>
#include <kernel/debug.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/theme.h>
#include <uapi/signal.h>

void hcf()
{
    // Force the backbuffer to the screen so we can actually see the panic!
    extern void gfx_swap_buffers(bool force);
    gfx_swap_buffers(true);

    asm("cli");
    for (;;)
        asm("hlt");
}

void panic_with_details(const char *message, const char *file, int line, const char *func);

void panic(const char *message)
{
    panic_with_details(message, nullptr, 0, nullptr);
}

void panic_with_details(const char *message, const char *file, int line, const char *func)
{
    uint32_t bg = g_theme ? g_theme->window_bg : 0x011116; // Default to mocha-like dark
    uint32_t text = g_theme ? g_theme->text_primary : 0xcdd6f4;
    uint32_t alert = g_theme ? g_theme->btn_close : 0xf38ba8;
    uint32_t dim = g_theme ? g_theme->text_dim : 0x7f849c;
    uint32_t accent = g_theme ? g_theme->accent : 0x89b4fa;

    if (gfx_get_width() > 0)
        gfx_clear(bg);

    kprintf("\n\n");
    kprintf_color(alert, "!!! KERNEL PANIC !!!\n");
    kprintf_color(dim, "--------------------------------------------------\n");
    kprintf_color(text, "MSG: %s\n", message);
    if (file) {
        kprintf_color(dim, "LOC: ");
        kprintf_color(accent, "%s:%d", file, line);
        if (func) {
            kprintf_color(dim, " in ");
            kprintf_color(accent, "%s()", func);
        }
        kprintf("\n");
    }
    kprintf_color(dim, "--------------------------------------------------\n");

    debug_print_stack_trace();
    kprintf_color(text, "\nSystem halted.");
    hcf();
}

struct ExceptionColors
{
    uint32_t text;
    uint32_t alert;
    uint32_t dim;
    uint32_t accent;
};

static ExceptionColors exception_colors()
{
    if (!g_theme)
        return {0xcdd6f4, 0xf38ba8, 0x7f849c, 0x89b4fa};
    return {g_theme->text_primary, g_theme->btn_close, g_theme->text_dim, g_theme->accent};
}

static bool exception_from_user_mode(const InterruptFrame *frame)
{
    return frame && ((frame->cs & 0x3) == 0x3);
}

static const char *exception_name(uint64_t int_no)
{
    switch (int_no) {
        case 0:
            return "Divide Error";
        case 3:
            return "Breakpoint";
        case 4:
            return "Overflow";
        case 5:
            return "Bound Range Exceeded";
        case 6:
            return "Invalid Opcode";
        case 7:
            return "Device Not Available";
        case 8:
            return "Double Fault";
        case 10:
            return "Invalid TSS";
        case 11:
            return "Segment Not Present";
        case 12:
            return "Stack Segment Fault";
        case 13:
            return "General Protection Fault";
        case 14:
            return "Page Fault";
        case 16:
            return "x87 Floating-Point Exception";
        case 17:
            return "Alignment Check";
        case 18:
            return "Machine Check";
        case 19:
            return "SIMD Floating-Point Exception";
        default:
            return "CPU Exception";
    }
}

static int signal_for_exception(uint64_t int_no)
{
    switch (int_no) {
        case 0:
        case 16:
        case 19:
            return SIGFPE;
        case 3:
            return SIGTRAP;
        case 6:
            return SIGILL;
        case 14:
            return SIGSEGV;
        default:
            return SIGSEGV;
    }
}

static void terminate_user_exception(InterruptFrame *frame, uint64_t cr2, bool has_cr2)
{
    Process *proc = process_get_current();
    const int sig = signal_for_exception(frame ? frame->int_no : 0);
    if (proc) {
        if (has_cr2) {
            KLOG(LogModule::Kernel, LogLevel::Error,
                 "Terminating user process pid=%lu name=%s after %s: rip=0x%lx cr2=0x%lx err=0x%lx",
                 proc->pid, proc->name, exception_name(frame->int_no), frame->rip, cr2, frame->err_code);
        } else {
            KLOG(LogModule::Kernel, LogLevel::Error,
                 "Terminating user process pid=%lu name=%s after %s: rip=0x%lx err=0x%lx", proc->pid,
                 proc->name, exception_name(frame->int_no), frame->rip, frame->err_code);
        }
    }

    process_exit(-sig);
    hcf();
}

extern "C" void exception_handler(InterruptFrame *frame)
{
    uint64_t int_no = frame->int_no;
    uint64_t err_code = frame->err_code;
    uint64_t rip = frame->rip;
    ExceptionColors colors = exception_colors();

    if (int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        if (vmm_handle_page_fault(cr2, err_code))
            return;

        if (exception_from_user_mode(frame))
            terminate_user_exception(frame, cr2, true);

        kprintf_color(colors.alert, "\nEXCEPTION CAUGHT! (Page Fault)\n");
        kprintf_color(colors.dim, "--------------------------------------------------\n");
        kprintf_color(colors.text, "CR2: ");
        kprintf_color(colors.accent, "0x%016lx  ", cr2);
        kprintf_color(colors.text, "ERR: ");
        kprintf_color(colors.accent, "0x%04lx  ", err_code);
        kprintf_color(colors.text, "RIP: ");
        kprintf_color(colors.accent, "0x%016lx\n", rip);
    } else {
        if (exception_from_user_mode(frame))
            terminate_user_exception(frame, 0, false);

        kprintf_color(colors.alert, "\nEXCEPTION CAUGHT!\n");
        kprintf_color(colors.dim, "--------------------------------------------------\n");
        kprintf_color(colors.text, "INT: ");
        kprintf_color(colors.accent, "0x%02lx  ", int_no);
        kprintf_color(colors.text, "ERR: ");
        kprintf_color(colors.accent, "0x%04lx  ", err_code);
        kprintf_color(colors.text, "RIP: ");
        kprintf_color(colors.accent, "0x%016lx\n", rip);
    }

    kprintf_color(colors.dim, "RAX: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->rax);
    kprintf_color(colors.dim, "RBX: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->rbx);
    kprintf_color(colors.dim, "RCX: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->rcx);
    kprintf_color(colors.dim, "RDX: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->rdx);
    kprintf_color(colors.dim, "RSI: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->rsi);
    kprintf_color(colors.dim, "RDI: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->rdi);
    kprintf_color(colors.dim, "RBP: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->rbp);
    kprintf_color(colors.dim, "RSP: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->rsp);
    kprintf_color(colors.dim, "R8:  ");
    kprintf_color(colors.text, "0x%016lx  ", frame->r8);
    kprintf_color(colors.dim, "R9:  ");
    kprintf_color(colors.text, "0x%016lx\n", frame->r9);
    kprintf_color(colors.dim, "R10: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->r10);
    kprintf_color(colors.dim, "R11: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->r11);
    kprintf_color(colors.dim, "R12: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->r12);
    kprintf_color(colors.dim, "R13: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->r13);
    kprintf_color(colors.dim, "R14: ");
    kprintf_color(colors.text, "0x%016lx  ", frame->r14);
    kprintf_color(colors.dim, "R15: ");
    kprintf_color(colors.text, "0x%016lx\n", frame->r15);
    kprintf_color(colors.dim, "CS:  ");
    kprintf_color(colors.text, "0x%04lx              ", frame->cs);
    kprintf_color(colors.dim, "FLG: ");
    kprintf_color(colors.text, "0x%08lx\n", frame->rflags);
    kprintf_color(colors.dim, "--------------------------------------------------\n");

    debug_print_stack_trace();
    hcf();
}
