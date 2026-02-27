#include <kernel/panic.h>
#include <kernel/debug.h>
#include <kernel/mm/vmm.h>
#include <drivers/video/framebuffer.h>

void hcf(void) {
    asm("cli");
    for (;;) asm("hlt");
}

void panic(const char* message) {
    if (gfx_get_width() > 0) {
        gfx_clear(0x220000); 
    }
    
    kprintf("\n\n");
    kprintf_color(COLOR_RED, "!!! KERNEL PANIC !!!\n");
    kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");
    kprintf_color(COLOR_WHITE, "%s\n", message);
    kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");
    
    debug_print_stack_trace();
    
    kprintf_color(COLOR_WHITE, "\nSystem halted.");
    
    hcf();
}

extern "C" void exception_handler(InterruptFrame* frame) {
    uint64_t int_no = frame->int_no;
    uint64_t err_code = frame->err_code;
    uint64_t rip = frame->rip;

    if (int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        
        if (vmm_handle_page_fault(cr2, err_code)) {
            return;
        }
        
        kprintf_color(COLOR_RED, "\nEXCEPTION CAUGHT! (Page Fault)\n");
        kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");
        kprintf_color(COLOR_WHITE, "CR2: "); kprintf_color(COLOR_CYAN, "0x%016llx  ", cr2);
        kprintf_color(COLOR_WHITE, "ERR: "); kprintf_color(COLOR_CYAN, "0x%04x  ", err_code);
        kprintf_color(COLOR_WHITE, "RIP: "); kprintf_color(COLOR_CYAN, "0x%016lx\n", rip);
    } else {
        kprintf_color(COLOR_RED, "\nEXCEPTION CAUGHT!\n");
        kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");
        kprintf_color(COLOR_WHITE, "INT: "); kprintf_color(COLOR_CYAN, "0x%02x  ", int_no);
        kprintf_color(COLOR_WHITE, "ERR: "); kprintf_color(COLOR_CYAN, "0x%04x  ", err_code);
        kprintf_color(COLOR_WHITE, "RIP: "); kprintf_color(COLOR_CYAN, "0x%016lx\n", rip);
    }
    
    // Register Dump
    kprintf_color(COLOR_GRAY, "RAX: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->rax);
    kprintf_color(COLOR_GRAY, "RBX: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->rbx);
    kprintf_color(COLOR_GRAY, "RCX: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->rcx);
    kprintf_color(COLOR_GRAY, "RDX: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->rdx);
    kprintf_color(COLOR_GRAY, "RSI: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->rsi);
    kprintf_color(COLOR_GRAY, "RDI: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->rdi);
    kprintf_color(COLOR_GRAY, "RBP: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->rbp);
    kprintf_color(COLOR_GRAY, "RSP: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->rsp);
    kprintf_color(COLOR_GRAY, "R8:  "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->r8);
    kprintf_color(COLOR_GRAY, "R9:  "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->r9);
    kprintf_color(COLOR_GRAY, "R10: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->r10);
    kprintf_color(COLOR_GRAY, "R11: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->r11);
    kprintf_color(COLOR_GRAY, "R12: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->r12);
    kprintf_color(COLOR_GRAY, "R13: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->r13);
    kprintf_color(COLOR_GRAY, "R14: "); kprintf_color(COLOR_WHITE, "0x%016lx  ", frame->r14);
    kprintf_color(COLOR_GRAY, "R15: "); kprintf_color(COLOR_WHITE, "0x%016lx\n", frame->r15);
    kprintf_color(COLOR_GRAY, "CS:  "); kprintf_color(COLOR_WHITE, "0x%04lx              ", frame->cs);
    kprintf_color(COLOR_GRAY, "FLG: "); kprintf_color(COLOR_WHITE, "0x%08lx\n", frame->rflags);
    kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");

    debug_print_stack_trace();
    
    hcf();
}
