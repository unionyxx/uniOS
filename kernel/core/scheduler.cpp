#include "scheduler.h"
#include "process.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"  // For VMM isolation
#include "debug.h"
#include "spinlock.h"
#include "timer.h"
#include "gdt.h"  // For tss_set_rsp0
#include <stddef.h>
#include <string.h>  // For memset if available, else we'll do it manually

// External assembly function to initialize FPU state
extern "C" void init_fpu_state(uint8_t* fpu_buffer);

// Scheduler lock for thread safety
static Spinlock scheduler_lock = SPINLOCK_INIT;

// KERNEL_STACK_SIZE and KERNEL_STACK_TOP are now defined in vmm.h

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
    DEBUG_INFO("Initializing Scheduler...\n");
    
    // Create a process struct for the current running kernel thread (idle task)
    // Use aligned_alloc to ensure FPU state is 16-byte aligned for fxsave/fxrstor
    current_process = (Process*)aligned_alloc(16, sizeof(Process));
    if (!current_process) {
        panic("Failed to allocate initial process!");
    }
    
    // Zero the entire struct first
    uint8_t* p = (uint8_t*)current_process;
    for (size_t i = 0; i < sizeof(Process); i++) p[i] = 0;
    
    // Allocate a real stack for the idle task
    // This is critical for rsp0 updates - without it, when switching back to
    // the idle task, rsp0 wouldn't be updated, which could cause crashes
    current_process->stack_base = (uint64_t*)malloc(KERNEL_STACK_SIZE);
    if (!current_process->stack_base) {
        panic("Failed to allocate idle task stack!");
    }
    
    current_process->pid = 0;
    current_process->parent_pid = 0;
    current_process->sp = 0;  // Not used - idle task continues on current stack
    current_process->stack_phys = 0;  // Heap-allocated, not PMM
    current_process->page_table = nullptr; // Kernel tasks share kernel page table
    current_process->state = PROCESS_RUNNING;
    current_process->exit_status = 0;
    current_process->wait_for_pid = 0;
    current_process->next = current_process; // Circular list
    
    // Initialize FPU state for idle task
    init_fpu_state(current_process->fpu_state);
    current_process->fpu_initialized = true;
    
    process_list = current_process;
    
    DEBUG_INFO("Scheduler Initialized. Initial PID: 0\n");
}

void scheduler_create_task(void (*entry)()) {
    // Use aligned_alloc to ensure FPU state is 16-byte aligned for fxsave/fxrstor
    Process* new_process = (Process*)aligned_alloc(16, sizeof(Process));
    if (!new_process) {
        DEBUG_ERROR("Failed to allocate process struct\n");
        return;
    }
    
    // Zero the entire struct first
    uint8_t* p = (uint8_t*)new_process;
    for (size_t i = 0; i < sizeof(Process); i++) p[i] = 0;
    
    new_process->pid = next_pid++;
    new_process->parent_pid = current_process ? current_process->pid : 0;
    new_process->state = PROCESS_READY;
    new_process->exit_status = 0;
    new_process->wait_for_pid = 0;
    new_process->page_table = nullptr;  // Kernel task - no VMM isolation
    new_process->stack_phys = 0;        // Kernel task - stack is heap-allocated
    
    // Initialize FPU state for the new task
    init_fpu_state(new_process->fpu_state);
    new_process->fpu_initialized = true;
    
    // Allocate stack (16KB for deep call chains like networking)
    new_process->stack_base = (uint64_t*)malloc(KERNEL_STACK_SIZE);
    if (!new_process->stack_base) {
        DEBUG_ERROR("Failed to allocate stack for PID %d\n", new_process->pid);
        aligned_free(new_process);  // Must use aligned_free, not free!
        return; 
    }
    
    // Align stack top to 16 bytes
    uint64_t stack_addr = (uint64_t)new_process->stack_base + KERNEL_STACK_SIZE;
    stack_addr &= ~0xF; 
    uint64_t* stack_top = (uint64_t*)stack_addr;
    
    // Set up initial stack for switch_to_task
    stack_top--; *stack_top = 0; // Dummy return
    stack_top--; *stack_top = (uint64_t)entry; // RIP
    stack_top--; *stack_top = 0x202; // RFLAGS
    
    // Callee-saved regs
    for (int i = 0; i < 6; i++) {
        stack_top--; *stack_top = 0;
    }
    
    new_process->sp = (uint64_t)stack_top;
    
    // Add to list (protected by scheduler lock)
    spinlock_acquire(&scheduler_lock);
    Process* last = process_list;
    while (last->next != process_list) {
        last = last->next;
    }
    last->next = new_process;
    new_process->next = process_list;
    spinlock_release(&scheduler_lock);
    
    DEBUG_INFO("Created Task PID: %d\n", new_process->pid);
}

