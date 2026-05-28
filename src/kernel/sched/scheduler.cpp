#include <drivers/acpi/acpi.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>
#include <uapi/syscalls.h>

extern "C" void load_idt(void *);
extern "C" void init_fpu_state(uint8_t *fpu_buffer);
extern "C" void fork_ret();

static Spinlock g_sched_lock = SPINLOCK_INIT;
static Process *g_current_proc = nullptr;
static Process *g_proc_list = nullptr;
static Process *g_proc_tail = nullptr;
static uint64_t g_next_pid = 1;
static volatile uint32_t g_shutdown_action = 0;
WaitQueue g_epoll_wait_queue = {nullptr, nullptr};
extern "C" void scheduler_unlock_after_switch();

static void process_free_reaped(Process *target)
{
    if (!target)
        return;
    if (target->stack_phys) {
        for (size_t i = 0; i < KERNEL_STACK_SIZE / 4096; i++)
            pmm_free_frame(reinterpret_cast<void *>(target->stack_phys + i * 4096));
    }

    bool share_page_table = false;
    bool share_vma_list = false;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    Process *curr = g_proc_list;
    if (curr) {
        do {
            if (curr != target) {
                if (curr->page_table == target->page_table) {
                    share_page_table = true;
                }
                if (curr->vma_list == target->vma_list) {
                    share_vma_list = true;
                }
            }
            curr = curr->next;
        } while (curr != g_proc_list);
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    if (target->page_table && !share_page_table)
        vmm_free_address_space(target->page_table);
    if (target->vma_list && !share_vma_list)
        vma_free_all(target->vma_list);

    aligned_free(target);
}

static Process *detach_kernel_zombie_locked()
{
    if (!g_proc_list)
        return nullptr;

    Process *target = nullptr;
    Process *prev = g_proc_tail;
    Process *p = g_proc_list;
    do {
        if (p != g_current_proc && p->pid != 0 && p->parent_pid == 0 && p->state == ProcessState_Zombie) {
            target = p;
            break;
        }
        prev = p;
        p = p->next;
    } while (p != g_proc_list);

    if (!target)
        return nullptr;

    Process *kernel = g_proc_list;
    if (kernel && kernel->pid == 0) {
        Process *prev_child = nullptr;
        Process *child = kernel->children_list;
        while (child) {
            if (child == target) {
                if (prev_child)
                    prev_child->sibling_next = child->sibling_next;
                else
                    kernel->children_list = child->sibling_next;
                break;
            }
            prev_child = child;
            child = child->sibling_next;
        }
    }

    if (target->next == target) {
        g_proc_list = nullptr;
        g_proc_tail = nullptr;
    } else {
        prev->next = target->next;
        if (g_proc_list == target)
            g_proc_list = target->next;
        if (g_proc_tail == target)
            g_proc_tail = prev;
    }

    target->next = nullptr;
    target->sibling_next = nullptr;
    return target;
}

static void reap_kernel_zombies()
{
    while (true) {
        const uint64_t flags = interrupts_save_disable();
        spinlock_acquire(&g_sched_lock);
        Process *target = detach_kernel_zombie_locked();
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);
        if (!target)
            return;
        DEBUG_INFO("Reaped detached zombie PID %d", target->pid);
        process_free_reaped(target);
    }
}

static void process_release_private_fds(Process *proc)
{
    if (!proc)
        return;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!proc->fd_table[i].used)
            continue;

        VNode *node = proc->fd_table[i].vnode;
        proc->fd_table[i].used = false;
        proc->fd_table[i].vnode = nullptr;
        if (node)
            vfs_close_vnode(node);
    }
}

#define NUM_PRIORITY_LEVELS 3
static Process *g_ready_queues[NUM_PRIORITY_LEVELS] = {nullptr};
static Process *g_ready_tails[NUM_PRIORITY_LEVELS] = {nullptr};
static Process *g_sleep_queue = nullptr;
static uint64_t g_last_sleep_tick = 0; // Baseline for true elapsed sleep time

static void ready_queue_push(Process *p)
{
    uint8_t prio = p->priority;
    if (prio >= NUM_PRIORITY_LEVELS)
        prio = NUM_PRIORITY_LEVELS - 1;
    p->queue_next = nullptr;
    if (!g_ready_tails[prio]) {
        g_ready_queues[prio] = g_ready_tails[prio] = p;
    } else {
        g_ready_tails[prio]->queue_next = p;
        g_ready_tails[prio] = p;
    }
}

