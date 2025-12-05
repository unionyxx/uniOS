#include "scheduler.h"
#include "process.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include <stddef.h>

static Process* current_process = nullptr;
static Process* process_list = nullptr;
static uint64_t next_pid = 1;

Process* process_get_current() {
    return current_process;
}

Process* process_find_by_pid(uint64_t pid) {
    Process* p = process_list;
    if (!p) return nullptr;
    
    do {
        if (p->pid == pid) return p;
        p = p->next;
    } while (p != process_list);
    
    return nullptr;
}

void scheduler_init() {
    // Create a process struct for the current running kernel thread (idle task)
    current_process = (Process*)malloc(sizeof(Process));
    current_process->pid = 0;
    current_process->parent_pid = 0;
    current_process->state = PROCESS_RUNNING;
    current_process->exit_status = 0;
    current_process->wait_for_pid = 0;
    current_process->page_table = vmm_get_kernel_pml4();
    current_process->next = current_process; // Circular list
    process_list = current_process;
}

void scheduler_create_task(void (*entry)()) {
    Process* new_process = (Process*)malloc(sizeof(Process));
    new_process->pid = next_pid++;
    new_process->parent_pid = current_process ? current_process->pid : 0;
    new_process->state = PROCESS_READY;
    new_process->exit_status = 0;
    new_process->wait_for_pid = 0;
    new_process->page_table = vmm_get_kernel_pml4();
    
    // Allocate stack (4KB)
    new_process->stack_base = (uint64_t*)malloc(4096);
    if (!new_process->stack_base) {
        free(new_process);
        return; 
    }
    
    // Align stack top to 16 bytes
    uint64_t stack_addr = (uint64_t)new_process->stack_base + 4096;
    stack_addr &= ~0xF; 
    uint64_t* stack_top = (uint64_t*)stack_addr;
    
    // Set up initial stack for switch_to_task
    stack_top--;
    *stack_top = 0; // Dummy return address

    stack_top--;
    *stack_top = (uint64_t)entry; // Return address (RIP)
    
    stack_top--;
    *stack_top = 0x202; // RFLAGS (Interrupts Enabled)

    // Callee-saved registers (R15, R14, R13, R12, RBP, RBX)
    for (int i = 0; i < 6; i++) {
        stack_top--;
        *stack_top = 0;
    }
    
    new_process->sp = (uint64_t)stack_top;
    
    // Add to circular list
    Process* last = process_list;
    while (last->next != process_list) {
        last = last->next;
    }
    last->next = new_process;
    new_process->next = process_list;
}

void scheduler_schedule() {
    if (!current_process) return;
    
    // Find next runnable process
    Process* next = current_process->next;
    Process* start = next;
    
    do {
        if (next->state == PROCESS_READY || next->state == PROCESS_RUNNING) {
            break;
        }
        next = next->next;
    } while (next != start);
    
    if (next == current_process) return; // Only one runnable task
    if (next->state != PROCESS_READY && next->state != PROCESS_RUNNING) return;
    
    Process* prev = current_process;
    if (prev->state == PROCESS_RUNNING) {
        prev->state = PROCESS_READY;
    }
    
    current_process = next;
    current_process->state = PROCESS_RUNNING;
    
    switch_to_task(prev, current_process);
}

void scheduler_yield() {
    scheduler_schedule();
}

// Fork: Create a copy of current process
uint64_t process_fork() {
    Process* parent = current_process;
    Process* child = (Process*)malloc(sizeof(Process));
    if (!child) return (uint64_t)-1;
    
    child->pid = next_pid++;
    child->parent_pid = parent->pid;
    child->state = PROCESS_READY;
    child->exit_status = 0;
    child->wait_for_pid = 0;
    
    // Allocate new stack for child
    child->stack_base = (uint64_t*)malloc(4096);
    if (!child->stack_base) {
        free(child);
        return (uint64_t)-1;
    }
    
    // Copy parent's stack
    for (int i = 0; i < 512; i++) {
        child->stack_base[i] = parent->stack_base[i];
    }
    
    // Adjust child's SP relative to new stack
    uint64_t stack_offset = parent->sp - (uint64_t)parent->stack_base;
    child->sp = (uint64_t)child->stack_base + stack_offset;
    
    // Share page table for now (proper fork would copy)
    child->page_table = parent->page_table;
    
    // Add child to process list
    Process* last = process_list;
    while (last->next != process_list) {
        last = last->next;
    }
    last->next = child;
    child->next = process_list;
    
    return child->pid;
}

// Exit: Terminate current process
void process_exit(int32_t status) {
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_status = status;
    
    // Wake up parent if waiting
    Process* parent = process_find_by_pid(current_process->parent_pid);
    if (parent && parent->state == PROCESS_WAITING) {
        if (parent->wait_for_pid == 0 || parent->wait_for_pid == current_process->pid) {
            parent->state = PROCESS_READY;
        }
    }
    
    // Schedule another process
    scheduler_schedule();
    
    // Should never reach here
    for(;;);
}

// Waitpid: Wait for child to exit
int64_t process_waitpid(int64_t pid, int32_t* status) {
    while (true) {
        // Look for zombie child
        Process* p = process_list;
        do {
            if (p->parent_pid == current_process->pid && p->state == PROCESS_ZOMBIE) {
                if (pid == -1 || (uint64_t)pid == p->pid) {
                    // Found matching zombie child
                    if (status) *status = p->exit_status;
                    uint64_t child_pid = p->pid;
                    
                    // Remove from process list (simple version)
                    // In real OS, we'd properly unlink from list
                    p->state = PROCESS_BLOCKED; // Mark as cleaned up
                    
                    return child_pid;
                }
            }
            p = p->next;
        } while (p != process_list);
        
        // No zombie found, block and wait
        current_process->state = PROCESS_WAITING;
        current_process->wait_for_pid = (pid == -1) ? 0 : pid;
        scheduler_schedule();
    }
}
