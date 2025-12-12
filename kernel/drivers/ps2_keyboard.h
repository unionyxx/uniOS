#pragma once
#include <stdint.h>

#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

void ps2_keyboard_init();
void ps2_keyboard_handler();
char ps2_keyboard_get_char();
uint8_t ps2_keyboard_has_char();
