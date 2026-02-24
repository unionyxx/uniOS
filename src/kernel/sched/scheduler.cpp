#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <kernel/arch/x86_64/gdt.h>
#include <libk/kstring.h>
#include <stddef.h>

// External assembly function to initialize FPU state
extern "C" void init_fpu_state(uint8_t* fpu_buffer);

// Scheduler lock for thread safety
static Spinlock scheduler_lock = SPINLOCK_INIT;

static Process* current_process    = nullptr;
static Process* process_list       = nullptr;
// FIXED: tail pointer for O(1) insertion instead of O(n) list walk
static Process* process_list_tail  = nullptr;
static uint64_t next_pid           = 1;

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

Process* scheduler_get_process_list() {
    return process_list;
}

void scheduler_init() {
    DEBUG_INFO("Initializing Scheduler...\n");

    // Use aligned_alloc to ensure FPU state is 16-byte aligned for fxsave/fxrstor
    current_process = (Process*)aligned_alloc(16, sizeof(Process));
    if (!current_process) {
        panic("Failed to allocate initial process!");
    }

    // FIXED: replaced manual byte loop with __builtin_memset
    __builtin_memset(current_process, 0, sizeof(Process));

    // Allocate a real stack for the idle task so rsp0 can be updated correctly
    // when switching back to this task after a user-mode task runs.
    current_process->stack_base = (uint64_t*)malloc(KERNEL_STACK_SIZE);
    if (!current_process->stack_base) {
        panic("Failed to allocate idle task stack!");
    }

    // Stack sentinel: fill bottom 8 words with magic pattern for overflow detection
    uint64_t* sentinel = current_process->stack_base;
    for (size_t i = 0; i < 8; i++) {
        sentinel[i] = 0xDEADBEEFDEADBEEFULL;
    }

    current_process->pid        = 0;
    current_process->parent_pid = 0;

    const char* init_name = "Kernel";
    int ni = 0;
    while (init_name[ni] && ni < 31) {
        current_process->name[ni] = init_name[ni];
        ni++;
    }
    current_process->name[ni] = '\0';

    current_process->cpu_time    = 0;
    current_process->sp          = 0;
    current_process->stack_phys  = 0;
    current_process->page_table  = nullptr;
    current_process->state       = PROCESS_RUNNING;
    current_process->exit_status = 0;
    current_process->wait_for_pid = 0;
    current_process->next        = current_process;  // Circular list

    // Initialize FPU state for idle task
    init_fpu_state(current_process->fpu_state);
    current_process->fpu_initialized = true;

    // FIXED: initialise both head and tail
    process_list      = current_process;
    process_list_tail = current_process;

    DEBUG_INFO("Scheduler Initialized. Initial PID: 0\n");
}