static Process *ready_queue_pop()
{
    for (int i = 0; i < NUM_PRIORITY_LEVELS; i++) {
        if (g_ready_queues[i]) {
            Process *p = g_ready_queues[i];
            g_ready_queues[i] = p->queue_next;
            if (!g_ready_queues[i])
                g_ready_tails[i] = nullptr;
            p->queue_next = nullptr;
            return p;
        }
    }
    return nullptr;
}

void scheduler_remove_from_ready_queue(Process *p)
{
    if (!p)
        return;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    for (int i = 0; i < NUM_PRIORITY_LEVELS; i++) {
        Process *curr = g_ready_queues[i];
        Process *prev = nullptr;
        while (curr) {
            if (curr == p) {
                if (prev) {
                    prev->queue_next = curr->queue_next;
                } else {
                    g_ready_queues[i] = curr->queue_next;
                }
                if (g_ready_tails[i] == curr) {
                    g_ready_tails[i] = prev;
                }
                curr->queue_next = nullptr;
                break;
            }
            prev = curr;
            curr = curr->queue_next;
        }
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}


static void sleep_queue_push(Process *p, uint64_t ticks)
{
    p->state = ProcessState_Sleeping;
    if (!g_sleep_queue) {
        p->wake_time = ticks;
        p->queue_next = nullptr;
        g_sleep_queue = p;
        g_last_sleep_tick = timer_get_ticks(); // Initialize baseline when queue becomes active
        return;
    }

    Process *curr = g_sleep_queue;
    Process *prev = nullptr;
    uint64_t remaining = ticks;

    while (curr && remaining >= curr->wake_time) {
        remaining -= curr->wake_time;
        prev = curr;
        curr = curr->queue_next;
    }

    p->wake_time = remaining;
    p->queue_next = curr;
    if (prev)
        prev->queue_next = p;
    else
        g_sleep_queue = p;

    if (curr)
        curr->wake_time -= remaining;
}

static void wake_sleeping_processes()
{
    if (!g_sleep_queue)
        return;

    uint64_t now = timer_get_ticks();
    if (now <= g_last_sleep_tick)
        return;

    // Decrement using real elapsed hardware ticks, not scheduler invoke counts
    uint64_t diff = now - g_last_sleep_tick;
    g_last_sleep_tick = now;

    while (diff > 0 && g_sleep_queue) {
        if (g_sleep_queue->wake_time <= diff) {
            diff -= g_sleep_queue->wake_time;
            g_sleep_queue->wake_time = 0;

            Process *p = g_sleep_queue;
            g_sleep_queue = p->queue_next;
            p->state = ProcessState_Ready;
            p->queue_next = nullptr;

            if (p->priority > 0 && p->pid != 0)
                p->priority--;

            ready_queue_push(p);
        } else {
            g_sleep_queue->wake_time -= diff;
            diff = 0;
        }
    }
}

static void wait_queue_remove(WaitQueue *q, Process *p)
{
    if (!q || !p)
        return;

    Process *prev = nullptr;
    Process *curr = q->head;
    while (curr) {
        if (curr == p) {
            Process *next = curr->queue_next;
            if (prev)
                prev->queue_next = next;
            else
                q->head = next;
            if (q->tail == curr)
                q->tail = prev;
            curr->queue_next = nullptr;
            curr->waiting_queue = nullptr;
            return;
        }
        prev = curr;
        curr = curr->queue_next;
    }
}

void wait_queue_push(WaitQueue *q, Process *p)
{
    p->state = ProcessState_Waiting;
    p->queue_next = nullptr;
    p->waiting_queue = q;
    if (!q->tail) {
        q->head = q->tail = p;
    } else {
        q->tail->queue_next = p;
        q->tail = p;
    }
}

void wait_queue_wake_all(WaitQueue *q)
{
    Process *p = q->head;
    while (p) {
        Process *next = p->queue_next;
        p->waiting_queue = nullptr;
        p->queue_next = nullptr;
        if (p->state == ProcessState_Waiting || p->state == ProcessState_Blocked) {
            p->state = ProcessState_Ready;
            ready_queue_push(p);
        }
        p = next;
    }
    q->head = q->tail = nullptr;
}

static void scheduler_wake_process_locked(Process *p)
{
    if (!p)
        return;

    if (p->waiting_queue)
        wait_queue_remove(p->waiting_queue, p);

    if (p->state == ProcessState_Waiting || p->state == ProcessState_Blocked) {
        p->state = ProcessState_Ready;
        ready_queue_push(p);
    }
}

static void scheduler_schedule_internal()
{
    g_current_proc->cpu_time++;
    g_current_proc->time_slice++;

    wake_sleeping_processes();

    const uint64_t now = timer_get_ticks();

    if (g_current_proc->state == ProcessState_Running) {
        uint32_t max_slice = (g_current_proc->priority == 0) ? 5 : (g_current_proc->priority == 1) ? 20 : 50;
        if (g_current_proc->time_slice >= max_slice) {
            if (g_current_proc->priority < NUM_PRIORITY_LEVELS - 1 && g_current_proc->pid != 0) {
                g_current_proc->priority++;
            }
            g_current_proc->time_slice = 0;
        }
        g_current_proc->state = ProcessState_Ready;
        ready_queue_push(g_current_proc);
    }

    Process *next = ready_queue_pop();
    if (!next) {
        if (g_current_proc->state == ProcessState_Running || g_current_proc->state == ProcessState_Ready) {
            next = g_current_proc;
        } else {
            Process *p = g_proc_list;
            while (p->pid != 0)
                p = p->next;
            next = p;
        }
    }

    if (next == g_current_proc) {
        g_current_proc->state = ProcessState_Running;
        g_current_proc->last_run_time = now;
        spinlock_release(&g_sched_lock);
        return;
    }

    Process *prev = g_current_proc;
    if (prev->state == ProcessState_Running)
        prev->state = ProcessState_Ready;

    g_current_proc = next;
    g_current_proc->state = ProcessState_Running;
    g_current_proc->last_run_time = now;

    uint64_t next_rsp0;
    if (g_current_proc->pid == 0) {
        next_rsp0 = g_current_proc->sp;
    } else {
        next_rsp0 = reinterpret_cast<uint64_t>(g_current_proc->stack_base) + KERNEL_STACK_SIZE;
    }

    tss_set_rsp0(next_rsp0);
    cpu_get_local()->kernel_stack = next_rsp0;

    uint64_t *prev_cr3 =
        prev->page_table
            ? reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(prev->page_table) - vmm_get_hhdm_offset())
            : reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset());
    uint64_t *next_cr3 =
        g_current_proc->page_table
            ? reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(g_current_proc->page_table) -
                                           vmm_get_hhdm_offset())
            : reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset());

    if (prev_cr3 != next_cr3) {
        vmm_switch_address_space(next_cr3);
    }

    switch_to_task(prev, g_current_proc);
    scheduler_unlock_after_switch();
}

