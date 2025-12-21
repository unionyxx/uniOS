#include "usb_hid.h"
#include "usb.h"
#include "xhci.h"
#include "io.h"
#include "timer.h"
#include <stddef.h>

// Keyboard state
static bool keyboard_available = false;
static UsbDeviceInfo* keyboard_device = nullptr;
static HidKeyboardReport last_keyboard_report = {0};

// Keyboard buffer
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint8_t kb_buffer_start = 0;
static volatile uint8_t kb_buffer_end = 0;

// Mouse state
static bool mouse_available = false;
static bool mouse_data_received = false;  // True only if USB mouse has sent data
static UsbDeviceInfo* mouse_device = nullptr;
static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static bool mouse_left = false;
static bool mouse_right = false;
static bool mouse_middle = false;
static int8_t mouse_scroll = 0;  // Scroll wheel delta accumulator

// Screen dimensions (set during init)
static int32_t screen_width = 1024;
static int32_t screen_height = 768;

// Polling interval tracking - per USB spec, must wait for interval before polling again
static uint64_t last_keyboard_poll = 0;
static uint64_t last_mouse_poll = 0;

// Debug flag - controls verbose logging
static bool hid_debug = false;

// HID keycode to ASCII conversion table (US keyboard layout)
// Index = HID keycode, value = ASCII (lowercase)
// Arrow keys use special codes matching PS/2 keyboard: 0x80=Up, 0x81=Down, 0x82=Left, 0x83=Right
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

static const char hid_to_ascii[128] = {
    0,    0,    0,    0,   'a',  'b',  'c',  'd',   // 0x00-0x07
    'e',  'f',  'g',  'h',  'i',  'j',  'k',  'l',   // 0x08-0x0F
    'm',  'n',  'o',  'p',  'q',  'r',  's',  't',   // 0x10-0x17
    'u',  'v',  'w',  'x',  'y',  'z',  '1',  '2',   // 0x18-0x1F
    '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',   // 0x20-0x27
    '\n', 27,   '\b', '\t', ' ',  '-',  '=',  '[',   // 0x28-0x2F (Enter, Esc, Backspace, Tab, Space)
    ']',  '\\', '#',  ';',  '\'', '`',  ',',  '.',   // 0x30-0x37
    '/',  0,    0,    0,    0,    0,    0,    0,     // 0x38-0x3F (CapsLock, F1-F6)
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x40-0x47 (F7-F12, PrintScreen, ScrollLock)
    0,    0,    (char)0x84, 0, (char)0x86, (char)0x85, 0, (char)0x83,  // 0x48-0x4F (Pause, Insert, Home, PageUp, Delete, End, PageDown, Right)
    (char)0x82, (char)0x81, (char)0x80, 0, '/',  '*',  '-',  '+',   // 0x50-0x57 (Left, Down, Up, NumLock, Keypad...)
    '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7',   // 0x58-0x5F (Keypad Enter, 1-7)
    '8',  '9',  '0',  '.',  0,    0,    0,    '=',   // 0x60-0x67
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x68-0x6F
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x70-0x77
    0,    0,    0,    0,    0,    0,    0,    0      // 0x78-0x7F
};

// Shifted ASCII
static const char hid_to_ascii_shift[128] = {
    0,    0,    0,    0,   'A',  'B',  'C',  'D',   // 0x00-0x07
    'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',   // 0x08-0x0F
    'M',  'N',  'O',  'P',  'Q',  'R',  'S',  'T',   // 0x10-0x17
    'U',  'V',  'W',  'X',  'Y',  'Z',  '!',  '@',   // 0x18-0x1F
    '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',   // 0x20-0x27
    '\n', 27,   '\b', '\t', ' ',  '_',  '+',  '{',   // 0x28-0x2F
    '}',  '|',  '~',  ':',  '"',  '~',  '<',  '>',   // 0x30-0x37
    '?',  0,    0,    0,    0,    0,    0,    0,     // 0x38-0x3F
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x40-0x47
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x48-0x4F
    0,    0,    0,    0,    '/',  '*',  '-',  '+',   // 0x50-0x57
    '\n', '1',  '2',  '3',  '4',  '5',  '6',  '7',   // 0x58-0x5F
    '8',  '9',  '0',  '.',  0,    0,    0,    '=',   // 0x60-0x67
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x68-0x6F
    0,    0,    0,    0,    0,    0,    0,    0,     // 0x70-0x77
    0,    0,    0,    0,    0,    0,    0,    0      // 0x78-0x7F
};