void scheduler_create_task(void (*entry)(), const char* name) {
    // Disable interrupts to prevent timer IRQ from running scheduler_schedule
    // while we modify the process list, which would cause corruption/deadlock.
    uint64_t flags = interrupts_save_disable();

    // Use aligned_alloc to ensure FPU state is 16-byte aligned for fxsave/fxrstor
    Process* new_process = (Process*)aligned_alloc(16, sizeof(Process));
    if (!new_process) {
        DEBUG_ERROR("Failed to allocate process struct\n");
        interrupts_restore(flags);
        return;
    }

    // FIXED: replaced manual byte loop with __builtin_memset
    __builtin_memset(new_process, 0, sizeof(Process));

    new_process->pid        = next_pid++;
    new_process->parent_pid = current_process ? current_process->pid : 0;

    int ni = 0;
    if (name) {
        while (name[ni] && ni < 31) {
            new_process->name[ni] = name[ni];
            ni++;
        }
    }
    new_process->name[ni] = '\0';

    new_process->cpu_time    = 0;
    new_process->state       = PROCESS_READY;
    new_process->exit_status = 0;
    new_process->wait_for_pid = 0;
    new_process->page_table  = nullptr;
    new_process->stack_phys  = 0;

    // Initialize FPU state
    init_fpu_state(new_process->fpu_state);
    new_process->fpu_initialized = true;

    // Allocate kernel stack (16 KB for deep call chains like networking)
    new_process->stack_base = (uint64_t*)malloc(KERNEL_STACK_SIZE);
    if (!new_process->stack_base) {
        DEBUG_ERROR("Failed to allocate stack for PID %d\n", new_process->pid);
        aligned_free(new_process);
        interrupts_restore(flags);
        return;
    }

    // Stack sentinel
    uint64_t* sentinel = new_process->stack_base;
    for (size_t i = 0; i < 8; i++) {
        sentinel[i] = 0xDEADBEEFDEADBEEFULL;
    }

    // Align stack top to 16 bytes
    uint64_t  stack_addr = (uint64_t)new_process->stack_base + KERNEL_STACK_SIZE;
    stack_addr &= ~(uint64_t)0xF;
    uint64_t* stack_top = (uint64_t*)stack_addr;

    // Set up initial stack frame for switch_to_task
    stack_top--; *stack_top = 0;               // Dummy return address
    stack_top--; *stack_top = (uint64_t)entry; // RIP
    stack_top--; *stack_top = 0x202;           // RFLAGS (IF=1)

    // Callee-saved registers (rbp, rbx, r12, r13, r14, r15)
    for (int i = 0; i < 6; i++) {
        stack_top--; *stack_top = 0;
    }

    new_process->sp = (uint64_t)stack_top;

    // FIXED: O(1) insertion using process_list_tail instead of O(n) list walk
    spinlock_acquire(&scheduler_lock);
    process_list_tail->next = new_process;
    new_process->next       = process_list;   // circular: new tail points to head
    process_list_tail       = new_process;
    spinlock_release(&scheduler_lock);

    interrupts_restore(flags);
    DEBUG_INFO("Created Task PID: %d\n", new_process->pid);
}

// Helper: wake up any sleeping processes whose sleep timer has expired
static void wake_sleeping_processes() {
    uint64_t now = timer_get_ticks();
    Process* p   = process_list;
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

    // FIXED: check ALL 8 sentinel words, not just [0].
    // An overflow that reaches [1]-[7] but not yet [0] was previously invisible.
    if (current_process->stack_base) {
        for (int s = 0; s < 8; s++) {
            if (current_process->stack_base[s] != 0xDEADBEEFDEADBEEFULL) {
                kprintf_color(0xFF0000, "\n*** STACK OVERFLOW DETECTED ***\n");
                kprintf("Process: PID=%d Name=%s sentinel[%d]=%llx\n",
                        current_process->pid, current_process->name,
                        s, current_process->stack_base[s]);
                panic("Stack overflow detected in process!");
            }
        }
    }

    // Disable interrupts during scheduling to prevent reentrancy.
    // We cannot hold the spinlock across the context switch itself.
    uint64_t flags = interrupts_save_disable();

    wake_sleeping_processes();

    Process* next  = current_process->next;
    Process* start = next;

    // Find the next runnable process
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

    current_process        = next;
    current_process->state = PROCESS_RUNNING;

    // CRITICAL: Update TSS rsp0 before context switch.
    // When the new task returns to user mode and an interrupt fires, the CPU
    // reads rsp0 from the TSS to find the kernel stack.
    if (current_process->page_table) {
        tss_set_rsp0(KERNEL_STACK_TOP);
    } else if (current_process->stack_base) {
        uint64_t new_rsp0 = (uint64_t)current_process->stack_base + KERNEL_STACK_SIZE;
        tss_set_rsp0(new_rsp0);
    }

    // Switch address space if required
    if (current_process->page_table) {
        uint64_t pml4_phys = (uint64_t)current_process->page_table - vmm_get_hhdm_offset();
        vmm_switch_address_space((uint64_t*)pml4_phys);
    } else if (prev->page_table) {
        // Switching from user process back to kernel task – restore kernel PML4
        uint64_t kernel_pml4_phys = (uint64_t)vmm_get_kernel_pml4() - vmm_get_hhdm_offset();
        vmm_switch_address_space((uint64_t*)kernel_pml4_phys);
    }

    switch_to_task(prev, current_process);

    // CRITICAL: Restore interrupts after the context switch.
    // switch_to_task saves/restores RFLAGS via pushfq/popfq, but since we
    // disabled interrupts before the switch the saved RFLAGS has IF=0.
    // Without this restore the task would resume with interrupts permanently
    // disabled, hanging the system (no timer = no scheduling).
    interrupts_restore(flags);
}

