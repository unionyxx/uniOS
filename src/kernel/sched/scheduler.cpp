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

static Spinlock scheduler_lock = SPINLOCK_INIT;

static Process* current_process    = nullptr;
static Process* process_list       = nullptr;
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
    DEBUG_INFO("Initializing Scheduler...");

    current_process = (Process*)aligned_alloc(16, sizeof(Process));
    if (!current_process) {
        panic("Failed to allocate initial process!");
    }

    __builtin_memset(current_process, 0, sizeof(Process));

    current_process->stack_base = (uint64_t*)malloc(KERNEL_STACK_SIZE);
    if (!current_process->stack_base) {
        panic("Failed to allocate idle task stack!");
    }

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
    current_process->next        = current_process;
    current_process->vma_list    = nullptr;
    current_process->cursor_x    = 50;
    current_process->cursor_y    = 480;
    current_process->exec_entry  = 0;
    current_process->exec_done   = false;
    current_process->exec_exit_status = 0;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        current_process->fd_table[i].in_use = false;
    }
    current_process->fd_table[0].in_use = true;
    current_process->fd_table[1].in_use = true;
    current_process->fd_table[2].in_use = true;

    init_fpu_state(current_process->fpu_state);
    current_process->fpu_initialized = true;

    process_list      = current_process;
    process_list_tail = current_process;

    DEBUG_INFO("Scheduler Initialized. Initial PID: 0");
}

void scheduler_create_task(void (*entry)(), const char* name) {
    // Disable interrupts to prevent timer IRQ from running scheduler_schedule
    // while we modify the process list, which would cause corruption/deadlock.
    uint64_t flags = interrupts_save_disable();

    // Use aligned_alloc to ensure FPU state is 16-byte aligned for fxsave/fxrstor
    Process* new_process = (Process*)aligned_alloc(16, sizeof(Process));
    if (!new_process) {
        DEBUG_ERROR("Failed to allocate process struct");
        interrupts_restore(flags);
        return;
    }

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
    new_process->vma_list    = nullptr;
    new_process->cursor_x    = 50;
    new_process->cursor_y    = 480;
    new_process->exec_entry  = 0;
    new_process->exec_done   = false;
    new_process->exec_exit_status = 0;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        new_process->fd_table[i].in_use = false;
    }
    new_process->fd_table[0].in_use = true;
    new_process->fd_table[1].in_use = true;
    new_process->fd_table[2].in_use = true;

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

    uint64_t* sentinel = new_process->stack_base;
    for (size_t i = 0; i < 8; i++) {
        sentinel[i] = 0xDEADBEEFDEADBEEFULL;
    }

    uint64_t  stack_addr = (uint64_t)new_process->stack_base + KERNEL_STACK_SIZE;
    stack_addr &= ~(uint64_t)0xF;
    uint64_t* stack_top = (uint64_t*)stack_addr;

    stack_top--; *stack_top = 0;               // Dummy return address
    stack_top--; *stack_top = (uint64_t)entry; // RIP
    stack_top--; *stack_top = 0x202;           // RFLAGS (IF=1)

    // Callee-saved registers
    for (int i = 0; i < 6; i++) {
        stack_top--; *stack_top = 0;
    }

    new_process->sp = (uint64_t)stack_top;

    spinlock_acquire(&scheduler_lock);
    process_list_tail->next = new_process;
    new_process->next       = process_list;
    process_list_tail       = new_process;
    spinlock_release(&scheduler_lock);

    interrupts_restore(flags);
    DEBUG_INFO("Created Task PID: %d", new_process->pid);
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

    // Check all stack sentinel words
    if (current_process->stack_base) {
        for (int s = 0; s < 8; s++) {
            if (current_process->stack_base[s] != 0xDEADBEEFDEADBEEFULL) {
                kprintf_color(LOG_COLOR_ERROR, "\n*** STACK OVERFLOW DETECTED ***\n");
                kprintf("Process: PID=%d Name=%s sentinel[%d]=%llx\n",
                        current_process->pid, current_process->name,
                        s, current_process->stack_base[s]);
                panic("Stack overflow detected in process!");
            }
        }
    }

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
    if (current_process->page_table) {
        tss_set_rsp0(KERNEL_STACK_TOP);
    } else if (current_process->stack_base) {
        uint64_t new_rsp0 = (uint64_t)current_process->stack_base + KERNEL_STACK_SIZE;
        tss_set_rsp0(new_rsp0);
    }

    if (current_process->page_table) {
        uint64_t pml4_phys = (uint64_t)current_process->page_table - vmm_get_hhdm_offset();
        vmm_switch_address_space((uint64_t*)pml4_phys);
    } else if (prev->page_table) {
        uint64_t kernel_pml4_phys = (uint64_t)vmm_get_kernel_pml4() - vmm_get_hhdm_offset();
        vmm_switch_address_space((uint64_t*)kernel_pml4_phys);
    }

    switch_to_task(prev, current_process);

    // CRITICAL: Restore interrupts after context switch.
    interrupts_restore(flags);
}

