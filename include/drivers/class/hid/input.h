#pragma once
#include <stdbool.h>
#include <stdint.h>

void input_init();

void input_poll();
bool input_keyboard_has_char();
char input_keyboard_get_char();
bool input_keyboard_available();
struct InputMouseState
{
    int32_t x;
    int32_t y;
    bool left;
    bool right;
    bool middle;
    int8_t scroll_delta;
};

bool input_mouse_available();
void input_mouse_get_state(InputMouseState *state);
void input_set_screen_size(int32_t width, int32_t height);
void input_set_debug(bool enabled);
