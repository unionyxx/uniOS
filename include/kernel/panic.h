#pragma once
#include <stdint.h>

// Halt and Catch Fire - infinite loop with interrupts disabled
void hcf(void);

// Kernel panic - halts system with error message
void panic(const char* message);

// Exception handler entry point (called from assembly)
struct InterruptFrame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

extern "C" void exception_handler(InterruptFrame* frame);
