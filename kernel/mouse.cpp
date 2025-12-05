#include "mouse.h"
#include "pic.h"
#include "limine.h"

extern struct limine_framebuffer* g_framebuffer;

static MouseState state = {0, 0, false, false, false};
static uint8_t mouse_cycle = 0;
static int8_t mouse_byte[3];

// PS/2 mouse ports
#define MOUSE_DATA    0x60
#define MOUSE_STATUS  0x64
#define MOUSE_COMMAND 0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        // Wait for data
        while (timeout--) {
            if ((inb(MOUSE_STATUS) & 1) == 1) return;
        }
    } else {
        // Wait to send
        while (timeout--) {
            if ((inb(MOUSE_STATUS) & 2) == 0) return;
        }
    }
}

static void mouse_write(uint8_t data) {
    mouse_wait(1);
    outb(MOUSE_COMMAND, 0xD4);  // Tell controller we're sending to mouse
    mouse_wait(1);
    outb(MOUSE_DATA, data);
}

static uint8_t mouse_read() {
    mouse_wait(0);
    return inb(MOUSE_DATA);
}

void mouse_init() {
    // Enable auxiliary device (mouse)
    mouse_wait(1);
    outb(MOUSE_COMMAND, 0xA8);
    
    // Enable interrupts
    mouse_wait(1);
    outb(MOUSE_COMMAND, 0x20);  // Get compaq status
    mouse_wait(0);
    uint8_t status = inb(MOUSE_DATA) | 2;  // Enable IRQ12
    mouse_wait(1);
    outb(MOUSE_COMMAND, 0x60);  // Set compaq status
    mouse_wait(1);
    outb(MOUSE_DATA, status);
    
    // Use default settings
    mouse_write(0xF6);
    mouse_read();  // Acknowledge
    
    // Enable mouse
    mouse_write(0xF4);
    mouse_read();  // Acknowledge
    
    // Initialize position to center of screen
    if (g_framebuffer) {
        state.x = g_framebuffer->width / 2;
        state.y = g_framebuffer->height / 2;
    }
    
    // Unmask IRQ2 (cascade) and IRQ12 (mouse) in PIC
    pic_clear_mask(2);   // Enable cascade from slave PIC
    pic_clear_mask(12);  // Enable mouse IRQ
}

void mouse_handler() {
    uint8_t data = inb(MOUSE_DATA);
    
    switch (mouse_cycle) {
        case 0:
            mouse_byte[0] = data;
            if (data & 0x08) {  // Validate byte (bit 3 always set)
                mouse_cycle++;
            }
            break;
        case 1:
            mouse_byte[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_byte[2] = data;
            mouse_cycle = 0;
            
            // Parse mouse packet
            state.left_button = mouse_byte[0] & 0x01;
            state.right_button = mouse_byte[0] & 0x02;
            state.middle_button = mouse_byte[0] & 0x04;
            
            // Update position
            int32_t dx = mouse_byte[1];
            int32_t dy = mouse_byte[2];
            
            // Handle sign extension
            if (mouse_byte[0] & 0x10) dx |= 0xFFFFFF00;
            if (mouse_byte[0] & 0x20) dy |= 0xFFFFFF00;
            
            state.x += dx;
            state.y -= dy;  // Y is inverted
            
            // Clamp to screen bounds
            if (g_framebuffer) {
                if (state.x < 0) state.x = 0;
                if (state.y < 0) state.y = 0;
                if (state.x >= (int32_t)g_framebuffer->width) 
                    state.x = g_framebuffer->width - 1;
                if (state.y >= (int32_t)g_framebuffer->height) 
                    state.y = g_framebuffer->height - 1;
            }
            break;
    }
}

const MouseState* mouse_get_state() {
    return &state;
}
