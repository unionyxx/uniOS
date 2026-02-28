#include <kernel/scheduler.h>
#include <kernel/process.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/panic.h>
#include <libk/kstring.h>
#include <libk/kstd.h>
#include <stddef.h>

using kstring::string_view;

extern "C" void init_fpu_state(uint8_t* fpu_buffer);
extern "C" void fork_ret();

static Spinlock g_sched_lock = SPINLOCK_INIT;
static Process* g_current_proc = nullptr;
static Process* g_proc_list = nullptr;
static Process* g_proc_tail = nullptr;
static uint64_t g_next_pid = 1;

[[nodiscard]] Process* process_get_current() { return g_current_proc; }

[[nodiscard]] Process* process_find_by_pid(uint64_t pid) {
    Process* p = g_proc_list;
    if (!p) return nullptr;
    do {
        if (p->pid == pid) return p;
        p = p->next;
    } while (p != g_proc_list);
    return nullptr;
}

[[nodiscard]] Process* scheduler_get_process_list() { return g_proc_list; }

void scheduler_init() {
    DEBUG_INFO("Initializing Scheduler...");
    g_current_proc = static_cast<Process*>(aligned_alloc(16, sizeof(Process)));
    if (!g_current_proc) panic("Failed to allocate initial process!");

    kstring::zero_memory(g_current_proc, sizeof(Process));
    g_current_proc->stack_base = static_cast<uint64_t*>(malloc(KERNEL_STACK_SIZE));
    if (!g_current_proc->stack_base) panic("Failed to allocate idle task stack!");

    for (size_t i = 0; i < 8; i++) g_current_proc->stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    g_current_proc->pid = 0;
    kstring::strncpy(g_current_proc->name, "Kernel", 31);
    g_current_proc->state = ProcessState::Running;
    g_current_proc->cursor_x = 50;
    g_current_proc->cursor_y = 480;
    g_current_proc->cwd[0] = '/';
    g_current_proc->cwd[1] = '\0';
    g_current_proc->next = g_current_proc;

    for (auto& fd : g_current_proc->fd_table) fd.used = false;
    g_current_proc->fd_table[0].used = g_current_proc->fd_table[1].used = g_current_proc->fd_table[2].used = true;

    init_fpu_state(g_current_proc->fpu_state);
    g_current_proc->fpu_initialized = true;
    g_proc_list = g_proc_tail = g_current_proc;

    DEBUG_INFO("Scheduler Initialized. Initial PID: 0");
}

void scheduler_create_task(void (*entry)(), const char* name) {
    const uint64_t flags = interrupts_save_disable();
    Process* proc = static_cast<Process*>(aligned_alloc(16, sizeof(Process)));
    if (!proc) {
        DEBUG_ERROR("Failed to allocate process struct");
        interrupts_restore(flags);
        return;
    }

    kstring::zero_memory(proc, sizeof(Process));
    proc->pid = g_next_pid++;
    proc->parent_pid = g_current_proc ? g_current_proc->pid : 0;
    if (name) kstring::strncpy(proc->name, name, 31);
    proc->state = ProcessState::Ready;
    proc->cursor_x = 50; proc->cursor_y = 480;
    proc->cwd[0] = '/'; proc->cwd[1] = '\0';

    for (auto& fd : proc->fd_table) fd.used = false;
    proc->fd_table[0].used = proc->fd_table[1].used = proc->fd_table[2].used = true;

    init_fpu_state(proc->fpu_state);
    proc->fpu_initialized = true;

    proc->stack_base = static_cast<uint64_t*>(malloc(KERNEL_STACK_SIZE));
    if (!proc->stack_base) {
        aligned_free(proc);
        interrupts_restore(flags);
        return;
    }
    for (size_t i = 0; i < 8; i++) proc->stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    uint64_t* stack_top = reinterpret_cast<uint64_t*>((reinterpret_cast<uint64_t>(proc->stack_base) + KERNEL_STACK_SIZE) & ~0xFULL);
    *(--stack_top) = 0;
    *(--stack_top) = reinterpret_cast<uint64_t>(entry);
    *(--stack_top) = 0x202;
    for (int i = 0; i < 6; i++) *(--stack_top) = 0;
    proc->sp = reinterpret_cast<uint64_t>(stack_top);

    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = proc;
    proc->next = g_proc_list;
    g_proc_tail = proc;
    spinlock_release(&g_sched_lock);

    interrupts_restore(flags);
    DEBUG_INFO("Created Task PID: %d", proc->pid);
}

