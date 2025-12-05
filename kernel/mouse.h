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

void mouse_init();
void mouse_handler();
const MouseState* mouse_get_state();
