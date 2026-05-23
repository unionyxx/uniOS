#include <drivers/apic/ioapic.h>
#include <drivers/class/hid/ps2_keyboard.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/irq.h>
#include <kernel/scheduler.h>
#include <stdint.h>

extern "C" void signal_send_current(int sig);

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static uint32_t kb_buffer_start = 0;
static uint32_t kb_buffer_end = 0;

// Modifier states
static uint8_t shift_held = 0;
static uint8_t ctrl_held = 0;
static uint8_t alt_held = 0;
static uint8_t caps_lock = 0;
static uint8_t extended_scancode = 0; // For 0xE0 prefixed scancodes

// Special key codes for arrow keys and other keys (match shell.cpp definitions)
#define KEY_UP_ARROW 0x80
#define KEY_DOWN_ARROW 0x81
#define KEY_LEFT_ARROW 0x82
#define KEY_RIGHT_ARROW 0x83
#define KEY_HOME 0x84
#define KEY_END 0x85
#define KEY_DELETE 0x86
#define KEY_PAGEUP 0x87
#define KEY_PAGEDOWN 0x88
// Shift+Arrow for text selection
#define KEY_SHIFT_LEFT 0x90
#define KEY_SHIFT_RIGHT 0x91

// US keyboard layout (lowercase)
static const char scancode_to_ascii[128] = {
    0,   27,   '1', '2', '3', '4', '5', '6',  '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',  't',
    'y', 'u',  'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's', 'd', 'f', 'g', 'h',  'j',  'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n',  'm', ',', '.', '/', 0,   '*', 0,    ' ',  0,   0,   0,   0,    0,
    0,   0,    0,   0,   0,   0,   0,   0,    0,   0,   0,   '-', 0,   0,   0,    '+',  0,   0,   0,   0,    0,
    0,   0,    0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,    0,    0,   0,   0,   0,    0,
    0,   0,    0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,    0,    0};

// Shifted characters
static const char scancode_to_ascii_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^',  '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T',
    'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H',  'J',  'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N',  'M', '<', '>', '?', 0,   '*', 0,    ' ',  0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   '-', 0,   0,   0,    '+',  0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,    0,    0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,    0,   0,   0,   0,   0,   0,   0,    0,    0};

static void push_char(char c)
{
    uint32_t tail = __atomic_load_n(&kb_buffer_end, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&kb_buffer_start, __ATOMIC_ACQUIRE);
    if (tail - head < KB_BUFFER_SIZE) {
        kb_buffer[tail & (KB_BUFFER_SIZE - 1)] = c;
        __atomic_store_n(&kb_buffer_end, tail + 1, __ATOMIC_RELEASE);
        scheduler_notify_input_waiters();
    }
}

void ps2_keyboard_init()
{
    // Guard against floating bus on missing hardware (0xFF)
    if (inb(KEYBOARD_STATUS_PORT) == 0xFF) {
        return;
    }

    // Flush any pending data in the keyboard buffer
    uint32_t timeout = 1000;
    while (timeout-- && (inb(KEYBOARD_STATUS_PORT) & 0x01)) {
        inb(KEYBOARD_DATA_PORT);
    }

    if (apic_is_enabled() && ioapic_is_ready()) {
        ioapic_set_entry(1, irq_isa_to_vector(1));
        return;
    }

    pic_clear_mask(1);
}

void ps2_keyboard_handler()
{
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    // Handle extended scancode prefix
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }

    // Handle extended scancodes (arrow keys, Home, End, Delete, etc.)
    if (extended_scancode) {
        extended_scancode = 0;

        // Key release for extended keys
        if (scancode & 0x80) {
            uint8_t released = scancode & 0x7F;
            if (released == 0x1D) { // Right Ctrl release
                ctrl_held = 0;
            } else if (released == 0x38) { // Right Alt release
                alt_held = 0;
            }
            return;
        }

        // Extended key press
        switch (scancode) {
            case 0x1D:
                ctrl_held = 1;
                return; // Right Ctrl
            case 0x38:
                alt_held = 1;
                return; // Right Alt
            case 0x48:
                push_char(KEY_UP_ARROW);
                return; // Up arrow
            case 0x50:
                push_char(KEY_DOWN_ARROW);
                return; // Down arrow
            case 0x4B:  // Left arrow
                push_char(shift_held ? KEY_SHIFT_LEFT : KEY_LEFT_ARROW);
                return;
            case 0x4D: // Right arrow
                push_char(shift_held ? KEY_SHIFT_RIGHT : KEY_RIGHT_ARROW);
                return;
            case 0x47:
                push_char(KEY_HOME);
                return; // Home
            case 0x49:
                push_char(KEY_PAGEUP);
                return; // Page Up
            case 0x51:
                push_char(KEY_PAGEDOWN);
                return; // Page Down
            case 0x4F:
                push_char(KEY_END);
                return; // End
            case 0x53:
                push_char(KEY_DELETE);
                return; // Delete
            default:
                return; // Unknown extended key
        }
    }

    // Key release (standard keys)
    if (scancode & 0x80) {
        uint8_t released = scancode & 0x7F;
        if (released == 0x2A || released == 0x36) { // Left/Right Shift
            shift_held = 0;
        } else if (released == 0x1D) { // Left Ctrl
            ctrl_held = 0;
        } else if (released == 0x38) { // Left Alt
            alt_held = 0;
        }
        return;
    }

    // Key press (standard keys)
    if (scancode == 0x2A || scancode == 0x36) { // Left/Right Shift
        shift_held = 1;
        return;
    }

    if (scancode == 0x1D) { // Left Ctrl
        ctrl_held = 1;
        return;
    }

    if (scancode == 0x38) { // Left Alt
        alt_held = 1;
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

    // Alt+Space for Index trigger
    if (alt_held && c == ' ') {
        push_char(29); // Use GS (Group Separator) as Index trigger
        scheduler_notify_input_waiters();
        return;
    }

    // Handle Ctrl key combinations - generate control codes
    if (ctrl_held && c != 0) {
        // Convert letter to control code (a=1, b=2, ..., z=26)
        if (c >= 'a' && c <= 'z') {
            char ctrl_c = c - 'a' + 1;
            if (ctrl_c == 3) {
                signal_send_current(2); // SIGINT
                return;
            }
            push_char(ctrl_c);
            return;
        }
        if (c >= 'A' && c <= 'Z') {
            push_char(c - 'A' + 1);
            return;
        }
    }

    // Handle Caps Lock for letters
    if (caps_lock && !ctrl_held) {
        if (c >= 'a' && c <= 'z') {
            c = c - 'a' + 'A';
        } else if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }
    }

    if (c != 0) {
        push_char(c);
        scheduler_notify_input_waiters();
    }
}

uint8_t ps2_keyboard_has_char()
{
    uint32_t head = __atomic_load_n(&kb_buffer_start, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&kb_buffer_end, __ATOMIC_ACQUIRE);
    return head != tail;
}

char ps2_keyboard_get_char()
{
    uint32_t head = __atomic_load_n(&kb_buffer_start, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&kb_buffer_end, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return 0;
    }
    char c = kb_buffer[head & (KB_BUFFER_SIZE - 1)];
    __atomic_store_n(&kb_buffer_start, head + 1, __ATOMIC_RELEASE);
    return c;
}
