#pragma once
#include <stdint.h>

// HID Class Requests
#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_GET_IDLE     0x02
#define HID_REQ_GET_PROTOCOL 0x03
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B

// HID Report types
#define HID_REPORT_INPUT   1
#define HID_REPORT_OUTPUT  2
#define HID_REPORT_FEATURE 3

// HID Protocol
#define HID_PROTOCOL_BOOT   0
#define HID_PROTOCOL_REPORT 1

// Boot protocol keyboard report (8 bytes)
struct HidKeyboardReport {
    uint8_t modifiers;    // Modifier keys (Ctrl, Shift, Alt, GUI)
    uint8_t reserved;
    uint8_t keys[6];      // Up to 6 simultaneous key presses
} __attribute__((packed));

// Keyboard modifier bits
#define HID_MOD_LEFT_CTRL   (1 << 0)
#define HID_MOD_LEFT_SHIFT  (1 << 1)
#define HID_MOD_LEFT_ALT    (1 << 2)
#define HID_MOD_LEFT_GUI    (1 << 3)
#define HID_MOD_RIGHT_CTRL  (1 << 4)
#define HID_MOD_RIGHT_SHIFT (1 << 5)
#define HID_MOD_RIGHT_ALT   (1 << 6)
#define HID_MOD_RIGHT_GUI   (1 << 7)

// Boot protocol mouse report (3-4 bytes)
struct HidMouseReport {
    uint8_t buttons;      // Button states
    int8_t  x;            // X movement (signed)
    int8_t  y;            // Y movement (signed)
    int8_t  wheel;        // Scroll wheel (optional, may not be present)
} __attribute__((packed));

// Mouse button bits
#define HID_MOUSE_LEFT   (1 << 0)
#define HID_MOUSE_RIGHT  (1 << 1)
#define HID_MOUSE_MIDDLE (1 << 2)

// HID functions
void usb_hid_init();
void usb_hid_poll();

// Keyboard functions
bool usb_hid_keyboard_available();
bool usb_hid_keyboard_has_char();
char usb_hid_keyboard_get_char();

// Mouse functions
bool usb_hid_mouse_available();
void usb_hid_mouse_get_state(int32_t* x, int32_t* y, bool* left, bool* right, bool* middle);
int8_t usb_hid_mouse_get_scroll();  // Get scroll wheel delta since last call
void usb_hid_set_screen_size(int32_t width, int32_t height);

// Debug control
void usb_hid_set_debug(bool enabled);
