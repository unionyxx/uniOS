#pragma once
#include <stdint.h>

enum ProcessState {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE,     // Exited, waiting for parent to collect
    PROCESS_WAITING     // Waiting for child to exit
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
    uint64_t parent_pid;      // Parent process ID
    uint64_t sp;              // Stack Pointer
    uint64_t* stack_base;     // For freeing
    uint64_t* page_table;     // Process page table (for user processes)
    ProcessState state;
    int32_t exit_status;      // Exit code when ZOMBIE
    uint64_t wait_for_pid;    // PID to wait for (0 = any child)
    Process* next;
};

extern "C" void switch_to_task(Process* current, Process* next);

// New process management functions
Process* process_get_current();
Process* process_find_by_pid(uint64_t pid);
uint64_t process_fork();
void process_exit(int32_t status);
int64_t process_waitpid(int64_t pid, int32_t* status);
