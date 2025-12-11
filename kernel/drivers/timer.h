#pragma once
#include <stdint.h>

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43

void timer_init(uint32_t frequency);
uint64_t timer_get_ticks();
uint32_t timer_get_frequency();
void timer_handler();
void sleep(uint32_t ms);