void scheduler_yield() {
    scheduler_schedule();
}

// Fork: create a copy of the current process with VMM isolation
uint64_t process_fork() {
    Process* parent = current_process;

    Process* child = (Process*)aligned_alloc(16, sizeof(Process));
    if (!child) return (uint64_t)-1;

    // FIXED: replaced manual byte loop with __builtin_memset
    __builtin_memset(child, 0, sizeof(Process));

    spinlock_acquire(&scheduler_lock);
    child->pid = next_pid++;
    spinlock_release(&scheduler_lock);

    child->parent_pid  = parent->pid;
    child->state       = PROCESS_READY;
    child->exit_status = 0;
    child->wait_for_pid = 0;

    // Copy parent's FPU state
    for (size_t i = 0; i < FPU_STATE_SIZE; i++) {
        child->fpu_state[i] = parent->fpu_state[i];
    }
    child->fpu_initialized = true;

    // === VMM isolation ===
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
    void*  stack_phys  = pmm_alloc_frames(stack_pages);
    if (!stack_phys) {
        vmm_free_address_space(child->page_table);
        aligned_free(child);
        return (uint64_t)-1;
    }
    child->stack_phys = (uint64_t)stack_phys;

    uint64_t stack_virt_base = KERNEL_STACK_TOP - KERNEL_STACK_SIZE;
    for (size_t i = 0; i < stack_pages; i++) {
        uint64_t virt = stack_virt_base + i * 4096;
        uint64_t phys = (uint64_t)stack_phys + i * 4096;
        vmm_map_page_in(child->page_table, virt, phys, PTE_PRESENT | PTE_WRITABLE);
    }
    child->stack_base = (uint64_t*)stack_virt_base;

    // Copy parent's stack content to child's physical pages
    uint64_t* dst = (uint64_t*)(child->stack_phys + vmm_get_hhdm_offset());

    if (parent->page_table) {
        // Parent is VMM-isolated – copy via HHDM; RBP pointers reference
        // KERNEL_STACK_TOP which is valid in both address spaces, no rebase needed.
        uint64_t* src = (uint64_t*)(parent->stack_phys + vmm_get_hhdm_offset());
        for (size_t i = 0; i < KERNEL_STACK_SIZE / sizeof(uint64_t); i++) {
            dst[i] = src[i];
        }
        child->sp = parent->sp;
    } else {
        // Parent is a kernel task (HHDM stack) – copy and rebase RBP pointers.
        // NOTE: This heuristic scans for values that look like pointers into the
        // parent stack range.  It can mis-rebase integer values that happen to
        // fall in that range (e.g. a stored fd number).  A proper fix is to save
        // only the cpu_context struct (callee-saved regs) rather than scanning
        // raw stack memory.
        uint64_t* src              = parent->stack_base;
        uint64_t  parent_stack_start = (uint64_t)parent->stack_base;
        uint64_t  parent_stack_end   = parent_stack_start + KERNEL_STACK_SIZE;

        for (size_t i = 0; i < KERNEL_STACK_SIZE / sizeof(uint64_t); i++) {
            uint64_t val = src[i];
            if (val >= parent_stack_start && val < parent_stack_end) {
                uint64_t offset = val - parent_stack_start;
                dst[i] = stack_virt_base + offset;
            } else {
                dst[i] = val;
            }
        }
        uint64_t sp_offset = parent->sp - parent_stack_start;
        child->sp = stack_virt_base + sp_offset;
    }

    // FIXED: O(1) insertion using process_list_tail
    spinlock_acquire(&scheduler_lock);
    process_list_tail->next = child;
    child->next             = process_list;
    process_list_tail       = child;
    spinlock_release(&scheduler_lock);

    DEBUG_INFO("Forked PID %d -> %d (isolated)\n", parent->pid, child->pid);
    return child->pid;
}

