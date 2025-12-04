#pragma once
#include <stdint.h>

#define KEYBOARD_DATA_PORT    0x60
#define KEYBOARD_STATUS_PORT  0x64

void keyboard_init();
void keyboard_handler();
char keyboard_get_char();
uint8_t keyboard_has_char();
