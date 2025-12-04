#pragma once
#include <stdint.h>

enum ProcessState {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED
};

struct Context {
    // Callee-saved registers
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    
    // Return address (pushed by call)
    uint64_t rip;
};

struct Process {
    uint64_t pid;
    uint64_t sp; // Stack Pointer
    uint64_t* stack_base; // For freeing
    ProcessState state;
    Process* next;
};

extern "C" void switch_to_task(Process* current, Process* next);
