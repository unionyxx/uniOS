#pragma once
#include <stdint.h>

enum ProcessState {
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_SLEEPING,   // Sleeping until wake_time
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

// FPU/SSE state size for fxsave/fxrstor (512 bytes, must be 16-byte aligned)
#define FPU_STATE_SIZE 512

struct Process {
    // FPU state MUST be first and 16-byte aligned for fxsave/fxrstor
    // The Process struct itself will be allocated with 16-byte alignment
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    
    // Offset 512: Process metadata
    uint64_t pid;
    uint64_t parent_pid;      // Parent process ID
    uint64_t sp;              // Stack Pointer (offset 528 = 512 + 16)
    uint64_t* stack_base;     // Virtual address of stack (KERNEL_STACK_TOP - SIZE)
    uint64_t stack_phys;      // Physical address of stack (for freeing)
    uint64_t* page_table;     // Process page table (PML4 virtual address)
    ProcessState state;
    int32_t exit_status;      // Exit code when ZOMBIE
    uint64_t wait_for_pid;    // PID to wait for (0 = any child)
    uint64_t wake_time;       // Timer tick when process should wake (for SLEEPING)
    bool fpu_initialized;     // Whether FPU state has been initialized
    Process* next;
};

extern "C" void switch_to_task(Process* current, Process* next);

// New process management functions
Process* process_get_current();
Process* process_find_by_pid(uint64_t pid);
uint64_t process_fork();
void process_exit(int32_t status);
int64_t process_waitpid(int64_t pid, int32_t* status);