static void wake_sleeping_processes() {
    const uint64_t now = timer_get_ticks();
    Process* p = g_proc_list;
    if (!p) return;
    do {
        if (p->state == ProcessState::Sleeping && now >= p->wake_time) p->state = ProcessState::Ready;
        p = p->next;
    } while (p != g_proc_list);
}

void scheduler_schedule() {
    if (!g_current_proc) return;
    if (g_current_proc->stack_base) {
        for (int s = 0; s < 8; s++) {
            if (g_current_proc->stack_base[s] != 0xDEADBEEFDEADBEEFULL) panic("Stack overflow detected!");
        }
    }

    const uint64_t flags = interrupts_save_disable();
    g_current_proc->cpu_time++;
    wake_sleeping_processes();

    Process* next = g_current_proc->next;
    Process* start = next;
    do {
        if (next->state == ProcessState::Ready || next->state == ProcessState::Running) break;
        next = next->next;
    } while (next != start);

    if (next == g_current_proc || (next->state != ProcessState::Ready && next->state != ProcessState::Running)) {
        interrupts_restore(flags);
        return;
    }

    Process* prev = g_current_proc;
    if (prev->state == ProcessState::Running) prev->state = ProcessState::Ready;
    g_current_proc = next;
    g_current_proc->state = ProcessState::Running;

    if (g_current_proc->page_table) tss_set_rsp0(KERNEL_STACK_TOP);
    else if (g_current_proc->stack_base) tss_set_rsp0(reinterpret_cast<uint64_t>(g_current_proc->stack_base) + KERNEL_STACK_SIZE);

    if (g_current_proc->page_table) vmm_switch_address_space(reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(g_current_proc->page_table) - vmm_get_hhdm_offset()));
    else if (prev->page_table) vmm_switch_address_space(reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));

    switch_to_task(prev, g_current_proc);
    interrupts_restore(flags);
}

void scheduler_yield() { scheduler_schedule(); }

[[nodiscard]] uint64_t process_fork(SyscallFrame* frame) {
    if (!g_current_proc->page_table) panic("process_fork: Cannot fork a kernel thread!");
    Process* child = static_cast<Process*>(aligned_alloc(16, sizeof(Process)));
    if (!child) return static_cast<uint64_t>(-1);

    kstring::zero_memory(child, sizeof(Process));
    spinlock_acquire(&g_sched_lock);
    child->pid = g_next_pid++;
    spinlock_release(&g_sched_lock);

    child->parent_pid = g_current_proc->pid;
    child->state = ProcessState::Ready;
    kstring::memcpy(child->fpu_state, g_current_proc->fpu_state, FPU_STATE_SIZE);
    child->fpu_initialized = true;
    for (int i = 0; i < MAX_OPEN_FILES; i++) child->fd_table[i] = g_current_proc->fd_table[i];
    child->cursor_x = g_current_proc->cursor_x; child->cursor_y = g_current_proc->cursor_y;
    child->page_table = vmm_clone_address_space(g_current_proc->page_table);
    child->vma_list = vma_clone(g_current_proc->vma_list);

    if (!child->page_table) { aligned_free(child); return static_cast<uint64_t>(-1); }

    const size_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void* stack_phys = pmm_alloc_frames(stack_pages);
    if (!stack_phys) { vmm_free_address_space(child->page_table); aligned_free(child); return static_cast<uint64_t>(-1); }
    child->stack_phys = reinterpret_cast<uint64_t>(stack_phys);

    const uint64_t stack_virt_base = KERNEL_STACK_TOP - KERNEL_STACK_SIZE;
    for (size_t i = 0; i < stack_pages; i++) vmm_map_page_in(child->page_table, stack_virt_base + i * 4096, child->stack_phys + i * 4096, PTE_PRESENT | PTE_WRITABLE);
    child->stack_base = reinterpret_cast<uint64_t*>(stack_virt_base);

    uint64_t* hhdm_stack_base = reinterpret_cast<uint64_t*>(child->stack_phys + vmm_get_hhdm_offset());
    for (size_t i = 0; i < 8; i++) hhdm_stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    uint64_t stack_top_hhdm = child->stack_phys + KERNEL_STACK_SIZE + vmm_get_hhdm_offset();
    stack_top_hhdm -= sizeof(SyscallFrame);
    *reinterpret_cast<SyscallFrame*>(stack_top_hhdm) = *frame;
    stack_top_hhdm -= sizeof(Context);
    Context* child_context = reinterpret_cast<Context*>(stack_top_hhdm);
    kstring::zero_memory(child_context, sizeof(Context));
    child_context->rflags = 0x202; child_context->rip = reinterpret_cast<uint64_t>(fork_ret);
    child->sp = KERNEL_STACK_TOP - (sizeof(SyscallFrame) + sizeof(Context));

    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = child; g_proc_tail = child; child->next = g_proc_list;
    spinlock_release(&g_sched_lock);

    DEBUG_INFO("Forked PID %d -> %d (isolated)", g_current_proc->pid, child->pid);
    return child->pid;
}