// Check if a key was in the previous report (for detecting new key presses)
static bool key_was_pressed(uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (last_keyboard_report.keys[i] == keycode) {
            return true;
        }
    }
    return false;
}

// Add a character to the keyboard buffer
static void kb_buffer_push(char c) {
    uint8_t next = (kb_buffer_end + 1) % KB_BUFFER_SIZE;
    if (next != kb_buffer_start) {
        kb_buffer[kb_buffer_end] = c;
        kb_buffer_end = next;
    }
}

// Key repeat state - timer-based (like PS2 keyboards)
static uint8_t repeat_keycode = 0;
static uint64_t repeat_start_tick = 0;      // When key was first pressed
static uint64_t repeat_last_tick = 0;       // When last repeat occurred
static bool repeat_shift = false;
static const uint32_t REPEAT_DELAY_TICKS = 500;  // 500ms initial delay (1000Hz timer)
static const uint32_t REPEAT_RATE_TICKS = 33;    // ~30 chars per second (33ms between repeats)

// Handle key repeat - called every poll, uses real timer ticks
static void handle_key_repeat() {
    if (repeat_keycode == 0) return;
    
    uint64_t now = timer_get_ticks();
    uint64_t elapsed = now - repeat_start_tick;
    
    // Check if past initial delay
    if (elapsed >= REPEAT_DELAY_TICKS) {
        // Check if it's time for another repeat
        if (now - repeat_last_tick >= REPEAT_RATE_TICKS) {
            char c;
            if (repeat_shift) {
                c = hid_to_ascii_shift[repeat_keycode];
            } else {
                c = hid_to_ascii[repeat_keycode];
            }
            if (c != 0) {
                kb_buffer_push(c);
            }
            repeat_last_tick = now;
        }
    }
}

// Process a keyboard report - only called when new data arrives
static void process_keyboard_report(HidKeyboardReport* report) {
    bool shift = (report->modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
    bool ctrl = (report->modifiers & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) != 0;
    
    // Find first currently pressed key
    uint8_t current_key = 0;
    for (int i = 0; i < 6; i++) {
        if (report->keys[i] != 0 && report->keys[i] < 128) {
            current_key = report->keys[i];
            break;
        }
    }
    
    // Check all keys for new presses
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keys[i];
        
        if (keycode == 0) continue;
        if (keycode >= 128) continue;
        
        // Only process if this is a new key press
        if (!key_was_pressed(keycode)) {
            // Handle Shift+Arrow for text selection (HID: 0x50=Left, 0x4F=Right)
            if (shift && keycode == 0x50) {  // Shift+Left
                kb_buffer_push(KEY_SHIFT_LEFT);
                continue;
            }
            if (shift && keycode == 0x4F) {  // Shift+Right
                kb_buffer_push(KEY_SHIFT_RIGHT);
                continue;
            }
            
            char c;
            if (shift) {
                c = hid_to_ascii_shift[keycode];
            } else {
                c = hid_to_ascii[keycode];
            }
            
            // Handle Ctrl key combinations - generate control codes
            if (ctrl && c != 0) {
                // Convert letter to control code (a=1, b=2, ..., z=26)
                if (c >= 'a' && c <= 'z') {
                    kb_buffer_push(c - 'a' + 1);
                    // Start repeat for Ctrl combo
                    repeat_keycode = keycode;
                    repeat_shift = shift;
                    repeat_start_tick = timer_get_ticks();
                    repeat_last_tick = repeat_start_tick;
                    continue;
                }
                if (c >= 'A' && c <= 'Z') {
                    kb_buffer_push(c - 'A' + 1);
                    repeat_keycode = keycode;
                    repeat_shift = shift;
                    repeat_start_tick = timer_get_ticks();
                    repeat_last_tick = repeat_start_tick;
                    continue;
                }
                // Special cases
                if (c == '[' || c == '{') { kb_buffer_push(27); continue; }  // Ctrl+[ = Escape
                if (c == '\\' || c == '|') { kb_buffer_push(28); continue; } // Ctrl+\
                if (c == ']' || c == '}') { kb_buffer_push(29); continue; }  // Ctrl+]
            }
            
            if (c != 0) {
                kb_buffer_push(c);
            }
            
            // Start repeat for this key
            repeat_keycode = keycode;
            repeat_shift = shift;
            repeat_start_tick = timer_get_ticks();
            repeat_last_tick = repeat_start_tick;
        }
    }
    
    // Handle key release
    if (current_key == 0) {
        // All keys released - stop repeating
        repeat_keycode = 0;
    } else if (current_key != repeat_keycode && repeat_keycode != 0) {
        // Different key now held - switch to new key
        // Don't start repeat, wait for it to be detected as "new"
    }
    // Note: repeat counter is incremented in handle_key_repeat(), not here
    
    // Save report for next comparison
    last_keyboard_report = *report;
}