// Helper: Wake up any sleeping processes whose time has come
static void wake_sleeping_processes() {
    uint64_t now = timer_get_ticks();
    Process* p = process_list;
    if (!p) return;
    
    do {
        if (p->state == PROCESS_SLEEPING && now >= p->wake_time) {
            p->state = PROCESS_READY;
        }
        p = p->next;
    } while (p != process_list);
}

void scheduler_schedule() {
    if (!current_process) return;
    
    // Disable interrupts during scheduling to prevent reentrancy
    // Note: We don't use spinlock here because we can't hold it across context switch
    uint64_t flags = interrupts_save_disable();
    
    // Wake up any sleeping processes
    wake_sleeping_processes();
    
    Process* next = current_process->next;
    Process* start = next;
    
    // Find next runnable process
    do {
        if (next->state == PROCESS_READY || next->state == PROCESS_RUNNING) {
            break;
        }
        next = next->next;
    } while (next != start);
    
    if (next == current_process || 
        (next->state != PROCESS_READY && next->state != PROCESS_RUNNING)) {
        interrupts_restore(flags);
        return;
    }
    
    Process* prev = current_process;
    if (prev->state == PROCESS_RUNNING) {
        prev->state = PROCESS_READY;
    }
    
    current_process = next;
    current_process->state = PROCESS_RUNNING;
    
    // CRITICAL: Update TSS rsp0 before context switch!
    // When the new task returns to user mode and an interrupt occurs,
    // the CPU reads rsp0 from the TSS to find the kernel stack.
    // For processes with VMM isolation, use KERNEL_STACK_TOP
    // For kernel tasks (no page_table), use the HHDM stack address
    if (current_process->page_table) {
        // Process has its own address space - stack is at fixed virtual address
        tss_set_rsp0(KERNEL_STACK_TOP);
    } else if (current_process->stack_base) {
        // Kernel task - stack is in HHDM
        uint64_t new_rsp0 = (uint64_t)current_process->stack_base + KERNEL_STACK_SIZE;
        tss_set_rsp0(new_rsp0);
    }
    
    // Switch address space if the next process has its own page table
    if (current_process->page_table) {
        uint64_t pml4_phys = (uint64_t)current_process->page_table - vmm_get_hhdm_offset();
        vmm_switch_address_space((uint64_t*)pml4_phys);
    } else if (prev->page_table) {
        // Switching from user process back to kernel task - restore kernel PML4
        uint64_t kernel_pml4_phys = (uint64_t)vmm_get_kernel_pml4() - vmm_get_hhdm_offset();
        vmm_switch_address_space((uint64_t*)kernel_pml4_phys);
    }
    
    switch_to_task(prev, current_process);
    
    // CRITICAL: Restore interrupts after context switch!
    // switch_to_task saves/restores RFLAGS via pushfq/popfq, but since we 
    // disabled interrupts before the switch, the saved RFLAGS has IF=0.
    // If we don't restore here, this task resumes with interrupts disabled,
    // causing the system to hang (no timer interrupts = no scheduling).
    interrupts_restore(flags);
}

void scheduler_yield() {
    scheduler_schedule();
}

