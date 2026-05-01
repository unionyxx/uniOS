#pragma once

#include <stdint.h>

void boot_splash_init(void);
void boot_splash_set_progress(uint32_t percent);
bool boot_splash_is_active(void);