// Process a mouse report - supports both 8-bit and 16-bit formats
// Gaming mice often use Report IDs and 16-bit coordinates
static void process_mouse_report(HidMouseReport* report, uint16_t transferred) {
    uint8_t* raw = (uint8_t*)report;
    
    mouse_data_received = true;
    
    uint8_t buttons = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    int8_t dwheel = 0;
    
    // Safety check
    if (transferred < 3) return;

    // --- DETECT FORMAT ---
    // Check for Report ID prefix first
    // Gaming mice often send: [ReportID, Buttons, X_Lo, X_Hi, Y_Lo, Y_Hi, Wheel, ...]
    // Heuristic: If byte 0 is small (ID 1-10) and byte 1 looks like buttons (low bits set, high bits 0)
    // FIX: Use > 4, not >= 4, because standard 4-byte mice [Buttons][X][Y][Wheel] can trigger
    // false positives when clicking while stationary (0x01, 0x00, ...)
    int offset = 0;
    if (transferred > 4 && raw[0] >= 1 && raw[0] <= 10 && (raw[1] & 0xF8) == 0) {
        offset = 1;  // Skip Report ID
    }
    
    // Calculate payload size after offset
    uint16_t payload_size = transferred - offset;
    
    // Determine format based on payload size
    // 16-bit format: [Buttons, X_Lo, X_Hi, Y_Lo, Y_Hi, Wheel?] = 5+ bytes
    bool is_16bit = (payload_size >= 5);

    if (is_16bit) {
        // --- 16-BIT FORMAT (Gaming mice) ---
        buttons = raw[offset + 0];
        
        // Proper 16-bit reconstruction (little-endian)
        dx = (int16_t)(raw[offset + 1] | (raw[offset + 2] << 8));
        dy = (int16_t)(raw[offset + 3] | (raw[offset + 4] << 8));
        
        if (payload_size >= 6) {
            dwheel = (int8_t)raw[offset + 5];
        }
    } 
    else {
        // --- 8-BIT FORMAT (Boot protocol or simple mice) ---
        buttons = raw[offset + 0];
        dx = (int8_t)raw[offset + 1];
        dy = (int8_t)raw[offset + 2];
        
        if (payload_size >= 4) {
            dwheel = (int8_t)raw[offset + 3];
        }
    } 
    
    // Update button states
    mouse_left = (buttons & HID_MOUSE_LEFT) != 0;
    mouse_right = (buttons & HID_MOUSE_RIGHT) != 0;
    mouse_middle = (buttons & HID_MOUSE_MIDDLE) != 0;
    
    // Apply raw movement directly (no divisor - let hardware DPI handle sensitivity)
    mouse_x += dx;
    mouse_y += dy;
    mouse_scroll += dwheel;

    // Clamp to screen bounds
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen_width) mouse_x = screen_width - 1;
    if (mouse_y >= screen_height) mouse_y = screen_height - 1;
}

// Set boot protocol for a HID device
static bool set_boot_protocol(UsbDeviceInfo* dev) {
    uint16_t transferred;
    return xhci_control_transfer(
        dev->slot_id,
        0x21,  // Host-to-device, Class, Interface
        HID_REQ_SET_PROTOCOL,
        HID_PROTOCOL_BOOT,
        dev->hid_interface,
        0,
        nullptr,
        &transferred
    );
}

// Set idle rate (0 = only report on change)
static bool set_idle(UsbDeviceInfo* dev, uint8_t idle_rate) {
    uint16_t transferred;
    return xhci_control_transfer(
        dev->slot_id,
        0x21,  // Host-to-device, Class, Interface
        HID_REQ_SET_IDLE,
        idle_rate << 8,
        dev->hid_interface,
        0,
        nullptr,
        &transferred
    );
}

