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

static void draw_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, char c, uint32_t color) {
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

static void clear_char(struct limine_framebuffer *fb, uint64_t x, uint64_t y, uint32_t bg_color) {
    // Fill the entire 8x8 character cell with background color
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 9; col++) { // 9 to include padding
            put_pixel(fb, x + col, y + row, bg_color);
        }
    }
}

static void draw_string(struct limine_framebuffer *fb, uint64_t x, uint64_t y, const char *str, uint32_t color) {
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

// Global framebuffer pointer for use in handlers
static struct limine_framebuffer* g_framebuffer = nullptr;
static uint64_t cursor_x = 50;
static uint64_t cursor_y = 210;

// Exception handler called from assembly
extern "C" void exception_handler(void* stack_frame) {
    (void)stack_frame;
    if (g_framebuffer) {
        draw_string(g_framebuffer, 50, 110, "EXCEPTION CAUGHT!", 0xFF0000);
    }
    hcf();
}

// IRQ handler called from assembly
extern "C" void irq_handler(void* stack_frame) {
    uint64_t* regs = (uint64_t*)stack_frame;
    uint64_t int_no = regs[15];
    uint8_t irq = int_no - 32;
    
    if (irq == 1) {
        keyboard_handler();
    }
    
    pic_send_eoi(irq);
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
    
    draw_string(framebuffer, 50, 190, "PIC & Keyboard Init...", 0xFFFF00);
    keyboard_init(); // Unmasks IRQ1
    draw_string(framebuffer, 50, 190, "PIC & Keyboard Ready. ", 0x00FF00);

    draw_string(framebuffer, 50, 210, "> ", 0x00FFFF);
    cursor_x = 68;

    // Enable interrupts
    asm("sti");

    // Main loop - process keyboard input
    while (true) {
        if (keyboard_has_char()) {
            char c = keyboard_get_char();
            if (c == '\n') {
                cursor_x = 50;
                cursor_y += 10;
                draw_char(framebuffer, cursor_x, cursor_y, '>', 0x00FFFF);
                cursor_x = 60;
            } else if (c == '\b') {
                if (cursor_x > 68) {
                    cursor_x -= 9;
                    clear_char(framebuffer, cursor_x, cursor_y, 0x000022);
                }
            } else {
                draw_char(framebuffer, cursor_x, cursor_y, c, 0xFFFFFF);
                cursor_x += 9;
            }
        }
        asm("hlt"); // Wait for next interrupt
    }
}
