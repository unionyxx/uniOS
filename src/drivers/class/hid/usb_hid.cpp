/**
 * @file usb_hid.cpp
 * @brief USB HID (Human Interface Device) driver for uniOS
 */

#include <drivers/class/hid/usb_hid.h>
#include <drivers/bus/usb/usb.h>
#include <drivers/bus/usb/xhci/xhci.h>
#include <kernel/time/timer.h>
#include <kernel/debug.h>
#include <stddef.h>

// Keyboard state
static bool keyboard_available = false;
static UsbDeviceInfo* keyboard_device = nullptr;
static HidKeyboardReport last_kbd_report = {0, 0, {0}};

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint8_t kb_head = 0;
static volatile uint8_t kb_tail = 0;

// Mouse state
static bool mouse_available = false;
static bool mouse_data_received = false;
static UsbDeviceInfo* mouse_device = nullptr;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static bool mouse_left = false;
static bool mouse_right = false;
static bool mouse_middle = false;
static int8_t mouse_scroll = 0;

// Screen dimensions
static int32_t screen_width = 1024;
static int32_t screen_height = 768;

// Key repeat
static uint8_t repeat_key = 0;
static uint64_t repeat_start = 0;
static uint64_t repeat_last = 0;
static bool repeat_shift = false;
static const uint32_t REPEAT_DELAY = 500;
static const uint32_t REPEAT_RATE = 33;

// Special key codes (matching PS/2)
#define KEY_UP       0x80
#define KEY_DOWN     0x81
#define KEY_LEFT     0x82
#define KEY_RIGHT    0x83
#define KEY_HOME     0x84
#define KEY_END      0x85
#define KEY_DELETE   0x86
#define KEY_PAGEUP   0x87
#define KEY_PAGEDOWN 0x88
#define KEY_SHIFT_LEFT  0x90
#define KEY_SHIFT_RIGHT 0x91

// HID keycode to ASCII (lowercase)
static const char hid_to_ascii[128] = {
    0,    0,    0,    0,   'a',  'b',  'c',  'd',
    'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',
    'm',  'n',  'o',  'p',  'q',  'r',  's',  't',
    'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2',
    '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',
    '\n', 27,   '\b', '\t', ' ',  '-',  '=',  '[',
    ']',  '\\', '#',  ';',  '\'', '`',  ',',  '.',
    '/',  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    (char)KEY_HOME, (char)KEY_PAGEUP, (char)KEY_DELETE, (char)KEY_END, (char)KEY_PAGEDOWN, (char)KEY_RIGHT,
    (char)KEY_LEFT, (char)KEY_DOWN, (char)KEY_UP, 0, '/',  '*',  '-',  '+',
    '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7',
    '8',  '9',  '0',  '.',  0,    0,    0,    '=',
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0
};

// HID keycode to ASCII (shifted)
static const char hid_to_ascii_shift[128] = {
    0,    0,    0,    0,   'A',  'B',  'C',  'D',
    'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',
    'M',  'N',  'O',  'P',  'Q',  'R',  'S',  'T',
    'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@',
    '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',
    '\n', 27,   '\b', '\t', ' ',  '_',  '+',  '{',
    '}',  '|',  '~',  ':',  '"',  '~',  '<',  '>',
    '?',  0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    '/',  '*',  '-',  '+',
    '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7',
    '8',  '9',  '0',  '.',  0,    0,    0,    '=',
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0,    0,    0,    0
};

static bool key_was_pressed(uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (last_kbd_report.keys[i] == keycode) return true;
    }
    return false;
}

static void kb_push(char c) {
    uint8_t next = (kb_tail + 1) % KB_BUFFER_SIZE;
    if (next != kb_head) {
        kb_buffer[kb_tail] = c;
        kb_tail = next;
    }
}

static void handle_key_repeat() {
    if (repeat_key == 0) return;
    uint64_t now = timer_get_ticks();
    if (now - repeat_start < REPEAT_DELAY) return;
    if (now - repeat_last >= REPEAT_RATE) {
        char c = repeat_shift ? hid_to_ascii_shift[repeat_key] : hid_to_ascii[repeat_key];
        if (c) kb_push(c);
        repeat_last = now;
    }
}

static void process_keyboard_report(HidKeyboardReport* report) {
    bool shift = (report->modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    bool ctrl = (report->modifiers & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) != 0;
    
    uint8_t current_key = 0;
    for (int i = 0; i < 6; i++) {
        if (report->keys[i] != 0 && report->keys[i] < 128) {
            current_key = report->keys[i];
            break;
        }
    }
    
    for (int i = 0; i < 6; i++) {
        uint8_t key = report->keys[i];
        if (key == 0 || key >= 128) continue;
        if (key_was_pressed(key)) continue;
        
        if (shift && key == 0x50) { kb_push(KEY_SHIFT_LEFT); continue; }
        if (shift && key == 0x4F) { kb_push(KEY_SHIFT_RIGHT); continue; }
        
        char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
        
        if (ctrl && c != 0) {
            if (c >= 'a' && c <= 'z') { kb_push(c - 'a' + 1); continue; }
            if (c >= 'A' && c <= 'Z') { kb_push(c - 'A' + 1); continue; }
            if (c == '[' || c == '{') { kb_push(27); continue; }
        }
        
        if (c) kb_push(c);
        repeat_key = key;
        repeat_shift = shift;
        repeat_start = timer_get_ticks();
        repeat_last = repeat_start;
    }
    
    if (current_key == 0) repeat_key = 0;
    last_kbd_report = *report;
}

static void process_mouse_report(uint8_t* data, uint16_t length) {
    if (length < 3) return;
    
    uint8_t btn = 0;
    int8_t dx = 0, dy = 0, wheel = 0;
    bool has_report_id = (length >= 5) && (data[0] == 1 || data[0] == 2);
    
    if (has_report_id) {
        btn = data[1];
        dx = (int8_t)data[2];
        dy = (int8_t)data[3];
        if (length >= 5) wheel = (int8_t)data[4];
    } else {
        btn = data[0];
        dx = (int8_t)data[1];
        dy = (int8_t)data[2];
        if (length >= 4) wheel = (int8_t)data[3];
    }
    
    mouse_left = (btn & HID_MOUSE_LEFT) != 0;
    mouse_right = (btn & HID_MOUSE_RIGHT) != 0;
    mouse_middle = (btn & HID_MOUSE_MIDDLE) != 0;
    mouse_scroll += wheel;
    mouse_x += dx;
    mouse_y += dy;
    
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_width) mouse_x = screen_width - 1;
    if (mouse_y >= screen_height) mouse_y = screen_height - 1;
    
    mouse_available = true;
    mouse_data_received = true;
}