void usb_hid_init() {
    int count = usb_get_device_count();
    
    // Always log this to help diagnose issues
    DEBUG_INFO("HID Init: %d USB devices", count);
    
    for (int i = 0; i < count; i++) {
        UsbDeviceInfo* dev = usb_get_device(i);
        if (!dev || !dev->configured) continue;
        
        // Log device info for debugging
        DEBUG_LOG("Dev %d: Slot %d KBD=%d MOUSE=%d EP1=%d EP2=%d", 
            i, dev->slot_id, dev->is_keyboard ? 1 : 0, dev->is_mouse ? 1 : 0,
            dev->hid_endpoint, dev->hid_endpoint2);
        
        if (dev->is_keyboard) {
            keyboard_available = true;
            keyboard_device = dev;
            
            // Only send SET_PROTOCOL to Boot Interface devices
            if (dev->is_boot_interface && dev->hid_endpoint != 0) {
                if (set_boot_protocol(dev)) {
                    DEBUG_LOG("Slot %d: Keyboard Boot Proto OK", dev->slot_id);
                } else {
                    DEBUG_ERROR("Slot %d: Keyboard Boot Proto FAIL", dev->slot_id);
                }
            }
            
            // SET_IDLE to 100ms (25 * 4ms) to ensure we get periodic reports
            // This helps recover if a "key up" packet is missed
            if (dev->hid_endpoint != 0) {
                set_idle(dev, 25);
            }
        }
        
        // NOTE: Not using else-if so composite devices (keyboard+mouse) initialize both
        if (dev->is_mouse) {
            mouse_available = true;
            mouse_device = dev;
            
            // Determine which endpoint/interface to use for mouse
            uint8_t mouse_ep = (dev->hid_endpoint2 != 0) ? dev->hid_endpoint2 : dev->hid_endpoint;
            uint8_t mouse_iface = (dev->hid_interface2 != 0) ? dev->hid_interface2 : dev->hid_interface;
            
            DEBUG_LOG("Mouse detected: Slot %d EP %d Iface %d Boot=%d", 
                dev->slot_id, mouse_ep, mouse_iface, dev->is_boot_interface ? 1 : 0);
            
            // FIX: Do NOT force Boot Protocol for mice!
            // Gaming mice (1000Hz, high DPI) behave poorly in Boot Protocol.
            // Boot Protocol clamps values to signed 8-bit (-127 to 127),
            // causing "negative acceleration" or cursor sticking on fast movements.
            // Leave mice in Report Protocol (default) for full precision.
            
            // Only set Idle rate to 0 (report on change only) to prevent keep-alive spam
            if (mouse_ep != 0) {
                uint16_t transferred;
                xhci_control_transfer(
                    dev->slot_id,
                    0x21,  // Host-to-device, Class, Interface
                    HID_REQ_SET_IDLE,
                    0,  // idle_rate = 0 (report only on change)
                    mouse_iface,
                    0,
                    nullptr,
                    &transferred
                );
            }
            
            // Center mouse on screen
            mouse_x = screen_width / 2;
            mouse_y = screen_height / 2;
        }
    }
    
    // Summary log
    DEBUG_INFO("HID: Keyboard=%s Mouse=%s", 
        keyboard_available ? "YES" : "NO", 
        mouse_available ? "YES" : "NO");
}