extern "C" void scheduler_unlock_after_switch()
{
    spinlock_release_no_restore(&g_sched_lock);
}

void scheduler_wait(WaitQueue *q, Spinlock *lock)
{
    if (!g_current_proc || !q)
        return;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    wait_queue_push(q, g_current_proc);

    if (lock)
        spinlock_release_no_restore(lock);

    scheduler_schedule_internal();

    interrupts_restore(flags);

    if (lock)
        spinlock_acquire(lock);
}

void scheduler_wake_all(WaitQueue *q)
{
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    wait_queue_wake_all(q);
    if (q != &g_epoll_wait_queue) {
        wait_queue_wake_all(&g_epoll_wait_queue);
    }
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

void scheduler_wake_process(Process *p)
{
    if (!p)
        return;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    scheduler_wake_process_locked(p);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

static void halt_forever()
{
    asm volatile("cli");
    for (;;)
        asm volatile("hlt");
}

static void shutdown_io_delay(unsigned rounds = 64)
{
    for (unsigned i = 0; i < rounds; i++)
        io_wait();
}

static void shutdown_mark_user_processes_zombie()
{
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    Process *p = g_proc_list;
    if (p) {
        do {
            if (p->pid > 1 && p != g_current_proc)
                p->state = ProcessState_Zombie;
            p = p->next;
        } while (p != g_proc_list);
    }
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

static void shutdown_prepare(uint32_t action, const char *message)
{
    const uint64_t flags = interrupts_save_disable();
    if (g_shutdown_action != 0) {
        interrupts_restore(flags);
        DEBUG_WARN("Shutdown already in progress; ignoring duplicate request.");
        halt_forever();
    }
    g_shutdown_action = action;
    interrupts_restore(flags);

    DEBUG_INFO("%s", message);
    shutdown_mark_user_processes_zombie();
    vfs_sync();
    asm volatile("cli" ::: "memory");
}

static bool keyboard_controller_can_accept_command()
{
    for (int i = 0; i < 0x10000; i++) {
        if ((inb(0x64) & 0x02u) == 0)
            return true;
        io_wait();
    }
    return false;
}

static void reboot_via_keyboard_controller()
{
    if (!keyboard_controller_can_accept_command())
        return;
    outb(0x64, 0xFE);
    shutdown_io_delay(256);
}

static void reboot_via_pci_reset_control()
{
    outb(0xCF9, 0x02);
    shutdown_io_delay();
    outb(0xCF9, 0x06);
    shutdown_io_delay(256);
}

static void reboot_via_triple_fault()
{
    struct
    {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {0, 0};
    asm volatile("lidt %0; int $3" ::"m"(idtr));
}

void system_reboot()
{
    shutdown_prepare(1, "System rebooting...");

    asm volatile("wbinvd" ::: "memory");
    asm volatile("cli" ::: "memory");

    acpi_reboot();
    reboot_via_keyboard_controller();
    reboot_via_pci_reset_control();
    reboot_via_triple_fault();
    halt_forever();
}

void system_poweroff()
{
    shutdown_prepare(2, "System powering off...");

    asm volatile("wbinvd" ::: "memory");
    asm volatile("cli" ::: "memory");

    acpi_poweroff();
    DEBUG_WARN("Poweroff failed, halting CPU.");
    halt_forever();
}

[[nodiscard]] Process *process_get_current()
{
    return g_current_proc;
}

[[nodiscard]] Process *process_find_by_pid(uint64_t pid)
{
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    Process *p = g_proc_list;
    if (!p) {
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);
        return nullptr;
    }
    do {
        if (p->pid == pid) {
            spinlock_release(&g_sched_lock);
            interrupts_restore(flags);
            return p;
        }
        p = p->next;
    } while (p != g_proc_list);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
    return nullptr;
}

[[nodiscard]] Process *scheduler_get_process_list()
{
    return g_proc_list;
}

void scheduler_init()
{
    DEBUG_INFO("Initializing O(1) MLFQ Scheduler...");
    g_current_proc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!g_current_proc)
        panic("Failed to allocate initial process!");

    kstring::zero_memory(g_current_proc, sizeof(Process));
    event_init(g_current_proc->event_queue);

    g_current_proc->pid = 0;
    g_current_proc->uid = 0;
    kstring::strncpy(g_current_proc->name, "Kernel", 31);
    g_current_proc->state = ProcessState_Running;

    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    g_current_proc->sp = current_rsp;
    g_current_proc->stack_base = nullptr;

    g_current_proc->priority = 2;
    spinlock_init(&g_current_proc->fd_lock);

    for (auto &fd : g_current_proc->fd_table)
        fd.used = false;
    g_current_proc->fd_table[0].used = g_current_proc->fd_table[1].used = g_current_proc->fd_table[2].used = true;

    init_fpu_state(g_current_proc->fpu_state);
    g_current_proc->fpu_initialized = true;
    g_current_proc->children_list = nullptr;
    g_current_proc->sibling_next = nullptr;
    g_current_proc->next = g_current_proc;
    g_proc_list = g_proc_tail = g_current_proc;

    cpu_get_local()->kernel_stack = current_rsp;

    DEBUG_INFO("Scheduler Initialized.");
}

extern "C" void kernel_task_wrapper(void (*entry)())
{
    scheduler_unlock_after_switch();
    asm volatile("sti");
    if (entry)
        entry();
    process_exit(0);
}

extern "C" void kernel_thread_entry();

Process *scheduler_create_task(void (*entry)(), const char *name)
{
    const uint64_t flags = interrupts_save_disable();
    Process *proc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!proc) {
        DEBUG_ERROR("Failed to allocate process struct");
        interrupts_restore(flags);
        return nullptr;
    }

    kstring::zero_memory(proc, sizeof(Process));
    event_init(proc->event_queue);
    spinlock_acquire(&g_sched_lock);
    proc->pid = g_next_pid++;
    spinlock_release(&g_sched_lock);
    proc->uid = g_current_proc ? g_current_proc->uid : 0;
    proc->parent_pid = g_current_proc ? g_current_proc->pid : 0;
    if (name)
        kstring::strncpy(proc->name, name, 31);
    proc->state = ProcessState_Ready;
    proc->priority = 1;
    proc->time_slice = 0;
    proc->last_run_time = timer_get_ticks();
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    spinlock_init(&proc->fd_lock);
    spinlock_init(&proc->vma_lock);
    proc->vma_lock_ptr = &proc->vma_lock;

    for (auto &fd : proc->fd_table)
        fd.used = false;
    proc->fd_table[0].used = proc->fd_table[1].used = proc->fd_table[2].used = true;

    init_fpu_state(proc->fpu_state);
    proc->fpu_initialized = true;

    const size_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void *frames = pmm_alloc_frames(stack_pages);
    if (!frames) {
        aligned_free(proc);
        interrupts_restore(flags);
        return nullptr;
    }

    proc->stack_phys = reinterpret_cast<uint64_t>(frames);
    uint64_t virt_base = vmm_phys_to_virt(proc->stack_phys);
    proc->stack_base = reinterpret_cast<uint64_t *>(virt_base);

    for (size_t i = 0; i < KERNEL_STACK_SIZE / sizeof(uint64_t); i++)
        proc->stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    uint64_t *stack_top = reinterpret_cast<uint64_t *>(virt_base + KERNEL_STACK_SIZE);

    *(--stack_top) = reinterpret_cast<uint64_t>(kernel_thread_entry);
    *(--stack_top) = reinterpret_cast<uint64_t>(entry);
    *(--stack_top) = 0;
    *(--stack_top) = 0;
    *(--stack_top) = 0;
    *(--stack_top) = 0;
    *(--stack_top) = 0;

    proc->sp = reinterpret_cast<uint64_t>(stack_top);

    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = proc;
    proc->next = g_proc_list;
    g_proc_tail = proc;

    proc->children_list = nullptr;
    if (g_current_proc) {
        proc->sibling_next = g_current_proc->children_list;
        g_current_proc->children_list = proc;
    } else {
        proc->sibling_next = nullptr;
    }

    ready_queue_push(proc);
    spinlock_release(&g_sched_lock);

    interrupts_restore(flags);
    return proc;
}

void scheduler_notify_input_waiters()
{
    // Wake processes waiting for input events to minimize latency.
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    Process *p = g_proc_list;
    if (p) {
        do {
            wait_queue_wake_all(&p->event_wait_queue);
            p = p->next;
        } while (p && p != g_proc_list);
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

extern "C" uint64_t g_kernel_scratch_rsp;

void scheduler_schedule()
{
    if (!g_current_proc)
        return;
    if (g_current_proc->stack_base) {
        for (int s = 0; s < 8; s++) {
            if (g_current_proc->stack_base[s] != 0xDEADBEEFDEADBEEFULL)
                panic("Stack overflow detected!");
        }
    }

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    scheduler_schedule_internal();
    interrupts_restore(flags);
    reap_kernel_zombies();
}

void scheduler_yield()
{
    scheduler_schedule();
}

extern "C" void save_fpu_state(uint8_t *fpu_buffer);

[[nodiscard]] uint64_t process_fork(SyscallFrame *frame)
{
    Process *child = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!child)
        return static_cast<uint64_t>(-1);
    kstring::zero_memory(child, sizeof(Process));
    event_init(child->event_queue);

    // Allocate a PID under the scheduler lock without allowing timer-driven
    // preemption to interleave with scheduler state updates.
    uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    child->pid = g_next_pid++;
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    child->parent_pid = g_current_proc->pid;
    child->uid = g_current_proc->uid;
    child->state = ProcessState_Ready;
    child->priority = g_current_proc->priority;
    child->time_slice = 0;
    child->last_run_time = timer_get_ticks();

    save_fpu_state(g_current_proc->fpu_state);
    kstring::memcpy(child->fpu_state, g_current_proc->fpu_state, FPU_STATE_SIZE);
    child->fpu_initialized = true;
    spinlock_init(&child->fd_lock);

    spinlock_acquire(&g_current_proc->fd_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child->fd_table[i] = g_current_proc->fd_table[i];
        if (child->fd_table[i].used && child->fd_table[i].vnode) {
            __sync_fetch_and_add(&child->fd_table[i].vnode->ref_count, 1);
        }
    }
    spinlock_release(&g_current_proc->fd_lock);
    child->cursor_x = g_current_proc->cursor_x;
    child->cursor_y = g_current_proc->cursor_y;
    child->page_table = vmm_clone_address_space(g_current_proc->page_table);
    if (!child->page_table) {
        process_release_private_fds(child);
        aligned_free(child);
        return static_cast<uint64_t>(-1);
    }

    asm volatile("mov %%cr3, %%rax\n\tmov %%rax, %%cr3" ::: "rax", "memory");

    spinlock_init(&child->vma_lock);
    child->vma_lock_ptr = &child->vma_lock;
    spinlock_acquire(&g_current_proc->vma_lock);
    child->vma_list = vma_clone(g_current_proc->vma_list);
    spinlock_release(&g_current_proc->vma_lock);

    if (g_current_proc->vma_list && !child->vma_list) {
        process_release_private_fds(child);
        vmm_free_address_space(child->page_table);
        aligned_free(child);
        return static_cast<uint64_t>(-1);
    }

    const size_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void *stack_phys = pmm_alloc_frames(stack_pages);
    if (!stack_phys) {
        process_release_private_fds(child);
        if (child->vma_list)
            vma_free_all(child->vma_list);
        vmm_free_address_space(child->page_table);
        aligned_free(child);
        return static_cast<uint64_t>(-1);
    }
    child->stack_phys = reinterpret_cast<uint64_t>(stack_phys);

    uint64_t virt_base = vmm_phys_to_virt(child->stack_phys);
    child->stack_base = reinterpret_cast<uint64_t *>(virt_base);

    uint64_t *hhdm_stack_base = reinterpret_cast<uint64_t *>(virt_base);
    for (size_t i = 0; i < 8; i++)
        hhdm_stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    uint64_t stack_top_hhdm = virt_base + KERNEL_STACK_SIZE;

    const size_t total_frame_size = sizeof(SyscallFrame) + sizeof(Context);
    stack_top_hhdm -= total_frame_size;
    stack_top_hhdm &= ~0xFULL;

    SyscallFrame *child_frame = reinterpret_cast<SyscallFrame *>(stack_top_hhdm + sizeof(Context));
    Context *child_context = reinterpret_cast<Context *>(stack_top_hhdm);

    *child_frame = *frame;
    kstring::zero_memory(child_context, sizeof(Context));
    child_context->rip = reinterpret_cast<uint64_t>(fork_ret);
    child->sp = stack_top_hhdm;

    // Link the child into the process list and ready queue atomically with
    // respect to timer interrupts and scheduler activity.
    flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = child;
    g_proc_tail = child;
    child->next = g_proc_list;

    child->children_list = nullptr;
    child->sibling_next = g_current_proc->children_list;
    g_current_proc->children_list = child;

    ready_queue_push(child);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    return child->pid;
}

void process_exit(int32_t status)
{
    DEBUG_INFO("Process %d exiting with status %d", g_current_proc->pid, status);

    spinlock_acquire(&g_current_proc->fd_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_current_proc->fd_table[i].used) {
            FileDescriptor *f = &g_current_proc->fd_table[i];
            VNode *node = f->vnode;
            f->used = false;
            f->vnode = nullptr;
            if (node) {
                spinlock_release(&g_current_proc->fd_lock);
                vfs_close_vnode(node);
                spinlock_acquire(&g_current_proc->fd_lock);
            }
        }
    }
    spinlock_release(&g_current_proc->fd_lock);

    shm_cleanup_process(g_current_proc);

    const uint64_t flags = interrupts_save_disable();
    (void)flags;
    spinlock_acquire(&g_sched_lock);

    g_current_proc->state = ProcessState_Zombie;
    g_current_proc->exit_status = status;

    if (g_current_proc->children_list) {
        Process *init = nullptr;
        Process *p = g_proc_list;
        if (p) {
            do {
                if (p->pid == 1) {
                    init = p;
                    break;
                }
                p = p->next;
            } while (p != g_proc_list);
        }

        if (init) {
            Process *last = g_current_proc->children_list;
            while (true) {
                last->parent_pid = 1;
                if (!last->sibling_next)
                    break;
                last = last->sibling_next;
            }
            last->sibling_next = init->children_list;
            init->children_list = g_current_proc->children_list;
        }
        g_current_proc->children_list = nullptr;
    }

    wait_queue_wake_all(&g_current_proc->wait_queue);

    Process *parent = nullptr;
    Process *found_p = g_proc_list;
    if (found_p) {
        do {
            if (found_p->pid == g_current_proc->parent_pid) {
                parent = found_p;
                break;
            }
            found_p = found_p->next;
        } while (found_p != g_proc_list);
    }

    if (parent) {
        // Reverted to accurate wildcard (0) logic. Step 1 `wake_all` handles the strict PID wakes
        if (parent->state == ProcessState_Waiting && parent->wait_for_pid == 0) {
            scheduler_wake_process_locked(parent);
        }
        parent->exec_done = true;
        parent->exec_exit_status = status;
    }

    vmm_switch_address_space(
        reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));

    scheduler_schedule_internal();
    for (;;)
        ;
}

