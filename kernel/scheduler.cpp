#include "scheduler.h"
#include "process.h"
#include "heap.h"
#include "pmm.h"
#include <stddef.h>

static Process* current_process = nullptr;
static Process* process_list = nullptr;
static uint64_t next_pid = 1;

void scheduler_init() {
    // Create a process struct for the current running kernel thread (idle task)
    current_process = (Process*)malloc(sizeof(Process));
    current_process->pid = 0;
    current_process->state = PROCESS_RUNNING;
    current_process->next = current_process; // Circular list
    process_list = current_process;
}

void scheduler_create_task(void (*entry)()) {
    Process* new_process = (Process*)malloc(sizeof(Process));
    new_process->pid = next_pid++;
    new_process->state = PROCESS_READY;
    
    // Allocate stack (4KB)
    // We use malloc for simplicity, but ideally we should use PMM/VMM for guard pages
    new_process->stack_base = (uint64_t*)malloc(4096);
    uint64_t* stack_top = (uint64_t*)((uint8_t*)new_process->stack_base + 4096);
    
    // Set up initial stack for switch_to_task
    // We need to emulate what switch_to_task expects to pop
    
    // 1. Return address (RIP) - this is where switch_to_task will "return" to
    stack_top--;
    *stack_top = (uint64_t)entry;
    
    // 2. Callee-saved registers (R15, R14, R13, R12, RBP, RBX)
    // Initialize to 0
    for (int i = 0; i < 6; i++) {
        stack_top--;
        *stack_top = 0;
    }
    
    new_process->sp = (uint64_t)stack_top;
    
    // Add to list (simple circular linked list)
    Process* last = process_list;
    while (last->next != process_list) {
        last = last->next;
    }
    last->next = new_process;
    new_process->next = process_list;
}

void scheduler_schedule() {
    if (!current_process) return;
    
    Process* next = current_process->next;
    
    // Simple Round Robin: just pick the next one
    // In a real scheduler, we'd loop to find a READY process
    
    if (next == current_process) return; // Only one task
    
    Process* prev = current_process;
    current_process = next;
    current_process->state = PROCESS_RUNNING;
    
    switch_to_task(prev, current_process);
}

void scheduler_yield() {
    scheduler_schedule();
}