// Fork: Create a copy of current process with VMM isolation
uint64_t process_fork() {
    Process* parent = current_process;
    
    // Use aligned_alloc to ensure FPU state is 16-byte aligned for fxsave/fxrstor
    Process* child = (Process*)aligned_alloc(16, sizeof(Process));
    if (!child) return (uint64_t)-1;
    
    // Zero the child struct first
    uint8_t* p = (uint8_t*)child;
    for (size_t i = 0; i < sizeof(Process); i++) p[i] = 0;
    
    spinlock_acquire(&scheduler_lock);
    child->pid = next_pid++;
    spinlock_release(&scheduler_lock);
    
    child->parent_pid = parent->pid;
    child->state = PROCESS_READY;
    child->exit_status = 0;
    child->wait_for_pid = 0;
    
    // Copy parent's FPU state
    for (size_t i = 0; i < FPU_STATE_SIZE; i++) {
        child->fpu_state[i] = parent->fpu_state[i];
    }
    child->fpu_initialized = true;
    
    // === VMM ISOLATION ===
    // Clone parent's address space (or create new if parent is kernel task)
    if (parent->page_table) {
        child->page_table = vmm_clone_address_space(parent->page_table);
    } else {
        child->page_table = vmm_create_address_space();
    }
    
    if (!child->page_table) {
        aligned_free(child);
        return (uint64_t)-1;
    }
    
    // Allocate physical pages for child's kernel stack
    size_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void* stack_phys = pmm_alloc_frames(stack_pages);
    if (!stack_phys) {
        vmm_free_address_space(child->page_table);
        aligned_free(child);
        return (uint64_t)-1;
    }
    child->stack_phys = (uint64_t)stack_phys;
    
    // Map stack at KERNEL_STACK_TOP - KERNEL_STACK_SIZE in child's address space
    uint64_t stack_virt_base = KERNEL_STACK_TOP - KERNEL_STACK_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        uint64_t virt = stack_virt_base + i * 4096;
        uint64_t phys = (uint64_t)stack_phys + i * 4096;
        vmm_map_page_in(child->page_table, virt, phys, PTE_PRESENT | PTE_WRITABLE);
    }
    child->stack_base = (uint64_t*)stack_virt_base;
    
    // Copy parent's stack content to child's physical pages
    // This works because:
    // - Parent's stack is either at KERNEL_STACK_TOP (if isolated) or HHDM (if kernel task)
    // - Child's stack is at KERNEL_STACK_TOP (isolated)
    // - RBP pointers on stack reference KERNEL_STACK_TOP range, which is valid in BOTH address spaces
    uint64_t* dst = (uint64_t*)(child->stack_phys + vmm_get_hhdm_offset());
    
    if (parent->page_table) {
        // Parent is isolated - copy from parent's physical stack via HHDM
        // RBP pointers already reference KERNEL_STACK_TOP, no rebasing needed
        uint64_t* src = (uint64_t*)(parent->stack_phys + vmm_get_hhdm_offset());
        for (size_t i = 0; i < KERNEL_STACK_SIZE / sizeof(uint64_t); i++) {
            dst[i] = src[i];
        }
        // Child's SP is same as parent's (both use KERNEL_STACK_TOP)
        child->sp = parent->sp;
    } else {
        // Parent is kernel task (HHDM stack) - copy and REBASE pointers
        // CRITICAL: RBP values on parent's stack point to HHDM addresses.
        // We must rebase them to point to KERNEL_STACK_TOP range.
        uint64_t* src = parent->stack_base;
        uint64_t parent_stack_start = (uint64_t)parent->stack_base;
        uint64_t parent_stack_end = parent_stack_start + KERNEL_STACK_SIZE;
        
        for (size_t i = 0; i < KERNEL_STACK_SIZE / sizeof(uint64_t); i++) {
            uint64_t val = src[i];
            // Check if value looks like a pointer into parent's stack
            if (val >= parent_stack_start && val < parent_stack_end) {
                // Rebase: convert HHDM address to fixed virtual address
                uint64_t offset = val - parent_stack_start;
                dst[i] = stack_virt_base + offset;
            } else {
                dst[i] = val;
            }
        }
        // Adjust SP from HHDM to fixed virtual address
        uint64_t sp_offset = parent->sp - parent_stack_start;
        child->sp = stack_virt_base + sp_offset;
    }
    
    // Add to list (protected by scheduler lock)
    spinlock_acquire(&scheduler_lock);
    Process* last = process_list;
    while (last->next != process_list) {
        last = last->next;
    }
    last->next = child;
    child->next = process_list;
    spinlock_release(&scheduler_lock);
    
    DEBUG_INFO("Forked PID %d -> %d (isolated)\n", parent->pid, child->pid);
    return child->pid;
}

