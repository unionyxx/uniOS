#include <stdint.h>
#include <stddef.h>
#include "limine.h"

// Set the base revision to 2, this is recommended as it is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

// The Limine requests can be placed anywhere, but it is good practice
// to place them in the same section, e.g. .requests.

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

// Define the start and end markers for the Limine requests.
// These can also be omitted if you don't need them, but they are good practice.
__attribute__((used, section(".requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

#include "font.h"

// Halt and catch fire function.
static void hcf(void) {
    asm("cli");
    for (;;) {
        asm("hlt");
    }
}

static void put_pixel(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) return;
    
    // Assuming 32-bit color (4 bytes per pixel)
    // We should check fb->bpp, but for now we assume 32.
    uint32_t *fb_ptr = (uint32_t*)fb->address;
    fb_ptr[y * (fb->pitch / 4) + x] = color;
}

void draw_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, char c, uint32_t color) {
    if (c < 0 || c > 127) return;
    
    const uint8_t *glyph = font8x8[(int)c];
    
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if ((glyph[row] >> (7 - col)) & 1) {
                put_pixel(fb, x + col, y + row, color);
            }
        }
    }
}

void clear_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint32_t bg_color) {
    // Fill the entire 8x8 character cell with background color
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 9; col++) { // 9 to include padding
            put_pixel(fb, x + col, y + row, bg_color);
        }
    }
}

void draw_string(struct limine_framebuffer *fb, uint64_t x, uint64_t y, const char *str, uint32_t color) {
    uint64_t cursor_x = x;
    uint64_t cursor_y = y;
    
    while (*str) {
        if (*str == '\n') {
            cursor_x = x;
            cursor_y += 10; // 8px height + 2px padding
        } else {
            draw_char(fb, cursor_x, cursor_y, *str, color);
            cursor_x += 9; // 8px width + 1px padding
        }
        str++;
    }
}

#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "scheduler.h"
#include "unifs.h"
#include "shell.h"

// Global framebuffer pointer for use in handlers
static struct limine_framebuffer* g_framebuffer = nullptr;
static uint64_t cursor_x = 50;
static uint64_t cursor_y = 210;