[[nodiscard]] int64_t process_waitpid(int64_t pid, int32_t *status, int options)
{
    const bool nohang = (options & WNOHANG) != 0;
    while (true) {
        const uint64_t flags = interrupts_save_disable();
        spinlock_acquire(&g_sched_lock);

        Process *target = nullptr;
        Process *prev_sibling = nullptr;
        Process *p = g_current_proc->children_list;

        while (p) {
            if (p->state == ProcessState_Zombie) {
                if (pid == -1 || static_cast<uint64_t>(pid) == p->pid) {
                    target = p;
                    break;
                }
            }
            prev_sibling = p;
            p = p->sibling_next;
        }

        if (target) {
            if (status)
                *status = target->exit_status;
            const uint64_t child_pid = target->pid;

            if (prev_sibling)
                prev_sibling->sibling_next = target->sibling_next;
            else
                g_current_proc->children_list = target->sibling_next;

            Process *prev = g_proc_list;
            while (prev->next != target && prev->next != g_proc_list)
                prev = prev->next;
            if (prev->next == target) {
                prev->next = target->next;
                if (g_proc_list == target)
                    g_proc_list = target->next;
                if (g_proc_tail == target)
                    g_proc_tail = prev;
            }

            spinlock_release(&g_sched_lock);
            interrupts_restore(flags);

            process_free_reaped(target);
            DEBUG_INFO("Reaped zombie PID %d", child_pid);
            return static_cast<int64_t>(child_pid);
        }

        if (nohang) {
            if (pid != -1) {
                Process *child = nullptr;
                Process *c = g_current_proc->children_list;
                while (c) {
                    if (c->pid == static_cast<uint64_t>(pid)) {
                        child = c;
                        break;
                    }
                    c = c->sibling_next;
                }
                if (!child) {
                    spinlock_release(&g_sched_lock);
                    interrupts_restore(flags);
                    return -1;
                }
            }
            spinlock_release(&g_sched_lock);
            interrupts_restore(flags);
            return 0;
        }

        // Reverted to 0 for wildcard representation as initially architected
        if (pid == -1) {
            g_current_proc->state = ProcessState_Waiting;
            g_current_proc->wait_for_pid = 0;
        } else {
            Process *child = nullptr;
            Process *c = g_current_proc->children_list;
            while (c) {
                if (c->pid == static_cast<uint64_t>(pid)) {
                    child = c;
                    break;
                }
                c = c->sibling_next;
            }

            if (!child) {
                spinlock_release(&g_sched_lock);
                interrupts_restore(flags);
                return -1;
            }
            wait_queue_push(&child->wait_queue, g_current_proc);
        }

        scheduler_schedule_internal();
        interrupts_restore(flags);
    }
}