void process_exit(int32_t status) {
    DEBUG_INFO("Process %d exiting with status %d", g_current_proc->pid, status);
    g_current_proc->state = ProcessState::Zombie;
    g_current_proc->exit_status = status;
    if (Process* parent = process_find_by_pid(g_current_proc->parent_pid); parent && parent->state == ProcessState::Waiting) {
        if (parent->wait_for_pid == 0 || parent->wait_for_pid == g_current_proc->pid) parent->state = ProcessState::Ready;
    }
    scheduler_schedule();
    for (;;);
}

[[nodiscard]] int64_t process_waitpid(int64_t pid, int32_t* status) {
    while (true) {
        Process* p = g_proc_list;
        do {
            if (p->parent_pid == g_current_proc->pid && p->state == ProcessState::Zombie) {
                if (pid == -1 || static_cast<uint64_t>(pid) == p->pid) {
                    if (status) *status = p->exit_status;
                    const uint64_t child_pid = p->pid;
                    spinlock_acquire(&g_sched_lock);
                    Process* prev = g_proc_list;
                    while (prev->next != p && prev->next != g_proc_list) prev = prev->next;
                    if (prev->next == p) {
                        prev->next = p->next;
                        if (g_proc_list == p) g_proc_list = p->next;
                        if (g_proc_tail == p) g_proc_tail = prev;
                    }
                    spinlock_release(&g_sched_lock);
                    if (p->page_table) {
                        if (p->stack_phys) {
                            for (size_t i = 0; i < KERNEL_STACK_SIZE / 4096; i++) pmm_free_frame(reinterpret_cast<void*>(p->stack_phys + i * 4096));
                        }
                        vmm_free_address_space(p->page_table);
                    } else if (p->stack_base) free(p->stack_base);
                    if (p->vma_list) vma_free_all(p->vma_list);
                    aligned_free(p);
                    DEBUG_INFO("Reaped zombie PID %d", child_pid);
                    return static_cast<int64_t>(child_pid);
                }
            }
            p = p->next;
        } while (p != g_proc_list);
        g_current_proc->state = ProcessState::Waiting;
        g_current_proc->wait_for_pid = (pid == -1) ? 0 : static_cast<uint64_t>(pid);
        scheduler_schedule();
    }
}

void scheduler_sleep(uint64_t ticks) {
    if (!g_current_proc) return;
    const uint64_t flags = interrupts_save_disable();
    g_current_proc->wake_time = timer_get_ticks() + ticks;
    g_current_proc->state = ProcessState::Sleeping;
    interrupts_restore(flags);
    scheduler_schedule();
}

void scheduler_sleep_ms(uint64_t ms) {
    const uint32_t freq = timer_get_frequency();
    uint64_t ticks = (ms * freq) / 1000;
    if (ticks == 0 && ms > 0) ticks = 1;
    scheduler_sleep(ticks);
}
