#include <stdint.h>
#include <stddef.h>
#include "limine.h"

// Limine base revision
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(2);

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

__attribute__((used, section(".requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

#include "font.h"

static void hcf(void) {
    asm("cli");
    for (;;) asm("hlt");
}

static void put_pixel(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) return;
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
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 9; col++) {
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
            cursor_y += 10;
        } else {
            draw_char(fb, cursor_x, cursor_y, *str, color);
            cursor_x += 9;
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
#include "mouse.h"
#include "debug.h"

// Global framebuffer pointer
struct limine_framebuffer* g_framebuffer = nullptr;

// Exception handler
extern "C" void exception_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    uint64_t int_no = regs[15];
    uint64_t err_code = regs[16];
    uint64_t rip = regs[17];

    if (g_framebuffer) {
        draw_string(g_framebuffer, 50, 400, "EXCEPTION!", 0xFF0000);
        char buf[32];
        
        buf[0] = 'I'; buf[1] = 'N'; buf[2] = 'T'; buf[3] = ':'; buf[4] = ' ';
        for (int i = 0; i < 16; i++) {
            int nibble = (int_no >> (60 - i*4)) & 0xF;
            buf[5+i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        }
        buf[21] = 0;
        draw_string(g_framebuffer, 50, 420, buf, 0xFFFFFF);
        
        buf[0] = 'E'; buf[1] = 'R'; buf[2] = 'R'; buf[3] = ':'; buf[4] = ' ';
        for (int i = 0; i < 16; i++) {
            int nibble = (err_code >> (60 - i*4)) & 0xF;
            buf[5+i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        }
        buf[21] = 0;
        draw_string(g_framebuffer, 50, 440, buf, 0xFFFFFF);
        
        buf[0] = 'R'; buf[1] = 'I'; buf[2] = 'P'; buf[3] = ':'; buf[4] = ' ';
        for (int i = 0; i < 16; i++) {
            int nibble = (rip >> (60 - i*4)) & 0xF;
            buf[5+i] = nibble < 10 ? '0' + nibble : 'A' + nibble - 10;
        }
        buf[21] = 0;
        draw_string(g_framebuffer, 50, 460, buf, 0xFFFFFF);
    }
    hcf();
}

// IRQ handler
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
    } else if (irq == 12) {
        mouse_handler();
    }
}

// User mode test program
static void user_program() __attribute__((section(".user_code")));
static void user_program() {
    const char* msg = "Hello from User Mode!\n";
    asm volatile(
        "mov $1, %%rax\n"
        "mov %0, %%rbx\n"
        "mov $22, %%rcx\n"
        "int $0x80\n"
        : : "r"(msg) : "rax", "rbx", "rcx"
    );
    asm volatile("mov $60, %%rax\n" "int $0x80\n" : : : "rax");
    for(;;);
}

static uint8_t user_stack[4096] __attribute__((aligned(16)));
extern "C" void jump_to_user_mode(uint64_t code_sel, uint64_t stack, uint64_t entry);

void run_user_test() {
    user_program();
}

// GUI Mode
#include "graphics.h"

static uint32_t cursor_backup[12 * 19];
static int32_t backup_x = -1, backup_y = -1;

static void save_cursor_area(int32_t x, int32_t y) {
    uint32_t* fb = (uint32_t*)g_framebuffer->address;
    uint32_t pitch = g_framebuffer->pitch / 4;
    int idx = 0;
    for (int row = 0; row < 19; row++) {
        for (int col = 0; col < 12; col++) {
            int32_t px = x + col, py = y + row;
            if (px >= 0 && py >= 0 && px < (int32_t)g_framebuffer->width && py < (int32_t)g_framebuffer->height) {
                cursor_backup[idx] = fb[py * pitch + px];
            }
            idx++;
        }
    }
    backup_x = x; backup_y = y;
}

static void restore_cursor_area() {
    if (backup_x < 0) return;
    uint32_t* fb = (uint32_t*)g_framebuffer->address;
    uint32_t pitch = g_framebuffer->pitch / 4;
    int idx = 0;
    for (int row = 0; row < 19; row++) {
        for (int col = 0; col < 12; col++) {
            int32_t px = backup_x + col, py = backup_y + row;
            if (px >= 0 && py >= 0 && px < (int32_t)g_framebuffer->width && py < (int32_t)g_framebuffer->height) {
                fb[py * pitch + px] = cursor_backup[idx];
            }
            idx++;
        }
    }
}

void gui_start() {
    mouse_init();
    gfx_init(g_framebuffer);
    gfx_clear(COLOR_DESKTOP);
    gfx_fill_rect(0, g_framebuffer->height - 30, g_framebuffer->width, 30, COLOR_DARK_GRAY);
    draw_string(g_framebuffer, 10, g_framebuffer->height - 22, "uniOS Desktop - Press Q to exit", 0xFFFFFF);
    
    bool running = true;
    backup_x = -1;
    
    while (running) {
        const MouseState* mouse = mouse_get_state();
        if (mouse->x != backup_x || mouse->y != backup_y) {
            restore_cursor_area();
            save_cursor_area(mouse->x, mouse->y);
            gfx_draw_cursor(mouse->x, mouse->y);
        }
        if (keyboard_has_char()) {
            char c = keyboard_get_char();
            if (c == 'q' || c == 'Q' || c == 27) running = false;
        }
        for (volatile int i = 0; i < 50000; i++);
    }
    
    // Restore shell screen
    uint32_t* fb = (uint32_t*)g_framebuffer->address;
    for (uint64_t y = 0; y < g_framebuffer->height; y++) {
        for (uint64_t x = 0; x < g_framebuffer->width; x++) {
            fb[y * (g_framebuffer->pitch / 4) + x] = 0x000022;
        }
    }
    draw_string(g_framebuffer, 50, 50, "uniOS Shell (uniSH)", 0xFFFFFF);
}

// Kernel entry point
extern "C" void _start(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED) hcf();
    if (!framebuffer_request.response || framebuffer_request.response->framebuffer_count < 1) hcf();

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    g_framebuffer = fb;

    // Clear screen
    for (uint64_t y = 0; y < fb->height; y++) {
        for (uint64_t x = 0; x < fb->width; x++) {
            put_pixel(fb, x, y, 0x000022);
        }
    }

    // Welcome message
    draw_string(fb, 50, 50, "uniOS v0.1", 0xFFFFFF);

    // Initialize core systems
    gdt_init();
    idt_init();
    pic_remap(32, 40);
    for (int i = 0; i < 16; i++) pic_set_mask(i);
    keyboard_init();
    timer_init(100);
    pmm_init();
    vmm_init();
    
    // Initialize heap
    void* heap_start = pmm_alloc_frame();
    if (heap_start) {
        bool contiguous = true;
        void* current = heap_start;
        for (int i = 0; i < 15; i++) {
            void* next = pmm_alloc_frame();
            if ((uint64_t)next != (uint64_t)current + 4096) contiguous = false;
            current = next;
        }
        if (contiguous) {
            heap_init((void*)vmm_phys_to_virt((uint64_t)heap_start), 64 * 1024);
        }
    }
    
    scheduler_init();
    
    // Initialize filesystem
    if (module_request.response && module_request.response->module_count > 0) {
        unifs_init(module_request.response->modules[0]->address);
    }
    
    // Initialize shell
    shell_init(fb);
    draw_string(fb, 50, 70, "Type 'help' for commands.", 0x888888);
    draw_string(fb, 50, 90, "> ", 0x00FFFF);

    asm("sti");

    // Main loop
    while (true) {
        if (keyboard_has_char()) {
            shell_process_char(keyboard_get_char());
        }
        scheduler_yield();
        asm("hlt");
    }
}
