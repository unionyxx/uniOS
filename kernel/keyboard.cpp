#include "keyboard.h"
#include "pic.h"
#include <stdint.h>

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint8_t kb_buffer_start = 0;
static volatile uint8_t kb_buffer_end = 0;

// Shift state
static uint8_t shift_held = 0;

// US keyboard layout (lowercase)
static const char scancode_to_ascii[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Shifted characters
static const char scancode_to_ascii_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void keyboard_init() {
    pic_clear_mask(1);
}

void keyboard_handler() {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Key release
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) { // Left/Right Shift
            shift_held = 0;
        }
        return;
    }
    
    // Key press
    if (scancode == 0x2A || scancode == 0x36) { // Left/Right Shift
        shift_held = 1;
        return;
    }
    
    char c;
    if (shift_held) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }
    
    if (c != 0) {
        uint8_t next = (kb_buffer_end + 1) % KB_BUFFER_SIZE;
        if (next != kb_buffer_start) {
            kb_buffer[kb_buffer_end] = c;
            kb_buffer_end = next;
        }
    }
}

uint8_t keyboard_has_char() {
    return kb_buffer_start != kb_buffer_end;
}

char keyboard_get_char() {
    if (kb_buffer_start == kb_buffer_end) {
        return 0;
    }
    char c = kb_buffer[kb_buffer_start];
    kb_buffer_start = (kb_buffer_start + 1) % KB_BUFFER_SIZE;
    return c;
}
