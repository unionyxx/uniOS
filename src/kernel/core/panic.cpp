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

extern "C" void exception_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    uint64_t int_no = regs[15];
    uint64_t err_code = regs[16];
    uint64_t rip = regs[17];

    if (int_no == 14) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
        
        if (vmm_handle_page_fault(cr2, err_code)) {
            return;
        }
        
        kprintf_color(COLOR_RED, "\nEXCEPTION CAUGHT! (Page Fault)\n");
        kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");
        kprintf_color(COLOR_WHITE, "CR2: "); kprintf_color(COLOR_CYAN, "0x%llx  ", cr2);
        kprintf_color(COLOR_WHITE, "ERR: "); kprintf_color(COLOR_CYAN, "0x%x  ", err_code);
        kprintf_color(COLOR_WHITE, "RIP: "); kprintf_color(COLOR_CYAN, "0x%lx\n", rip);
    } else {
        kprintf_color(COLOR_RED, "\nEXCEPTION CAUGHT!\n");
        kprintf_color(COLOR_GRAY, "--------------------------------------------------\n");
        kprintf_color(COLOR_WHITE, "INT: "); kprintf_color(COLOR_CYAN, "0x%x  ", int_no);
        kprintf_color(COLOR_WHITE, "ERR: "); kprintf_color(COLOR_CYAN, "0x%x  ", err_code);
        kprintf_color(COLOR_WHITE, "RIP: "); kprintf_color(COLOR_CYAN, "0x%lx\n", rip);
    }
    
    debug_print_stack_trace();
    
    hcf();
}