static bool hid_set_protocol(UsbDeviceInfo* dev, uint8_t iface, uint8_t protocol) {
    uint16_t transferred;
    return xhci_control_transfer(dev->slot_id, 0x21, HID_REQ_SET_PROTOCOL,
        protocol, iface, 0, nullptr, &transferred);
}

static bool hid_set_idle(UsbDeviceInfo* dev, uint8_t iface, uint8_t duration) {
    uint16_t transferred;
    return xhci_control_transfer(dev->slot_id, 0x21, HID_REQ_SET_IDLE,
        (uint16_t)duration << 8, iface, 0, nullptr, &transferred);
}

void usb_hid_init() {
    int count = usb_get_device_count();
    DEBUG_INFO("HID init: %d USB devices", count);
    
    for (int i = 0; i < count; i++) {
        UsbDeviceInfo* dev = usb_get_device(i);
        if (!dev || !dev->configured) continue;
        
        if (dev->is_keyboard && dev->hid_endpoint != 0) {
            keyboard_available = true;
            keyboard_device = dev;
            if (dev->is_boot_interface) hid_set_protocol(dev, dev->hid_interface, HID_PROTOCOL_BOOT);
            hid_set_idle(dev, dev->hid_interface, 25);
            DEBUG_INFO("Keyboard ready: Slot=%d EP=%d", dev->slot_id, dev->hid_endpoint);
        }
        
        if (dev->is_mouse) {
            mouse_available = true;
            mouse_device = dev;
            uint8_t mouse_iface = dev->hid_interface2 ? dev->hid_interface2 : dev->hid_interface;
            hid_set_idle(dev, mouse_iface, 0);
            mouse_x = screen_width / 2;
            mouse_y = screen_height / 2;
            DEBUG_INFO("Mouse ready: Slot=%d EP=%d", dev->slot_id,
                      dev->hid_endpoint2 ? dev->hid_endpoint2 : dev->hid_endpoint);
        }
    }
    
    DEBUG_INFO("HID: Keyboard=%s Mouse=%s",
               keyboard_available ? "YES" : "NO", mouse_available ? "YES" : "NO");
}

void usb_hid_poll() {
    int count = usb_get_device_count();
    if (count <= 0) return;
    
    for (int i = 0; i < count; i++) {
        UsbDeviceInfo* dev = usb_get_device(i);
        if (!dev || !dev->configured || dev->slot_id == 0) continue;
        
        if (dev->is_keyboard && dev->hid_endpoint != 0) {
            uint8_t buffer[64];
            uint16_t transferred = 0;
            if (xhci_interrupt_transfer(dev->slot_id, dev->hid_endpoint,
                                        buffer, sizeof(HidKeyboardReport), &transferred)) {
                if (transferred == 8) process_keyboard_report((HidKeyboardReport*)buffer);
            }
        }
        
        uint8_t mouse_ep = 0;
        if (dev->is_mouse) {
            if (dev->hid_endpoint2 != 0) mouse_ep = dev->hid_endpoint2;
            else if (!dev->is_keyboard) mouse_ep = dev->hid_endpoint;
        }
        
        if (mouse_ep != 0) {
            uint8_t buffer[64];
            uint16_t transferred = 0;
            if (xhci_interrupt_transfer(dev->slot_id, mouse_ep, buffer, 64, &transferred)) {
                if (transferred >= 3) process_mouse_report(buffer, transferred);
            }
        }
    }
    handle_key_repeat();
}

bool usb_hid_keyboard_available() { return keyboard_available; }
bool usb_hid_keyboard_has_char() { return kb_head != kb_tail; }

char usb_hid_keyboard_get_char() {
    if (kb_head == kb_tail) return 0;
    char c = kb_buffer[kb_head];
    kb_head = (kb_head + 1) % KB_BUFFER_SIZE;
    return c;
}

bool usb_hid_mouse_available() { return mouse_available && mouse_data_received; }

void usb_hid_mouse_get_state(int32_t* x, int32_t* y, bool* left, bool* right, bool* middle) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (left) *left = mouse_left;
    if (right) *right = mouse_right;
    if (middle) *middle = mouse_middle;
}

int8_t usb_hid_mouse_get_scroll() {
    int8_t delta = mouse_scroll;
    mouse_scroll = 0;
    return delta;
}

void usb_hid_set_screen_size(int32_t width, int32_t height) {
    screen_width = width;
    screen_height = height;
    mouse_x = width / 2;
    mouse_y = height / 2;
}

void usb_hid_set_debug(bool enabled) { (void)enabled; }