// Exception handler called from assembly
extern "C" void exception_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    // Stack layout from isr_common_stub:
    // [0-14] Pushed regs (R15...RAX)
    // [15] Int No
    // [16] Err Code
    // [17] RIP
    // [18] CS
    // [19] RFLAGS
    
    uint64_t int_no = regs[15];
    uint64_t err_code = regs[16];
    uint64_t rip = regs[17];

    if (g_framebuffer) {
        draw_string(g_framebuffer, 50, 400, "EXCEPTION CAUGHT!", 0xFF0000);
        
        char buf[32];
        
        // Print Int No
        buf[0] = 'I'; buf[1] = 'N'; buf[2] = 'T'; buf[3] = ':'; buf[4] = ' ';
        uint64_t val = int_no;
        for (int i = 0; i < 16; i++) {
            int nibble = (val >> (60 - i*4)) & 0xF;
            buf[5+i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        }
        buf[21] = 0;
        draw_string(g_framebuffer, 50, 420, buf, 0xFFFFFF);
        
        // Print Err Code
        buf[0] = 'E'; buf[1] = 'R'; buf[2] = 'R'; buf[3] = ':'; buf[4] = ' ';
        val = err_code;
        for (int i = 0; i < 16; i++) {
            int nibble = (val >> (60 - i*4)) & 0xF;
            buf[5+i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        }
        buf[21] = 0;
        draw_string(g_framebuffer, 50, 440, buf, 0xFFFFFF);
        
        // Print RIP
        buf[0] = 'R'; buf[1] = 'I'; buf[2] = 'P'; buf[3] = ':'; buf[4] = ' ';
        val = rip;
        for (int i = 0; i < 16; i++) {
            int nibble = (val >> (60 - i*4)) & 0xF;
            buf[5+i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        }
        buf[21] = 0;
        draw_string(g_framebuffer, 50, 460, buf, 0xFFFFFF);
    }
    hcf();
}

// IRQ handler called from assembly
extern "C" void irq_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    uint64_t int_no = regs[15];
    uint8_t irq = int_no - 32;
    
    pic_send_eoi(irq);

    if (irq == 0) {
        timer_handler();
        scheduler_schedule();
    } else if (irq == 1) {
        keyboard_handler();
    }
}

// The following will be our kernel's entry point.
extern "C" void _start(void) {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED == false) {
        hcf();
    }

    // Ensure we got a framebuffer.
    if (framebuffer_request.response == NULL
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    // Fetch the first framebuffer.
    struct limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[0];
    g_framebuffer = framebuffer;

    // Clear screen to a deep blue (uniOS aesthetic)
    for (uint64_t y = 0; y < framebuffer->height; y++) {
        for (uint64_t x = 0; x < framebuffer->width; x++) {
            put_pixel(framebuffer, x, y, 0x000022); // Deep Blue
        }
    }

    // Draw welcome text
    draw_string(framebuffer, 50, 50, "Welcome to uniOS", 0xFFFFFF);
    draw_string(framebuffer, 50, 70, "System Initialized.", 0xAAAAAA);
    draw_string(framebuffer, 50, 90, "Kernel: C++ Bare Bones", 0x888888);

    // Initialize GDT and IDT
    draw_string(framebuffer, 50, 110, "Initializing GDT...", 0xFFFF00);
    gdt_init();
    draw_string(framebuffer, 50, 130, "GDT Loaded.", 0x00FF00);
    
    draw_string(framebuffer, 50, 150, "Initializing IDT...", 0xFFFF00);
    idt_init();
    draw_string(framebuffer, 50, 170, "IDT Loaded.", 0x00FF00);

    // Initialize PIC (do this before drawing, to avoid spurious interrupts)
    pic_remap(32, 40); // Remap IRQs to vectors 32-47
    
    // Mask all IRQs first
    for (int i = 0; i < 16; i++) {
        pic_set_mask(i);
    }
    
    keyboard_init(); // Unmasks IRQ1
    timer_init(100); // 100Hz timer (10ms per tick)
    
    draw_string(framebuffer, 50, 190, "Hardware Ready.", 0x00FF00);
    
    draw_string(framebuffer, 50, 210, "Initializing PMM...", 0xFFFF00);
    pmm_init();
    draw_string(framebuffer, 50, 230, "PMM Initialized.   ", 0x00FF00);
    
    draw_string(framebuffer, 50, 250, "Initializing VMM...", 0xFFFF00);
    vmm_init();
    draw_string(framebuffer, 50, 270, "VMM Initialized.   ", 0x00FF00);
    
    // Initialize Heap (64KB)
    void* heap_start_phys = pmm_alloc_frame();
    if (heap_start_phys) {
        // Try to allocate 15 more contiguous frames
        bool contiguous = true;
        void* current_phys = heap_start_phys;
        for (int i = 0; i < 15; i++) {
            void* next_phys = pmm_alloc_frame();
            if ((uint64_t)next_phys != (uint64_t)current_phys + 4096) {
                contiguous = false;
            }
            current_phys = next_phys;
        }

        if (contiguous) {
            void* heap_virt = (void*)vmm_phys_to_virt((uint64_t)heap_start_phys);
            heap_init(heap_virt, 64 * 1024); // 64KB
            draw_string(framebuffer, 50, 290, "Heap Initialized (64KB).", 0x00FF00);
            
            // Test allocation
            char* test_str = (char*)malloc(16);
            if (test_str) {
                test_str[0] = 'H'; test_str[1] = 'e'; test_str[2] = 'a'; test_str[3] = 'p'; test_str[4] = 0;
                draw_string(framebuffer, 300, 290, test_str, 0x00FFFF);
                free(test_str);
            }
        } else {
            draw_string(framebuffer, 50, 290, "Heap Init Failed (Frag).", 0xFF0000);
        }
    }
    
    // Display memory stats
    uint64_t free_mem = pmm_get_free_memory() / 1024 / 1024;
    uint64_t total_mem = pmm_get_total_memory() / 1024 / 1024;
    
    // Simple integer to string conversion for display
    char mem_str[64] = "Memory: ";
    // Append free_mem
    uint64_t n = free_mem;
    int i = 8;
    if (n == 0) mem_str[i++] = '0';
    else {
        char buf[20];
        int j = 0;
        while (n > 0) { buf[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) mem_str[i++] = buf[--j];
    }
    mem_str[i++] = 'M'; mem_str[i++] = 'B'; mem_str[i++] = '/';
    // Append total_mem
    n = total_mem;
    if (n == 0) mem_str[i++] = '0';
    else {
        char buf[20];
        int j = 0;
        while (n > 0) { buf[j++] = '0' + (n % 10); n /= 10; }
        while (j > 0) mem_str[i++] = buf[--j];
    }
    mem_str[i++] = 'M'; mem_str[i++] = 'B'; mem_str[i] = 0;
    
    // Initialize Scheduler
    scheduler_init();
    
    // Initialize Filesystem
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file* module = module_request.response->modules[0];
        unifs_init(module->address);
        draw_string(framebuffer, 50, 310, "uniFS Initialized.", 0x00FF00);
        
        // Read hello.txt
        const UniFSFile* file = unifs_open("hello.txt");
        if (file) {
            char buf[64];
            // Copy content to buffer to ensure null termination for printing
            for (uint64_t i = 0; i < file->size && i < 63; i++) {
                buf[i] = file->data[i];
            }
            buf[file->size < 63 ? file->size : 63] = 0;
            
            draw_string(framebuffer, 300, 310, "Read: ", 0xFFFF00);
            draw_string(framebuffer, 350, 310, buf, 0xFFFFFF);
        } else {
            draw_string(framebuffer, 300, 310, "File not found.", 0xFF0000);
        }
    } else {
        draw_string(framebuffer, 50, 310, "No FS Module.", 0xFF0000);
    }
    
    // Create a test task
    scheduler_create_task([]() {
        while (true) {
            if (g_framebuffer) {
                // Draw 'A' at a specific location
                static int x = 50;
                draw_char(g_framebuffer, x, 350, 'A', 0xFF00FF);
                x += 9;
                if (x > 200) x = 50;
                
                // Delay loop
                for (volatile int i = 0; i < 1000000; i++);
            }
            scheduler_yield();
        }
    });
    
    // Create another test task
    scheduler_create_task([]() {
        while (true) {
            if (g_framebuffer) {
                // Draw 'B' at a specific location
                static int x = 250;
                draw_char(g_framebuffer, x, 350, 'B', 0x00FFFF);
                x += 9;
                if (x > 400) x = 250;
                
                // Delay loop
                for (volatile int i = 0; i < 1000000; i++);
            }
            scheduler_yield();
        }
    });
    
    draw_string(framebuffer, 50, 330, "Scheduler Initialized.", 0x00FF00);
    
    // Initialize Shell
    shell_init(framebuffer);
    draw_string(framebuffer, 50, 370, "> ", 0x00FFFF);

    // Enable interrupts
    asm("sti");

    // Main loop (Idle task)
    while (true) {
        if (keyboard_has_char()) {
            char c = keyboard_get_char();
            shell_process_char(c);
        }
        
        // Yield to other tasks
        scheduler_yield();
        asm("hlt");
    }
}
