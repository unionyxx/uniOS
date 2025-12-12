#include "ps2_keyboard.h"
#include "pic.h"
#include "io.h"
#include <stdint.h>

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint8_t kb_buffer_start = 0;
static volatile uint8_t kb_buffer_end = 0;

// Modifier states
static uint8_t shift_held = 0;
static uint8_t ctrl_held = 0;
static uint8_t caps_lock = 0;
static uint8_t extended_scancode = 0;  // For 0xE0 prefixed scancodes

// Special key codes for arrow keys and other keys (match shell.cpp definitions)
#define KEY_UP_ARROW    0x80
#define KEY_DOWN_ARROW  0x81
#define KEY_LEFT_ARROW  0x82
#define KEY_RIGHT_ARROW 0x83
#define KEY_HOME        0x84
#define KEY_END         0x85
#define KEY_DELETE      0x86
// Shift+Arrow for text selection
#define KEY_SHIFT_LEFT  0x90
#define KEY_SHIFT_RIGHT 0x91

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

static void push_char(char c) {
    uint8_t next = (kb_buffer_end + 1) % KB_BUFFER_SIZE;
    if (next != kb_buffer_start) {
        kb_buffer[kb_buffer_end] = c;
        kb_buffer_end = next;
    }
}

void ps2_keyboard_init() {
    // Flush any pending data in the keyboard buffer
    while (inb(KEYBOARD_STATUS_PORT) & 0x01) {
        inb(KEYBOARD_DATA_PORT);
    }
    pic_clear_mask(1);
}

void ps2_keyboard_handler() {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);
    
    // Handle extended scancode prefix
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }
    
    // Handle extended scancodes (arrow keys, Home, End, Delete, etc.)
    if (extended_scancode) {
        extended_scancode = 0;
        
        // Key release for extended keys - check for Ctrl
        if (scancode & 0x80) {
            uint8_t released = scancode & 0x7F;
            if (released == 0x1D) {  // Right Ctrl release
                ctrl_held = 0;
            }
            return;
        }
        
        // Extended key press
        switch (scancode) {
            case 0x1D: ctrl_held = 1; return;            // Right Ctrl
            case 0x48: push_char(KEY_UP_ARROW); return;   // Up arrow
            case 0x50: push_char(KEY_DOWN_ARROW); return; // Down arrow
            case 0x4B:  // Left arrow
                push_char(shift_held ? KEY_SHIFT_LEFT : KEY_LEFT_ARROW);
                return;
            case 0x4D:  // Right arrow
                push_char(shift_held ? KEY_SHIFT_RIGHT : KEY_RIGHT_ARROW);
                return;
            case 0x47: push_char(KEY_HOME); return;       // Home
            case 0x4F: push_char(KEY_END); return;        // End
            case 0x53: push_char(KEY_DELETE); return;     // Delete
            default: return;  // Unknown extended key
        }
    }
    
    // Key release (standard keys)
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) { // Left/Right Shift
            shift_held = 0;
        } else if (released == 0x1D) {  // Left Ctrl
            ctrl_held = 0;
        }
        return;
    }
    
    // Key press (standard keys)
    if (scancode == 0x2A || scancode == 0x36) { // Left/Right Shift
        shift_held = 1;
        return;
    }
    
    if (scancode == 0x1D) {  // Left Ctrl
        ctrl_held = 1;
        return;
    }
    
    if (scancode == 0x3A) { // Caps Lock
        caps_lock = !caps_lock;
        return;
    }
    
    char c;
    if (shift_held) {
        c = scancode_to_ascii_shift[scancode];
    } else {
        c = scancode_to_ascii[scancode];
    }
    
    // Handle Ctrl key combinations - generate control codes
    if (ctrl_held && c != 0) {
        // Convert letter to control code (a=1, b=2, ..., z=26)
        if (c >= 'a' && c <= 'z') {
            push_char(c - 'a' + 1);
            return;
        }
        if (c >= 'A' && c <= 'Z') {
            push_char(c - 'A' + 1);
            return;
        }
        // Special cases
        if (c == '[' || c == '{') { push_char(27); return; }  // Ctrl+[ = Escape
        if (c == '\\' || c == '|') { push_char(28); return; } // Ctrl+\
        if (c == ']' || c == '}') { push_char(29); return; }  // Ctrl+]
    }
    
    // Handle Caps Lock for letters (not when Ctrl is held)
    if (caps_lock && !ctrl_held) {
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        } else if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }
    
    if (c != 0) {
        push_char(c);
    }
}

uint8_t ps2_keyboard_has_char() {
    return kb_buffer_start != kb_buffer_end;
}

char ps2_keyboard_get_char() {
    if (kb_buffer_start == kb_buffer_end) {
        return 0;
    }
    char c = kb_buffer[kb_buffer_start];
    kb_buffer_start = (kb_buffer_start + 1) % KB_BUFFER_SIZE;
    return c;
}

