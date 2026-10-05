#pragma once
#include <stdint.h>

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND 0x43

// ACPI PM timer rate (fixed 3.579545 MHz crystal). Shared by the TSC
// calibration fallback here and timekeeping's per-CPU offset calibration.
constexpr uint32_t TIMER_PM_TIMER_HZ = 3579545u;

void timer_init(uint32_t frequency);
void timer_set_frequency(uint32_t frequency);
uint64_t timer_get_ticks();
uint32_t timer_get_frequency();
uint32_t timer_handler();
void timer_poll_wait_ms(uint32_t ms);
void sleep(uint32_t ms);

uint64_t timer_ms_to_ticks(uint64_t ms);
void timer_tsc_calibrate();
uint64_t timer_tsc_freq_hz();
uint64_t timer_now_us();
void udelay(uint32_t us);

// Scaled division that stays inside 64 bits (whole units first so the
// remainder product cannot overflow); see the implementation in timer.cpp.
uint64_t scale_u64(uint64_t value, uint64_t numer, uint64_t denom);
