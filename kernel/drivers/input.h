#pragma once
#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Unified Input Subsystem for uniOS
// =============================================================================
// Provides a single API for keyboard and mouse input, abstracting away the
// underlying transport (USB HID or PS/2). The kernel should use these
// functions instead of directly calling USB or PS/2 layer functions.

// -----------------------------------------------------------------------------
// Initialization and Polling
// -----------------------------------------------------------------------------

// Initialize the input subsystem (USB + PS/2)
void input_init();

// Poll all input sources - call this in the main kernel loop
void input_poll();

// -----------------------------------------------------------------------------
// Keyboard API
// -----------------------------------------------------------------------------

// Check if a character is available in the keyboard buffer
bool input_keyboard_has_char();

// Get the next character from the keyboard buffer (0 if empty)
char input_keyboard_get_char();

// Check if any keyboard (USB or PS/2) is available
bool input_keyboard_available();

// -----------------------------------------------------------------------------
// Mouse API
// -----------------------------------------------------------------------------

// Mouse state structure
struct InputMouseState {
    int32_t x;
    int32_t y;
    bool left;
    bool right;
    bool middle;
    int8_t scroll_delta;  // Scroll wheel delta since last poll
};

// Check if any mouse (USB or PS/2) is available
bool input_mouse_available();

// Get the current mouse state
void input_mouse_get_state(InputMouseState* state);

// Set the screen size for mouse bounds clamping
void input_set_screen_size(int32_t width, int32_t height);

// -----------------------------------------------------------------------------
// Debug Control
// -----------------------------------------------------------------------------

// Enable/disable verbose debug logging in USB/HID layers
void input_set_debug(bool enabled);