void process_exit(int32_t status) {
    DEBUG_INFO("Process %d exiting with status %d\n", current_process->pid, status);
    
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_status = status;
    
    // Wake up parent if waiting
    Process* parent = process_find_by_pid(current_process->parent_pid);
    if (parent && parent->state == PROCESS_WAITING) {
        if (parent->wait_for_pid == 0 || parent->wait_for_pid == current_process->pid) {
            parent->state = PROCESS_READY;
        }
    }
    
    scheduler_schedule();
    for(;;);
}

int64_t process_waitpid(int64_t pid, int32_t* status) {
    while (true) {
        // Look for zombie child
        Process* p = process_list;
        do {
            if (p->parent_pid == current_process->pid && p->state == PROCESS_ZOMBIE) {
                if (pid == -1 || (uint64_t)pid == p->pid) {
                    // Found zombie
                    if (status) *status = p->exit_status;
                    uint64_t child_pid = p->pid;
                    
                    // Unlink from circular list
                    // Find the previous node
                    spinlock_acquire(&scheduler_lock);
                    Process* prev_node = process_list;
                    while (prev_node->next != p && prev_node->next != process_list) {
                        prev_node = prev_node->next;
                    }
                    if (prev_node->next == p) {
                        prev_node->next = p->next;
                        // If p was process_list head, move head
                        if (process_list == p) {
                            process_list = p->next;
                        }
                    }
                    spinlock_release(&scheduler_lock);
                    
                    // Free resources
                    // For VMM-isolated processes, free physical stack and address space
                    if (p->page_table) {
                        // Free physical stack pages
                        if (p->stack_phys) {
                            size_t stack_pages = KERNEL_STACK_SIZE / 4096;
                            for (size_t i = 0; i < stack_pages; i++) {
                                pmm_free_frame((void*)(p->stack_phys + i * 4096));
                            }
                        }
                        // Free address space (user pages + page tables)
                        vmm_free_address_space(p->page_table);
                    } else if (p->stack_base) {
                        // Kernel task - stack was heap-allocated
                        free(p->stack_base);
                    }
                    aligned_free(p);  // Process was allocated with aligned_alloc
                    
                    DEBUG_INFO("Reaped zombie PID %d\n", child_pid);
                    return child_pid;
                }
            }
            p = p->next;
        } while (p != process_list);
        
        // No zombie found, wait
        current_process->state = PROCESS_WAITING;
        current_process->wait_for_pid = (pid == -1) ? 0 : pid;
        scheduler_schedule();
    }
}

// Sleep current process for a given number of timer ticks
void scheduler_sleep(uint64_t ticks) {
    if (!current_process) return;
    
    uint64_t flags = interrupts_save_disable();
    
    current_process->wake_time = timer_get_ticks() + ticks;
    current_process->state = PROCESS_SLEEPING;
    
    interrupts_restore(flags);
    
    // Yield to let another process run
    scheduler_schedule();
}

// Sleep current process for a given number of milliseconds
void scheduler_sleep_ms(uint64_t ms) {
    uint32_t freq = timer_get_frequency();
    // Convert ms to ticks: ticks = ms * freq / 1000
    // Avoid overflow by doing division first for large ms values
    uint64_t ticks = (ms * freq) / 1000;
    if (ticks == 0 && ms > 0) ticks = 1;  // At least 1 tick for non-zero ms
    scheduler_sleep(ticks);
}
