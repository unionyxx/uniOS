#include "input.h"
#include "ps2_keyboard.h"
#include "ps2_mouse.h"
#include "usb.h"
#include "usb_hid.h"
#include "xhci.h"
#include "graphics.h"

// =============================================================================
// Unified Input Subsystem Implementation
// =============================================================================

// Debug flag for verbose logging
static bool input_debug = false;

// Screen dimensions for mouse clamping
static int32_t screen_width = 1024;
static int32_t screen_height = 768;

// Scroll wheel accumulator (USB mice report delta per poll)
static int8_t scroll_accumulator = 0;

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

void input_init() {
    // Initialize PS/2 keyboard (already done in kernel, but safe to call)
    ps2_keyboard_init();
    
    // Initialize PS/2 mouse
    ps2_mouse_init();
    
    // Initialize USB subsystem
    usb_init();
    
    // Initialize USB HID layer
    usb_hid_init();
    
    // Set screen size for USB mouse
    usb_hid_set_screen_size(screen_width, screen_height);
}

// -----------------------------------------------------------------------------
// Polling
// -----------------------------------------------------------------------------

void input_poll() {
    // Poll xHCI controller for events (USB transfers, port changes)
    if (xhci_is_initialized()) {
        xhci_poll_events();
    }
    
    // Poll USB HID layer (keyboard/mouse reports)
    usb_hid_poll();
    
    // Note: PS/2 keyboard/mouse are interrupt-driven, no polling needed
}

// -----------------------------------------------------------------------------
// Keyboard
// -----------------------------------------------------------------------------

bool input_keyboard_available() {
    return usb_hid_keyboard_available() || true; // PS/2 keyboard always "available"
}

bool input_keyboard_has_char() {
    // Check USB keyboard first (faster on modern systems)
    if (usb_hid_keyboard_has_char()) {
        return true;
    }
    // Fall back to PS/2 keyboard
    return ps2_keyboard_has_char();
}

char input_keyboard_get_char() {
    // Prefer USB keyboard input
    if (usb_hid_keyboard_has_char()) {
        return usb_hid_keyboard_get_char();
    }
    // Fall back to PS/2 keyboard
    return ps2_keyboard_get_char();
}

// -----------------------------------------------------------------------------
// Mouse
// -----------------------------------------------------------------------------

bool input_mouse_available() {
    return usb_hid_mouse_available() || true; // PS/2 mouse always "available"
}

void input_mouse_get_state(InputMouseState* state) {
    if (!state) return;
    
    // Clear scroll delta
    state->scroll_delta = 0;
    
    // Get PS/2 mouse state first (more reliable in QEMU)
    const MouseState* ps2_mouse = ps2_mouse_get_state();
    state->x = ps2_mouse->x;
    state->y = ps2_mouse->y;
    state->left = ps2_mouse->left_button;
    state->right = ps2_mouse->right_button;
    state->middle = ps2_mouse->middle_button;
    
    // If USB mouse is available AND has data, use it instead
    if (usb_hid_mouse_available()) {
        int32_t usb_x, usb_y;
        bool left, right, middle;
        usb_hid_mouse_get_state(&usb_x, &usb_y, &left, &right, &middle);
        
        // Always use USB mouse when available (don't require moved from origin)
        state->x = usb_x;
        state->y = usb_y;
        state->left = left;
        state->right = right;
        state->middle = middle;
        state->scroll_delta = usb_hid_mouse_get_scroll();
    }
}

void input_set_screen_size(int32_t width, int32_t height) {
    screen_width = width;
    screen_height = height;
    usb_hid_set_screen_size(width, height);
}

// -----------------------------------------------------------------------------
// Debug
// -----------------------------------------------------------------------------

void input_set_debug(bool enabled) {
    input_debug = enabled;
    // Could also propagate to USB layers if they expose debug flags
}
