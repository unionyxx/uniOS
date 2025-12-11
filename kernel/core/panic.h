#pragma once
#include <stdint.h>

// Halt and Catch Fire - infinite loop with interrupts disabled
void hcf(void);

// Kernel panic - halts system with error message
void panic(const char* message);

// Exception handler entry point (called from assembly)
extern "C" void exception_handler(void* stack_frame);