void scheduler_sleep(uint64_t ticks)
{
    if (!g_current_proc)
        return;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    sleep_queue_push(g_current_proc, ticks);

    spinlock_release(&g_sched_lock);
    scheduler_schedule();
    interrupts_restore(flags);
}

void scheduler_sleep_ms(uint64_t ms)
{
    if (ms == 0)
        return;

    // Ceil(ms * freq / 1000) without overflowing for very large values.
    const uint32_t freq = timer_get_frequency();
    if (freq == 0) {
        timer_poll_wait_ms(static_cast<uint32_t>(ms > UINT32_MAX ? UINT32_MAX : ms));
        return;
    }

    uint64_t whole = ms / 1000u;
    uint64_t rem = ms % 1000u;
    uint64_t ticks = whole * static_cast<uint64_t>(freq);
    uint64_t rem_ticks = (rem * static_cast<uint64_t>(freq) + 999u) / 1000u;
    if (UINT64_MAX - ticks < rem_ticks)
        ticks = UINT64_MAX;
    else
        ticks += rem_ticks;
    if (ticks == 0)
        ticks = 1;

    scheduler_sleep(ticks);
}

extern "C" void thread_ret();

[[nodiscard]] int64_t sys_thread_create(void (*entry)(), void *arg, void *stack_top, SyscallFrame *frame)
{
    if (!entry || !stack_top || !frame) {
        return -22; // -EINVAL
    }

    Process *parent = process_get_current();
    if (!parent) {
        return -1;
    }

    Process *thread = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!thread) {
        return -1;
    }
    kstring::zero_memory(thread, sizeof(Process));
    event_init(thread->event_queue);

    uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    thread->pid = g_next_pid++;
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    thread->parent_pid = parent->pid;
    thread->uid = parent->uid;
    thread->state = ProcessState_Ready;
    thread->priority = parent->priority;
    thread->time_slice = 0;
    thread->last_run_time = timer_get_ticks();

    save_fpu_state(parent->fpu_state);
    kstring::memcpy(thread->fpu_state, parent->fpu_state, FPU_STATE_SIZE);
    thread->fpu_initialized = true;

    // copy fd table, bump vnode refcounts
    spinlock_init(&thread->fd_lock);
    spinlock_acquire(&parent->fd_lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        thread->fd_table[i] = parent->fd_table[i];
        if (thread->fd_table[i].used && thread->fd_table[i].vnode) {
            __sync_fetch_and_add(&thread->fd_table[i].vnode->ref_count, 1);
        }
    }
    spinlock_release(&parent->fd_lock);

    thread->cursor_x = parent->cursor_x;
    thread->cursor_y = parent->cursor_y;
    kstring::strncpy(thread->cwd, parent->cwd, sizeof(thread->cwd));


    thread->page_table = parent->page_table;
    thread->vma_list = parent->vma_list;
    spinlock_init(&thread->vma_lock);
    thread->vma_lock_ptr = parent->vma_lock_ptr; // Share parent's VMA lock


    const size_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void *kstack_phys = pmm_alloc_frames(stack_pages);
    if (!kstack_phys) {
        process_release_private_fds(thread);
        aligned_free(thread);
        return -1;
    }
    thread->stack_phys = reinterpret_cast<uint64_t>(kstack_phys);
    uint64_t virt_base = vmm_phys_to_virt(thread->stack_phys);
    thread->stack_base = reinterpret_cast<uint64_t *>(virt_base);

    uint64_t *hhdm_stack_base = reinterpret_cast<uint64_t *>(virt_base);
    for (size_t i = 0; i < 8; i++)
        hhdm_stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    uint64_t stack_top_hhdm = virt_base + KERNEL_STACK_SIZE;
    const size_t total_frame_size = sizeof(SyscallFrame) + sizeof(Context);
    stack_top_hhdm -= total_frame_size;
    stack_top_hhdm &= ~0xFULL;

    SyscallFrame *child_frame = reinterpret_cast<SyscallFrame *>(stack_top_hhdm + sizeof(Context));
    Context *child_context = reinterpret_cast<Context *>(stack_top_hhdm);

    kstring::zero_memory(child_frame, sizeof(SyscallFrame));
    child_frame->rip = reinterpret_cast<uint64_t>(entry);
    child_frame->rsp = reinterpret_cast<uint64_t>(stack_top);
    child_frame->cs = frame->cs;
    child_frame->ss = frame->ss;
    child_frame->rflags = frame->rflags;
    child_frame->arg6 = reinterpret_cast<uint64_t>(arg); // r9 -> rdi via thread_ret

    kstring::zero_memory(child_context, sizeof(Context));
    child_context->rip = reinterpret_cast<uint64_t>(thread_ret);
    thread->sp = stack_top_hhdm;

    kstring::strncpy(thread->name, parent->name, 24);
    kstring::strncat(thread->name, "/thr", 7);

    flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = thread;
    g_proc_tail = thread;
    thread->next = g_proc_list;

    thread->children_list = nullptr;
    thread->sibling_next = parent->children_list;
    parent->children_list = thread;

    ready_queue_push(thread);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    return static_cast<int64_t>(thread->pid);
}

void preempt_disable()
{
    const uint64_t flags = interrupts_save_disable();
    Process *curr = process_get_current();
    if (curr) {
        curr->preempt_count++;
    }
    interrupts_restore(flags);
}

void preempt_enable()
{
    const uint64_t flags = interrupts_save_disable();
    Process *curr = process_get_current();
    bool should_yield = false;
    if (curr) {
        if (curr->preempt_count > 0) {
            curr->preempt_count--;
        }
        if (curr->preempt_count == 0 && curr->preempt_pending) {
            curr->preempt_pending = 0;
            should_yield = true;
        }
    }
    interrupts_restore(flags);

    if (should_yield) {
        scheduler_yield();
    }
}

