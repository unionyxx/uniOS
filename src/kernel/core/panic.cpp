#include <kernel/panic.h>
#include <kernel/debug.h>
#include <drivers/video/framebuffer.h>

void hcf(void) {
    asm("cli");
    for (;;) asm("hlt");
}

void panic(const char* message) {
    // Red background for panic
    if (gfx_get_width() > 0) {
        gfx_clear(0x550000); // Dark red
    }
    
    // Reset debug cursor to ensure message is visible
    // We might need to expose a way to reset debug cursor or just print newlines
    kprintf("\n\n");
    
    kprintf_color(0xFFFFFF, "=== KERNEL PANIC ===\n\n");
    kprintf_color(0xFFFFFF, "%s\n", message);
    
    // Print stack trace to help debugging
    debug_print_stack_trace();
    
    kprintf_color(0xFFFFFF, "\nSystem halted.");
    
    hcf();
}

extern "C" void exception_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    uint64_t int_no = regs[15];
    uint64_t err_code = regs[16];
    uint64_t rip = regs[17];

    // Red background for exception
    if (gfx_get_width() > 0) {
        // We don't want to clear the whole screen if we can avoid it, 
        // but for now let's make it visible
        // gfx_clear(0x550000); 
    }

    kprintf_color(0xFF0000, "\nEXCEPTION CAUGHT!\n");
    kprintf("INT: 0x%x  ERROR: 0x%x  RIP: 0x%lx\n", int_no, err_code, rip);
    
    // Print stack trace to help debugging
    debug_print_stack_trace();
    
    hcf();
}