void scheduler_yield() {
    scheduler_schedule();
}

extern "C" void fork_ret();

// Fork: create a copy of the current process with VMM isolation
uint64_t process_fork(struct SyscallFrame* frame) {
    Process* parent = current_process;

    if (!parent->page_table) {
        panic("process_fork: Cannot fork a kernel thread!");
    }

    Process* child = (Process*)aligned_alloc(16, sizeof(Process));
    if (!child) return (uint64_t)-1;

    __builtin_memset(child, 0, sizeof(Process));

    spinlock_acquire(&scheduler_lock);
    child->pid = next_pid++;
    spinlock_release(&scheduler_lock);

    child->parent_pid  = parent->pid;
    child->state       = PROCESS_READY;
    child->exit_status = 0;
    child->wait_for_pid = 0;

    // FPU state
    for (size_t i = 0; i < FPU_STATE_SIZE; i++) {
        child->fpu_state[i] = parent->fpu_state[i];
    }
    child->fpu_initialized = true;

    // File descriptors
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child->fd_table[i] = parent->fd_table[i];
    }

    child->cursor_x = parent->cursor_x;
    child->cursor_y = parent->cursor_y;
    child->exec_entry = 0;
    child->exec_done = false;
    child->exec_exit_status = 0;

    // === VMM isolation ===
    child->page_table = vmm_clone_address_space(parent->page_table);
    child->vma_list   = vma_clone(parent->vma_list);

    if (!child->page_table) {
        aligned_free(child);
        return (uint64_t)-1;
    }

    // Child kernel stack
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

    // Stack sentinel
    uint64_t* hhdm_stack_base = (uint64_t*)(child->stack_phys + vmm_get_hhdm_offset());
    for (size_t i = 0; i < 8; i++) {
        hhdm_stack_base[i] = 0xDEADBEEFDEADBEEFULL;
    }

    // CPU context on new kernel stack
    uint64_t stack_top_hhdm = child->stack_phys + KERNEL_STACK_SIZE + vmm_get_hhdm_offset();
    
    stack_top_hhdm -= sizeof(SyscallFrame);
    SyscallFrame* child_frame = (SyscallFrame*)stack_top_hhdm;
    *child_frame = *frame;
    
    stack_top_hhdm -= sizeof(Context);
    Context* child_context = (Context*)stack_top_hhdm;
    __builtin_memset(child_context, 0, sizeof(Context));
    child_context->rflags = 0x202; // IF=1
    child_context->rip = (uint64_t)fork_ret;
    
    uint64_t used_bytes = sizeof(SyscallFrame) + sizeof(Context);
    child->sp = KERNEL_STACK_TOP - used_bytes;

    spinlock_acquire(&scheduler_lock);
    process_list_tail->next = child;
    child->next             = process_list;
    process_list_tail       = child;
    spinlock_release(&scheduler_lock);

    DEBUG_INFO("Forked PID %d -> %d (isolated)", parent->pid, child->pid);
    return child->pid;
}

void process_exit(int32_t status) {
    DEBUG_INFO("Process %d exiting with status %d",
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
                    if (p->vma_list) {
                        vma_free_all(p->vma_list);
                    }
                    aligned_free(p);

                    DEBUG_INFO("Reaped zombie PID %d", child_pid);
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