void process_exit(int32_t status) {
    DEBUG_INFO("Process %d exiting with status %d\n",
               current_process->pid, status);

    current_process->state       = PROCESS_ZOMBIE;
    current_process->exit_status = status;

    // Wake up parent if it is blocked in waitpid
    Process* parent = process_find_by_pid(current_process->parent_pid);
    if (parent && parent->state == PROCESS_WAITING) {
        if (parent->wait_for_pid == 0 ||
            parent->wait_for_pid == current_process->pid) {
            parent->state = PROCESS_READY;
        }
    }

    scheduler_schedule();
    for (;;);
}

int64_t process_waitpid(int64_t pid, int32_t* status) {
    while (true) {
        Process* p = process_list;
        do {
            if (p->parent_pid == current_process->pid &&
                p->state      == PROCESS_ZOMBIE) {

                if (pid == -1 || (uint64_t)pid == p->pid) {
                    if (status) *status = p->exit_status;
                    uint64_t child_pid = p->pid;

                    // Unlink from circular list
                    spinlock_acquire(&scheduler_lock);
                    Process* prev_node = process_list;
                    while (prev_node->next != p &&
                           prev_node->next != process_list) {
                        prev_node = prev_node->next;
                    }
                    if (prev_node->next == p) {
                        prev_node->next = p->next;
                        if (process_list == p) {
                            process_list = p->next;
                        }
                        // FIXED: maintain tail pointer when the tail is reaped
                        if (process_list_tail == p) {
                            process_list_tail = prev_node;
                        }
                    }
                    spinlock_release(&scheduler_lock);

                    // Free process resources
                    if (p->page_table) {
                        if (p->stack_phys) {
                            size_t stack_pages = KERNEL_STACK_SIZE / 4096;
                            for (size_t i = 0; i < stack_pages; i++) {
                                pmm_free_frame((void*)(p->stack_phys + i * 4096));
                            }
                        }
                        vmm_free_address_space(p->page_table);
                    } else if (p->stack_base) {
                        free(p->stack_base);
                    }
                    aligned_free(p);

                    DEBUG_INFO("Reaped zombie PID %d\n", child_pid);
                    return child_pid;
                }
            }
            p = p->next;
        } while (p != process_list);

        // No zombie found – sleep until a child exits
        current_process->state        = PROCESS_WAITING;
        current_process->wait_for_pid = (pid == -1) ? 0 : pid;
        scheduler_schedule();
    }
}

// Sleep current process for a given number of timer ticks
void scheduler_sleep(uint64_t ticks) {
    if (!current_process) return;

    uint64_t flags = interrupts_save_disable();
    current_process->wake_time = timer_get_ticks() + ticks;
    current_process->state     = PROCESS_SLEEPING;
    interrupts_restore(flags);

    scheduler_schedule();
}

// Sleep current process for a given number of milliseconds
void scheduler_sleep_ms(uint64_t ms) {
    uint32_t freq  = timer_get_frequency();
    uint64_t ticks = (ms * freq) / 1000;
    if (ticks == 0 && ms > 0) ticks = 1;  // at least 1 tick for non-zero ms
    scheduler_sleep(ticks);
}
