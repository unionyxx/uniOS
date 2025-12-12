#pragma once
#include <stdint.h>

// Mouse state
struct MouseState {
    int32_t x;
    int32_t y;
    bool left_button;
    bool right_button;
    bool middle_button;
};

void ps2_mouse_init();
void ps2_mouse_handler();
const MouseState* ps2_mouse_get_state();