void usb_hid_poll() {
    int count = usb_get_device_count();
    if (count <= 0) return;
    
    uint64_t now = timer_get_ticks();
    
    for (int i = 0; i < count; i++) {
        UsbDeviceInfo* dev = usb_get_device(i);
        if (!dev || !dev->configured || dev->slot_id == 0) continue;
        
        // CASE 1: True Combo Device (Single endpoint for both keyboard and mouse)
        bool is_single_ep_combo = dev->is_keyboard && dev->is_mouse && dev->hid_endpoint2 == 0;
        
        if (is_single_ep_combo && dev->hid_endpoint != 0) {
            // --- COMBO DEVICE: Poll once, route by size ---
            uint8_t buffer[16];
            uint16_t transferred = 0;
            
            if (xhci_interrupt_transfer(dev->slot_id, dev->hid_endpoint,
                                        buffer, sizeof(buffer), &transferred)) {
                if (transferred >= 3) {
                    // Keyboard = 8 bytes with byte[1] = 0 (reserved)
                    if (transferred == 8 && buffer[1] == 0) {
                        process_keyboard_report((HidKeyboardReport*)buffer);
                    } else {
                        // Mouse = any other size
                        process_mouse_report((HidMouseReport*)buffer, transferred);
                    }
                }
            }
            last_keyboard_poll = now;
            last_mouse_poll = now;
            continue; // Skip the rest for this device
        }

        // CASE 2, 3, & 4: Separate Endpoints (Keyboard-only, Mouse-only, or Composite-Separate)
        // We poll endpoints independently if they exist.

        // --- Poll Keyboard Endpoint ---
        // Condition: Is a keyboard AND has a valid endpoint
        if (dev->is_keyboard && dev->hid_endpoint != 0) {
            uint64_t keyboard_interval = dev->hid_interval;
            if (keyboard_interval < 1) keyboard_interval = 10;
            // 1000Hz timer: 1 tick = 1ms, interval is already in ms
            uint64_t keyboard_ticks = keyboard_interval;
            
            if (now - last_keyboard_poll >= keyboard_ticks) {
                HidKeyboardReport report;
                uint16_t transferred = 0;
                
                if (xhci_interrupt_transfer(dev->slot_id, dev->hid_endpoint, 
                                            &report, sizeof(report), &transferred)) {
                    if (transferred >= 3) {
                        process_keyboard_report(&report);
                    }
                }
                // Update poll time regardless of transfer success (prevents polling storm)
                last_keyboard_poll = now;
            }
        }

        // --- Poll Mouse Endpoint ---
        // Condition: Is a mouse AND has a valid endpoint.
        // For composite devices, the mouse is on endpoint2.
        // For mouse-only devices, it's on endpoint (primary).
        uint8_t mouse_ep = 0;
        if (dev->is_mouse) {
            if (dev->hid_endpoint2 != 0) {
                mouse_ep = dev->hid_endpoint2; // Composite separate
            } else if (!dev->is_keyboard) {
                mouse_ep = dev->hid_endpoint;  // Mouse only
            }
        }

        if (mouse_ep != 0) {
            // Use device's max packet size for proper buffer sizing
            uint16_t max_packet = (dev->hid_max_packet2 != 0) ? dev->hid_max_packet2 : dev->hid_max_packet;
            if (max_packet == 0 || max_packet > 64) max_packet = 16;  // Clamp to reasonable size
            
            uint8_t report_buffer[64];
            uint16_t transferred = 0;
            
            // IMPORTANT: Use while loop to drain ALL pending mouse packets!
            // Gaming mice at 1000Hz can queue multiple packets between polls.
            // Using 'if' would only get one packet, dropping the rest.
            // xhci_interrupt_transfer returns true if data is ready, false if not.
            while (xhci_interrupt_transfer(dev->slot_id, mouse_ep,
                                           report_buffer, max_packet, &transferred)) {
                if (transferred >= 3) {
                    if (!mouse_data_received) {
                        DEBUG_INFO("USB Mouse: First data received!");
                    }
                    process_mouse_report((HidMouseReport*)report_buffer, transferred);
                }
            }
            // NOTE: No last_mouse_poll update - we run free, letting hardware schedule
        }
    }
    
    // Handle key repeat every poll cycle (independent of new reports)
    handle_key_repeat();
}

bool usb_hid_keyboard_available() {
    return keyboard_available;
}

bool usb_hid_keyboard_has_char() {
    return kb_buffer_start != kb_buffer_end;
}

char usb_hid_keyboard_get_char() {
    if (kb_buffer_start == kb_buffer_end) {
        return 0;
    }
    char c = kb_buffer[kb_buffer_start];
    kb_buffer_start = (kb_buffer_start + 1) % KB_BUFFER_SIZE;
    return c;
}

bool usb_hid_mouse_available() {
    // Only report available if we've actually received mouse data
    // This allows PS/2 mouse to work as fallback when USB mouse doesn't respond
    return mouse_available && mouse_data_received;
}

void usb_hid_mouse_get_state(int32_t* x, int32_t* y, bool* left, bool* right, bool* middle) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    if (left) *left = mouse_left;
    if (right) *right = mouse_right;
    if (middle) *middle = mouse_middle;
}

void usb_hid_set_screen_size(int32_t width, int32_t height) {
    screen_width = width;
    screen_height = height;
    // Center mouse on screen when dimensions are set
    mouse_x = width / 2;
    mouse_y = height / 2;
}

int8_t usb_hid_mouse_get_scroll() {
    int8_t delta = mouse_scroll;
    mouse_scroll = 0;  // Clear after reading
    return delta;
}

void usb_hid_set_debug(bool enabled) {
    hid_debug = enabled;
}
