#include <drivers/acpi/acpi.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/fs/vfs.h>
#include <kernel/irq.h>
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
#include <uapi/syscalls_ext.h>

extern "C" void load_idt(void *);
extern "C" void init_fpu_state(uint8_t *fpu_buffer);
extern "C" void fork_ret();
extern uint64_t gui_get_wm_pid();
extern uint64_t gui_get_focus_pid();

static Spinlock g_sched_lock = SPINLOCK_INIT;
// Current task is per-core state, reached through this core's PerCpu block.
static Process *current_proc()
{
    return cpu_get_local()->current;
}

static void set_current_proc(Process *p)
{
    cpu_get_local()->current = p;
}
static Process *g_proc_list = nullptr;
static Process *g_proc_tail = nullptr;
static uint64_t g_next_pid = 1;
static volatile uint32_t g_shutdown_action = 0;
WaitQueue g_epoll_wait_queue = {nullptr, nullptr};
// Earliest absolute tick at which some epoll waiter's timeout expires,
// UINT64_MAX when none is armed. Updated lock-free (monotonic min); fired
// from the tick path under g_sched_lock.
static volatile uint64_t g_epoll_wake_deadline = UINT64_MAX;

// Timed waits on leaf wait queues (futex timeouts): unlike epoll's single
// global queue there is no one queue to wake, so each waiter registers
// itself. The walker wakes entries whose deadline passed AND that are still
// parked (Blocked/Waiting): a waiter woken by its leaf (futex WAKE) is
// already Running/Ready and is skipped, so a genuine wake is never misread
// as a timeout.
struct TimedWaitEntry
{
    Process *proc;
    uint64_t deadline;
};
static TimedWaitEntry g_timed_waits[16];
static volatile uint64_t g_timed_wake_deadline = UINT64_MAX;
static void wake_expired_timed_waits(uint64_t now);

extern "C" void scheduler_unlock_after_switch();

// Zombies whose resources are still shared with live threads cannot be freed
// the moment they are reaped. They are parked here and retried on every
// kernel-zombie reap pass until the last sharer is gone. queue_next links.
static Process *g_deferred_frees = nullptr;

#ifdef DEBUG
static constexpr uint64_t k_proc_canary = 0x5AFECA7A11CED0ULL;

static inline void proc_canary_stamp(Process *p)
{
    if (p)
        p->debug_canary = k_proc_canary;
}

// DEBUG-only integrity guard: verifies the circular process list is intact
// and every node's canary is alive. A failure names the corrupted process
// (pid/name) so the overwriting path can be identified. Cheap: the list is
// short and this runs at list mutation points.
static void proc_list_check_locked(const char *where)
{
    if (!g_proc_list)
        return;
    Process *p = g_proc_list;
    for (int guard = 0; guard < 4096; guard++) {
        if (p->debug_canary != k_proc_canary) {
            KLOG(LogModule::Sched, LogLevel::Fatal, "%s: canary corrupt on pid %llu (%s)", where,
                 (unsigned long long)p->pid, p->name);
            panic("process struct corrupted (details in log)");
        }
        Process *nxt = p->next;
        if (!nxt) {
            KLOG(LogModule::Sched, LogLevel::Fatal, "%s: NULL next after pid %llu (%s)", where,
                 (unsigned long long)p->pid, p->name);
            panic("process list broken (details in log)");
        }
        p = nxt;
        if (p == g_proc_list)
            return;
    }
    panic("process list not circular");
}
#else
static inline void proc_canary_stamp(Process *)
{
}
static inline void proc_list_check_locked(const char *)
{
}
#endif

// Returns true when `target` was fully destroyed, false when it had to be
// deferred because live threads still share its address space or its
// embedded vma lock.
static bool process_free_reaped(Process *target)
{
    if (!target)
        return true;

    // Release any fd-table reference still attached: normal exits already
    // dropped it in process_release_private_fds (fdtab is null), but
    // manually-zombified tasks (ktest surgery) reach the reaper with the
    // reference still held. The table is independently refcounted, so this
    // is safe to do before the shared-address-space scan.
    if (target->fdtab) {
        fd_table_release(target->fdtab);
        target->fdtab = nullptr;
    }

    bool share_page_table = false;
    bool share_vma_list = false;
    bool share_vma_lock = false;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    proc_list_check_locked("free_reaped");

    Process *curr = g_proc_list;
    if (curr) {
        do {
            if (curr != target) {
                if (curr->page_table == target->page_table)
                    share_page_table = true;
                if (curr->vmalist == target->vmalist)
                    share_vma_list = true;
                // Threads lock VMAs through the leader's EMBEDDED spinlock;
                // freeing the struct while they do is a use-after-free of
                // the lock itself.
                if (curr->vma_lock_ptr == &target->vma_lock)
                    share_vma_lock = true;
                if (share_page_table && share_vma_list && share_vma_lock)
                    break;
            }
            curr = curr->next;
        } while (curr != g_proc_list);
    }

    if (share_page_table || share_vma_list || share_vma_lock) {
        target->queue_next = g_deferred_frees;
        g_deferred_frees = target;
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);
        return false;
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    if (target->stack_phys) {
        uintptr_t stack_ptr = target->stack_phys;
        size_t num_pages = KERNEL_STACK_SIZE / 4096;

        for (size_t i = 0; i < num_pages; i++) {
            pmm_free_frame(reinterpret_cast<void *>(stack_ptr));
            stack_ptr += 4096; // Move to the next page frame
        }
    }

    if (target->page_table)
        vmm_free_address_space(target->page_table);
    if (target->vmalist) {
        vma_free_all(target->vmalist->head);
        vma_list_free(target->vmalist);
        target->vmalist = nullptr;
    }

    aligned_free(target);
    return true;
}

static void retry_deferred_frees()
{
    while (true) {
        const uint64_t flags = interrupts_save_disable();
        spinlock_acquire(&g_sched_lock);
        proc_list_check_locked("retry_deferred");
        Process *target = g_deferred_frees;
        if (!target) {
            spinlock_release(&g_sched_lock);
            interrupts_restore(flags);
            return;
        }
        g_deferred_frees = target->queue_next;
        target->queue_next = nullptr;
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);

        if (!process_free_reaped(target))
            return; // still shared; re-queued — try the rest next pass
    }
}

static Process *detach_kernel_zombie_locked()
{
    if (!g_proc_list)
        return nullptr;

    Process *target = nullptr;
    Process *prev = g_proc_tail;
    Process *p = g_proc_list;
    do {
        if (p != current_proc() && p->pid != 0 && p->parent_pid == 0 && p->state == ProcessState_Zombie) {
            target = p;
            break;
        }
        prev = p;
        p = p->next;
    } while (p != g_proc_list);

    if (!target)
        return nullptr;

    Process *kernel = g_proc_list;
    if (kernel) {
        do {
            if (kernel->pid == 0)
                break;
            kernel = kernel->next;
        } while (kernel != g_proc_list);
    }
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
    proc_list_check_locked("detach_zombie");
    return target;
}

static void reap_kernel_zombies()
{
    retry_deferred_frees();
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

VmaList *vma_list_alloc()
{
    auto *l = static_cast<VmaList *>(aligned_alloc(64, sizeof(VmaList)));
    if (!l)
        return nullptr;
    l->head = nullptr;
    l->count = 0;
    return l;
}

void vma_list_free(VmaList *list)
{
    if (list)
        aligned_free(list);
}

FdTable *fd_table_alloc(bool stdio_marks)
{
    auto *t = static_cast<FdTable *>(aligned_alloc(64, sizeof(FdTable)));
    if (!t)
        return nullptr;
    kstring::zero_memory(t, sizeof(FdTable));
    spinlock_init(&t->lock);
    t->refs = 1;
    if (stdio_marks) {
        // Slots 0/1/2 are the stdin/stdout/stderr placeholders handled by
        // the sys_read/sys_write special cases.
        t->fds[0].used = true;
        t->fds[1].used = true;
        t->fds[2].used = true;
    }
    return t;
}

FdTable *fd_table_copy(FdTable *src)
{
    if (!src)
        return nullptr;
    auto *t = fd_table_alloc(false);
    if (!t)
        return nullptr;

    // irqsave: the table lock is an IRQ-touched leaf, and a raw acquire here
    // let the resched IPI (and any future IRQ path taking it) preempt
    // mid-copy.
    uint64_t flags = spinlock_acquire_irqsave(&src->lock);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        t->fds[i] = src->fds[i];
        if (t->fds[i].used && t->fds[i].vnode)
            __sync_fetch_and_add(&t->fds[i].vnode->ref_count, 1);
    }
    spinlock_release_irqrestore(&src->lock, flags);
    return t;
}

FdTable *fd_table_share(FdTable *t)
{
    if (!t)
        return nullptr;
    uint64_t flags = spinlock_acquire_irqsave(&t->lock);
    t->refs++;
    spinlock_release_irqrestore(&t->lock, flags);
    return t;
}

void fd_table_release(FdTable *t)
{
    if (!t)
        return;

    // A concurrent sys_fd_transfer from another process may still be
    // installing fds into this table while it dies; detach under the table
    // lock and drop the vnode references outside of it (fs close can do
    // real work).
    VNode *nodes[MAX_OPEN_FILES];
    int node_count = 0;
    bool final = false;

    uint64_t flags = spinlock_acquire_irqsave(&t->lock);
    if (t->refs > 0)
        t->refs--;
    final = (t->refs == 0);
    if (final) {
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            if (!t->fds[i].used)
                continue;
            if (t->fds[i].vnode && node_count < MAX_OPEN_FILES)
                nodes[node_count++] = t->fds[i].vnode;
            t->fds[i].used = false;
            t->fds[i].vnode = nullptr;
        }
    }
    spinlock_release_irqrestore(&t->lock, flags);

    if (!final)
        return;

    for (int i = 0; i < node_count; i++)
        vfs_close_vnode(nodes[i]);
    aligned_free(t);
}

static void process_release_private_fds(Process *proc)
{
    if (!proc)
        return;
    FdTable *t = proc->fdtab;
    proc->fdtab = nullptr;
    fd_table_release(t);
}

#define NUM_PRIORITY_LEVELS 3
static Process *g_ready_queues[NUM_PRIORITY_LEVELS] = {nullptr};
static Process *g_ready_tails[NUM_PRIORITY_LEVELS] = {nullptr};
static Process *g_sleep_queue = nullptr;
static uint64_t g_last_sleep_tick = 0;

static void interactive_boost_if_needed(Process *p)
{
    if (p && p->pid != 0) {
        uint64_t wm = gui_get_wm_pid();
        uint64_t focus = gui_get_focus_pid();
        if ((wm != 0 && (p->pid == wm || p->parent_pid == wm)) ||
            (focus != 0 && (p->pid == focus || p->parent_pid == focus))) {
            p->priority = 0;
        }
    }
}

// Ready-queue primitives operate on an explicit (head, tail) pair so they can
// be exercised on isolated state by scheduler_ready_queue_self_test().
static void ready_queue_push_entry(Process **head, Process **tail, Process *p)
{
    if (!p || p->in_ready_queue || p->on_cpu)
        return;
    p->queue_next = nullptr;
    p->in_ready_queue = true;
    if (!*tail) {
        *head = *tail = p;
    } else {
        (*tail)->queue_next = p;
        *tail = p;
    }
}

static Process *ready_queue_pop_entry(Process **head, Process **tail)
{
    Process *p = *head;
    if (!p)
        return nullptr;
    *head = p->queue_next;
    if (!*head)
        *tail = nullptr;
    p->queue_next = nullptr;
    p->in_ready_queue = false;
    return p;
}

static bool ready_queue_remove_entry(Process **head, Process **tail, Process *p)
{
    Process *prev = nullptr;
    for (Process *curr = *head; curr; prev = curr, curr = curr->queue_next) {
        if (curr != p)
            continue;
        if (prev)
            prev->queue_next = curr->queue_next;
        else
            *head = curr->queue_next;
        if (*tail == curr)
            *tail = prev;
        curr->queue_next = nullptr;
        curr->in_ready_queue = false;
        return true;
    }
    return false;
}

static void ready_queue_push(Process *p)
{
    if (!p)
        return;
    uint8_t prio = p->priority;
    if (prio >= NUM_PRIORITY_LEVELS)
        prio = NUM_PRIORITY_LEVELS - 1;
    ready_queue_push_entry(&g_ready_queues[prio], &g_ready_tails[prio], p);
}

static Process *ready_queue_pop()
{
    for (int i = 0; i < NUM_PRIORITY_LEVELS; i++) {
        if (Process *p = ready_queue_pop_entry(&g_ready_queues[i], &g_ready_tails[i]))
            return p;
    }
    return nullptr;
}

static void scheduler_remove_from_ready_queue_locked(Process *p)
{
    for (int i = 0; i < NUM_PRIORITY_LEVELS; i++) {
        if (ready_queue_remove_entry(&g_ready_queues[i], &g_ready_tails[i], p))
            return;
    }
}

void scheduler_remove_from_ready_queue(Process *p)
{
    if (!p)
        return;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    scheduler_remove_from_ready_queue_locked(p);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

// ktest hook: exercises the ready-queue push/pop/remove primitives on
// isolated synthetic state (never touches the live run queues). Returns false
// on any invariant breach.
bool scheduler_ready_queue_self_test()
{
    static Process fake_a;
    static Process fake_b;
    static Process fake_c;
    kstring::zero_memory(&fake_a, sizeof(Process));
    kstring::zero_memory(&fake_b, sizeof(Process));
    kstring::zero_memory(&fake_c, sizeof(Process));

    Process *head = nullptr;
    Process *tail = nullptr;
    bool ok = true;

    ready_queue_push_entry(&head, &tail, &fake_a);
    ok = ok && fake_a.in_ready_queue && !fake_a.on_cpu;
    ok = ok && head == &fake_a && tail == &fake_a;

    ready_queue_push_entry(&head, &tail, &fake_a); // guard: already queued, must not re-link
    ok = ok && head == &fake_a && tail == &fake_a && fake_a.queue_next == nullptr;

    ready_queue_push_entry(&head, &tail, &fake_b);
    ok = ok && head == &fake_a && tail == &fake_b && fake_a.queue_next == &fake_b && fake_b.queue_next == nullptr;

    Process *popped = ready_queue_pop_entry(&head, &tail); // FIFO order
    ok = ok && popped == &fake_a && !fake_a.in_ready_queue;
    ok = ok && head == &fake_b && tail == &fake_b;

    ok = ok && ready_queue_remove_entry(&head, &tail, &fake_b); // sole entry
    ok = ok && !fake_b.in_ready_queue && head == nullptr && tail == nullptr;
    ok = ok && ready_queue_pop_entry(&head, &tail) == nullptr;
    ok = ok && !ready_queue_remove_entry(&head, &tail, &fake_a); // absent: no-op

    ready_queue_push_entry(&head, &tail, &fake_a);
    ready_queue_push_entry(&head, &tail, &fake_b);
    ready_queue_push_entry(&head, &tail, &fake_c);
    ok = ok && ready_queue_remove_entry(&head, &tail, &fake_b); // middle entry
    ok = ok && head == &fake_a && tail == &fake_c && fake_a.queue_next == &fake_c && fake_c.queue_next == nullptr;
    ok = ok && !fake_b.in_ready_queue;

    ok = ok && ready_queue_pop_entry(&head, &tail) == &fake_a; // head removal
    ok = ok && ready_queue_pop_entry(&head, &tail) == &fake_c; // tail removal empties queue
    ok = ok && head == nullptr && tail == nullptr && ready_queue_pop_entry(&head, &tail) == nullptr;

    fake_a.on_cpu = true;
    ready_queue_push_entry(&head, &tail, &fake_a); // guard: on-cpu tasks are never queued
    ok = ok && !fake_a.in_ready_queue && head == nullptr && tail == nullptr;
    fake_a.on_cpu = false;

    return ok;
}

static void scheduler_boost_process_priority_locked(Process *p, uint8_t new_priority)
{
    if (!p)
        return;
    if (p->priority > new_priority) {
        scheduler_remove_from_ready_queue_locked(p);
        p->priority = new_priority;
        // Restart the slice: the old slice was sized for the lower-priority
        // class (bigger max), which would otherwise trigger an immediate
        // demotion out of the freshly boosted priority.
        p->time_slice = 0;
        if (p->state == ProcessState_Ready) {
            ready_queue_push(p);
        }
    }
}

void scheduler_boost_process_priority(Process *p, uint8_t new_priority)
{
    if (!p)
        return;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    scheduler_boost_process_priority_locked(p, new_priority);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

// Variant for callers already holding the big lock.
void scheduler_boost_process_priority_under_lock(Process *p, uint8_t new_priority)
{
    scheduler_boost_process_priority_locked(p, new_priority);
}

static void sleep_queue_push(Process *p, uint64_t ticks)
{
    p->state = ProcessState_Sleeping;
    if (!g_sleep_queue) {
        p->wake_time = ticks;
        p->queue_next = nullptr;
        g_sleep_queue = p;
        g_last_sleep_tick = timer_get_ticks();
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

            interactive_boost_if_needed(p);

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
    // A process joining a wait queue is logically off-CPU from this moment,
    // even though it is still executing and will context-switch away inside
    // the scheduler call that follows. Wakes can run between the push and
    // that switch — including the sleeper's OWN schedule_internal (the epoll
    // deadline check runs there): with on_cpu still set, wait_queue_wake_all
    // flips the process to Ready while ready_queue_push skips it (on_cpu
    // guard), and because its state is no longer Running the scheduler's
    // own re-queue is skipped too. The switch then clears on_cpu and the
    // process sits in NO queue — lost forever, which froze whole user space
    // minutes after boot. Clearing on_cpu here keeps every wake path sound:
    // a wake either finds the process still running (next == cur, restores
    // on_cpu) or properly parked in the ready queue.
    p->on_cpu = false;
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
            interactive_boost_if_needed(p);
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
        interactive_boost_if_needed(p);
        ready_queue_push(p);
    }
}

// Wakes a signal's target regardless of where the scheduler parked it.
// Must be called with g_sched_lock held (callers do the find-and-signal
// atomically so the target cannot be reaped between lookup and wake).
void scheduler_wake_for_signal_locked(Process *p)
{
    if (!p)
        return;

    if (p->state == ProcessState_Sleeping) {
        // Unlink from the delta-encoded sleep queue, handing our remaining
        // time to the next sleeper.
        Process *prev = nullptr;
        Process *cur = g_sleep_queue;
        while (cur && cur != p) {
            prev = cur;
            cur = cur->queue_next;
        }
        if (cur) {
            if (prev)
                prev->queue_next = cur->queue_next;
            else
                g_sleep_queue = cur->queue_next;
            if (cur->queue_next)
                cur->queue_next->wake_time += cur->wake_time;
            cur->queue_next = nullptr;
            cur->state = ProcessState_Ready;
            interactive_boost_if_needed(cur);
            ready_queue_push(cur);
        }
        return;
    }

    scheduler_wake_process_locked(p);
}

// Finds a process while g_sched_lock is ALREADY held. The returned pointer
// is only valid under the lock; callers must finish their business with it
// before releasing.
Process *process_find_by_pid_locked(uint64_t pid)
{
    Process *p = g_proc_list;
    if (!p)
        return nullptr;
    do {
        if (p->pid == pid)
            return p;
        p = p->next;
    } while (p != g_proc_list);
    return nullptr;
}

static void scheduler_schedule_internal(uint32_t elapsed_jiffies = 1)
{
    Process *cur = current_proc();
    // This core's private idle task: never queued, never demoted.
    const bool cur_is_idle = (cur == cpu_get_local()->idle);

    if (elapsed_jiffies == 0)
        elapsed_jiffies = 1;
    cur->cpu_time += elapsed_jiffies;
    cur->time_slice += elapsed_jiffies;

    wake_sleeping_processes();

    const uint64_t now = timer_get_ticks();

    // Timed epoll waits: g_epoll_wait_queue has no native timeout, so the
    // tick path here is what enforces sys_epoll_wait's timeout argument.
    if (g_epoll_wait_queue.head && now >= g_epoll_wake_deadline) {
        g_epoll_wake_deadline = UINT64_MAX;
        wait_queue_wake_all(&g_epoll_wait_queue);
    }
    wake_expired_timed_waits(now);

    if (cur->state == ProcessState_Running) {
        uint32_t max_slice = (cur->priority == 0) ? 5 : (cur->priority == 1) ? 20 : 50;
        if (cur->time_slice >= max_slice) {
            if (cur->priority < NUM_PRIORITY_LEVELS - 1 && cur->pid != 0) {
                cur->priority++;
            }
            cur->time_slice = 0;
        }
        if (!cur_is_idle) {
            cur->on_cpu = false;
            cur->state = ProcessState_Ready;
            ready_queue_push(cur);
        }
        // An idle current simply stays where it is; the empty-queue path
        // below re-selects it without touching the shared queues.
    }

    Process *next = ready_queue_pop();
    if (!next) {
        if (!cur_is_idle && (cur->state == ProcessState_Running || cur->state == ProcessState_Ready)) {
            next = cur;
        } else {
            // Nothing runnable: park this core on its own idle task. Each
            // core has a private idle context so two cores never share one
            // Process struct. The BSP falls back to the boot-time kernel
            // task (pid 0) before its idle exists.
            next = cpu_get_local()->idle;
            if (!next) {
                Process *p = g_proc_list;
                while (p->pid != 0)
                    p = p->next;
                next = p;
            }
        }
    }

    if (next == cur) {
        cur->state = ProcessState_Running;
        cur->on_cpu = true;
        cur->last_run_time = now;
        spinlock_release_no_restore(&g_sched_lock);
        return;
    }

    Process *prev = current_proc();
    if (prev->state == ProcessState_Running)
        prev->state = ProcessState_Ready;
    prev->on_cpu = false;

    set_current_proc(next);
    next->on_cpu = true;
    current_proc()->state = ProcessState_Running;
    current_proc()->last_run_time = now;

    uint64_t next_rsp0;
    if (current_proc()->pid == 0) {
        next_rsp0 = current_proc()->sp;
    } else {
        next_rsp0 = reinterpret_cast<uint64_t>(current_proc()->stack_base) + KERNEL_STACK_SIZE;
    }

    tss_set_rsp0(next_rsp0);
    cpu_get_local()->kernel_stack = next_rsp0;

    auto get_cr3 = [](Process *p) -> uint64_t * {
        uint64_t pt = p->page_table ? reinterpret_cast<uint64_t>(p->page_table)
                                    : reinterpret_cast<uint64_t>(vmm_get_kernel_pml4());
        return reinterpret_cast<uint64_t *>(pt - vmm_get_hhdm_offset());
    };
    uint64_t *next_cr3 = get_cr3(current_proc());

    // Compare against the CR3 actually loaded on this core, not the previous
    // task's: process_exit() switches to the kernel page tables before
    // scheduling, and a sibling thread of the exiting task has the SAME
    // page_table — comparing prev vs next would skip the switch and return
    // the sibling to user mode on kernel mappings.
    if (cpu_get_local()->current_cr3_phys != reinterpret_cast<uint64_t>(next_cr3)) {
        vmm_switch_address_space(next_cr3);
    }

    switch_to_task(prev, current_proc());
    scheduler_unlock_after_switch();
}

extern "C" void scheduler_unlock_after_switch()
{
    spinlock_release_no_restore(&g_sched_lock);
}

// Wakes idle cores so they pull newly-ready work. Cheap no-op on UP.
void scheduler_notify_idle_cpus()
{
    if (__atomic_load_n(&g_cpu_online_count, __ATOMIC_ACQUIRE) > 1)
        apic_send_resched_ipi_to_others();
}

// Final AP handoff: adopt the per-core idle context and run the idle loop.
// Must be called ON the new core with the idle task created by the BSP.
// Never returns. Uses the standard switch_to_task path so the sched-lock
// release-after-switch contract holds exactly as for every other switch.
void scheduler_enter_idle(Process *idle)
{
    if (!idle) {
        asm volatile("1:\ncli\nhlt\njmp 1b\n");
    }

    // Throwaway 'previous context': switch_to_task saves into it and never
    // comes back. One per call site is safe — APs call this once.
    static Process bootstrap_prev;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    idle->state = ProcessState_Running;
    idle->on_cpu = true;
    idle->last_run_time = timer_get_ticks();
    set_current_proc(idle);
    tss_set_rsp0(idle->sp); // pid 0 tasks run on their saved sp
    cpu_get_local()->kernel_stack = idle->sp;
    cpu_get_local()->idle = idle;
    // Publish online only now: the LAPIC is enabled and this is the last stop
    // before the idle loop enables interrupts, so IPI/shootdown senders that
    // observe this flag can actually reach the core. Sync the shootdown
    // sequence first so this core is never asked to ack invalidations that
    // completed before it existed.
    vmm_tlb_mark_this_cpu_synced();
    __atomic_store_n(&cpu_get_local()->online, true, __ATOMIC_RELEASE);
    __sync_fetch_and_add(&g_cpu_online_count, 1);
    BOOT_SUCCESS("SMP: core %u online (%d CPUs total)", cpu_get_local()->cpu_id,
                 __atomic_load_n(&g_cpu_online_count, __ATOMIC_ACQUIRE));

    switch_to_task(&bootstrap_prev, idle);
}

void scheduler_wait(WaitQueue *q, Spinlock *lock)
{
    if (!current_proc() || !q)
        return;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    wait_queue_push(q, current_proc());

    if (lock)
        spinlock_release_no_restore(lock);

    scheduler_schedule_internal();

    // Re-acquire the leaf before restoring interrupts: it was released (raw,
    // with IF=0) inside the wait, so reacquiring under IF=0 keeps the lock
    // never-held-with-IF=1 until the caller's flags are back.
    if (lock)
        spinlock_acquire(lock);
    interrupts_restore(flags);
}

void scheduler_wait_rechecked(WaitQueue *q, Spinlock *lock, scheduler_wait_recheck_fn recheck, void *ctx)
{
    if (!current_proc() || !q)
        return;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    wait_queue_push(q, current_proc());

    if (lock)
        spinlock_release_no_restore(lock);

    if (recheck && recheck(ctx)) {
        // The condition turned true between the caller's last scan and this
        // push. Whoever set it finished their wake before we queued (both
        // sides serialize on g_sched_lock), so that wake saw an empty queue
        // and no future wake is coming — sleep would hang. Dequeue and
        // return; the caller's loop re-runs its locked scan.
        wait_queue_remove(q, current_proc());
        current_proc()->state = ProcessState_Running;
        current_proc()->on_cpu = true;
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);
        if (lock)
            spinlock_acquire(lock);
        return;
    }

    scheduler_schedule_internal();

    if (lock)
        spinlock_acquire(lock);
    interrupts_restore(flags);
}

int scheduler_wake_waiters_under_leaf(WaitQueue *q, uint32_t count)
{
    if (!q || !q->head)
        return 0;

    // Serialize the traversal against signal-driven wait_queue_remove,
    // which unlinks waiters under g_sched_lock alone: walking the list with
    // only the caller's leaf lock can follow a stale queue_next into a
    // reaped Process.
    spinlock_acquire(&g_sched_lock);
    int woken = 0;
    Process *curr = q->head;
    while (curr && (count == 0 || (uint32_t)woken < count)) {
        Process *next = curr->queue_next;
        scheduler_wake_process_locked(curr);
        woken++;
        curr = next;
    }
    spinlock_release(&g_sched_lock);
    return woken;
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
    scheduler_notify_idle_cpus();
}

// Same as scheduler_wake_all() but for callers that already hold the big
// lock (find-and-act sequences). Does not notify idle CPUs; the caller does
// that after releasing the lock.
void scheduler_wake_all_locked(WaitQueue *q)
{
    if (!q)
        return;
    wait_queue_wake_all(q);
    if (q != &g_epoll_wait_queue) {
        wait_queue_wake_all(&g_epoll_wait_queue);
    }
}

void scheduler_wake_one(WaitQueue *q)
{
    if (!q)
        return;

    bool woke = false;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    // Wake exactly the head waiter, if any. If the woken task loses the race
    // for the lock and re-enters scheduler_wait, it re-queues itself; the
    // next unlock will pick up the (possibly different) head. This mirrors
    // Linux's __mutex_wakeup and avoids the thundering-herd of wake_all on
    // contended mutexes while preserving the existing wait_queue semantics.
    Process *p = q->head;
    if (p) {
        // Detach the head from the wait queue.
        Process *next = p->queue_next;
        q->head = next;
        if (!next)
            q->tail = nullptr;
        p->queue_next = nullptr;
        p->waiting_queue = nullptr;

        if (p->state == ProcessState_Waiting || p->state == ProcessState_Blocked) {
            p->state = ProcessState_Ready;
            interactive_boost_if_needed(p);
            ready_queue_push(p);
            woke = true;
        }

        // Preserve the existing side-effect: a non-epoll wake also nudges the
        // global epoll wait queue. This is removed in the epoll refactor step.
        if (q != &g_epoll_wait_queue && g_epoll_wait_queue.head) {
            Process *e = g_epoll_wait_queue.head;
            g_epoll_wait_queue.head = e->queue_next;
            if (!g_epoll_wait_queue.head)
                g_epoll_wait_queue.tail = nullptr;
            e->queue_next = nullptr;
            e->waiting_queue = nullptr;
            if (e->state == ProcessState_Waiting || e->state == ProcessState_Blocked) {
                e->state = ProcessState_Ready;
                interactive_boost_if_needed(e);
                ready_queue_push(e);
                woke = true;
            }
        }
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
    if (woke)
        scheduler_notify_idle_cpus();
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
    scheduler_notify_idle_cpus();
}

// Fatal-signal escape hatch for kernel wait loops: a pending SIGKILL-class
// default-fatal signal should break an otherwise endless block instead of
// being delivered only when some unrelated event wakes the process.
bool scheduler_fatal_signal_pending(const Process *p)
{
    if (!p)
        return false;
    const uint64_t pending = __atomic_load_n(&p->signals.pending, __ATOMIC_ACQUIRE);
    for (int i = 1; i < 32; i++) {
        if (!(pending & (1ULL << i)))
            continue;
        if (p->signals.handlers[i] == SIG_DFL &&
            (i == SIGINT || i == SIGTERM || i == SIGQUIT || i == SIGKILL || i == SIGSEGV))
            return true;
    }
    return false;
}

uint64_t scheduler_big_lock_irqsave()
{
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    return flags;
}

void scheduler_big_unlock_irqrestore(uint64_t flags)
{
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
            if (p->pid > 1 && p != current_proc())
                p->state = ProcessState_Zombie;
            p = p->next;
        } while (p != g_proc_list);
    }
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

static void shutdown_prepare(uint32_t action, const char *message)
{
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_shutdown_action, &expected, action, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        DEBUG_WARN("Shutdown already in progress; ignoring duplicate request.");
        halt_forever();
    }

    DEBUG_INFO("%s", message);
    shutdown_mark_user_processes_zombie();
    vfs_sync();
    apic_stop_other_cpus();
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
    return current_proc();
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
    Process *kproc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!kproc)
        panic("Failed to allocate initial process!");

    kstring::zero_memory(kproc, sizeof(Process));
    event_init(kproc->event_queue);
    proc_canary_stamp(kproc);

    kproc->pid = 0;
    kproc->leader_pid = 0;
    kproc->uid = 0;
    kstring::strncpy(kproc->name, "Kernel", 31);
    kproc->state = ProcessState_Running;

    uint64_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));
    kproc->sp = current_rsp;
    kproc->stack_base = nullptr;

    kproc->priority = 2;
    kproc->fdtab = fd_table_alloc();
    kproc->vmalist = vma_list_alloc();

    init_fpu_state(kproc->fpu_state);
    kproc->fpu_initialized = true;
    kproc->children_list = nullptr;
    kproc->sibling_next = nullptr;
    kproc->next = kproc;
    g_proc_list = g_proc_tail = kproc;
    set_current_proc(kproc);
    kproc->on_cpu = true;

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
    proc_canary_stamp(proc);
    proc->pid = __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_SEQ_CST);
    proc->leader_pid = proc->pid;
    proc->uid = current_proc() ? current_proc()->uid : 0;
    proc->parent_pid = current_proc() ? current_proc()->pid : 0;
    if (name)
        kstring::strncpy(proc->name, name, 31);
    proc->state = ProcessState_Ready;
    proc->priority = 1;
    proc->time_slice = 0;
    proc->last_run_time = timer_get_ticks();
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->fdtab = fd_table_alloc();
    proc->vmalist = vma_list_alloc();
    spinlock_init(&proc->vma_lock);
    proc->vma_lock_ptr = &proc->vma_lock;

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

    for (size_t i = 0; i < 8; i++)
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
    proc_list_check_locked("enqueue");

    proc->children_list = nullptr;
    if (current_proc()) {
        proc->sibling_next = current_proc()->children_list;
        current_proc()->children_list = proc;
    } else {
        proc->sibling_next = nullptr;
    }

    ready_queue_push(proc);
    spinlock_release(&g_sched_lock);

    interrupts_restore(flags);
    scheduler_notify_idle_cpus();
    return proc;
}

// Creates a task WITHOUT queueing it. The caller MUST fill any Process fields
// the task needs (page_table, exec_entry, ...) and then publish it atomically
// via scheduler_enqueue_task(). This closes the race where another core pops
// the freshly queued task before its setup fields are written.
Process *scheduler_create_task_deferred(void (*entry)(), const char *name)
{
    const uint64_t flags = interrupts_save_disable();
    Process *proc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!proc) {
        interrupts_restore(flags);
        return nullptr;
    }

    kstring::zero_memory(proc, sizeof(Process));
    event_init(proc->event_queue);
    proc_canary_stamp(proc);
    proc->pid = __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_SEQ_CST);
    proc->leader_pid = proc->pid;
    proc->uid = current_proc() ? current_proc()->uid : 0;
    proc->parent_pid = current_proc() ? current_proc()->pid : 0;
    if (name)
        kstring::strncpy(proc->name, name, 31);
    proc->state = ProcessState_Ready;
    proc->priority = 1;
    proc->time_slice = 0;
    proc->last_run_time = timer_get_ticks();
    proc->cwd[0] = '/';
    proc->cwd[1] = '\0';
    proc->fdtab = fd_table_alloc();
    proc->vmalist = vma_list_alloc();
    spinlock_init(&proc->vma_lock);
    proc->vma_lock_ptr = &proc->vma_lock;

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

    for (size_t i = 0; i < 8; i++)
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
    interrupts_restore(flags);
    return proc;
}

// Publishes a deferred-created task onto the runqueue.
void scheduler_enqueue_task(Process *proc)
{
    if (!proc)
        return;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    // Insert into the process list.
    g_proc_tail->next = proc;
    proc->next = g_proc_list;
    g_proc_tail = proc;
    proc_list_check_locked("enqueue");

    proc->children_list = nullptr;
    if (current_proc()) {
        proc->sibling_next = current_proc()->children_list;
        current_proc()->children_list = proc;
    } else {
        proc->sibling_next = nullptr;
    }

    ready_queue_push(proc);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    scheduler_notify_idle_cpus();
}

// Creates a per-core idle task: pid 0, never queued on the global runqueue,
// invisible to the process list. Each core parks on its own instance when
// nothing is runnable.
Process *scheduler_create_idle_task(void (*entry)(), const char *name)
{
    const uint64_t flags = interrupts_save_disable();
    Process *proc = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!proc) {
        interrupts_restore(flags);
        return nullptr;
    }

    kstring::zero_memory(proc, sizeof(Process));
    event_init(proc->event_queue);
    proc_canary_stamp(proc);
    proc->pid = 0;
    proc->leader_pid = 0;
    proc->uid = 0;
    proc->parent_pid = 0;
    if (name)
        kstring::strncpy(proc->name, name, 31);
    proc->state = ProcessState_Ready;
    proc->priority = NUM_PRIORITY_LEVELS - 1; // IDLE class: never preempts real work
    proc->time_slice = 0;
    proc->fdtab = fd_table_alloc(false);
    proc->vmalist = vma_list_alloc();
    spinlock_init(&proc->vma_lock);
    proc->vma_lock_ptr = &proc->vma_lock;
    init_fpu_state(proc->fpu_state);
    proc->fpu_initialized = true;
    proc->next = proc;

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
    for (size_t i = 0; i < 8; i++)
        proc->stack_base[i] = 0xDEADBEEFDEADBEEFULL;

    // Same bootstrap frame as regular tasks: the first switch into the idle
    // task lands in kernel_thread_entry -> kernel_task_wrapper(entry); the
    // idle body never returns.
    uint64_t *stack_top = reinterpret_cast<uint64_t *>(virt_base + KERNEL_STACK_SIZE);
    *(--stack_top) = reinterpret_cast<uint64_t>(kernel_thread_entry);
    *(--stack_top) = reinterpret_cast<uint64_t>(entry);
    for (int i = 0; i < 5; i++)
        *(--stack_top) = 0;
    proc->sp = reinterpret_cast<uint64_t>(stack_top);

    interrupts_restore(flags);
    return proc;
}

void scheduler_note_epoll_deadline(uint64_t deadline_ticks)
{
    uint64_t cur = __atomic_load_n(&g_epoll_wake_deadline, __ATOMIC_RELAXED);
    while (deadline_ticks < cur) {
        if (__sync_bool_compare_and_swap(&g_epoll_wake_deadline, cur, deadline_ticks))
            return;
        cur = __atomic_load_n(&g_epoll_wake_deadline, __ATOMIC_RELAXED);
    }
}

bool scheduler_note_wake_deadline(Process *p, uint64_t deadline_ticks)
{
    if (!p)
        return true;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    bool placed = false;
    uint64_t earliest = UINT64_MAX;
    for (auto &e : g_timed_waits) {
        if (!placed && e.proc == nullptr) {
            e.proc = p;
            e.deadline = deadline_ticks;
            placed = true;
        }
        if (e.proc)
            earliest = (e.deadline < earliest) ? e.deadline : earliest;
    }
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);

    if (placed) {
        uint64_t cur = __atomic_load_n(&g_timed_wake_deadline, __ATOMIC_RELAXED);
        while (earliest < cur) {
            if (__sync_bool_compare_and_swap(&g_timed_wake_deadline, cur, earliest))
                break;
            cur = __atomic_load_n(&g_timed_wake_deadline, __ATOMIC_RELAXED);
        }
        return true;
    }

    // No free slot: report it so the caller can fail the wait instead of
    // silently degrading to an infinite sleep. Loud in debug as well.
    DEBUG_WARN("timed wait table full: pid %lu timeout dropped", p->pid);
    return false;
}

// Caller MUST hold g_sched_lock. Removing entries can only raise the
// table's true minimum, so the global deadline (<= that minimum by
// construction) stays valid: it may fire early and re-arm from the
// survivors.
static void clear_timed_wait_entries_locked(Process *p)
{
    for (auto &e : g_timed_waits) {
        if (e.proc == p)
            e.proc = nullptr;
    }
}

// A timed-wait registration must never outlive the wait that armed it:
// when the wait ends by wake, signal or expiry, drop the entry. A
// lingering entry makes the walker mark the process's next (unrelated)
// blocked wait as timed out, and a freed-then-recycled Process at the
// same address turns the liveness check into a false positive.
void scheduler_clear_wake_deadline(Process *p)
{
    if (!p)
        return;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    clear_timed_wait_entries_locked(p);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

// Caller MUST hold g_sched_lock: membership in the circular process list
// proves the struct is still live — reaping unlinks a zombie under this same
// lock before process_free_reaped can free the memory. A timed waiter that
// died or was reaped before its deadline leaves its registration behind, and
// dereferencing that stale pointer was a use-after-free.
static bool timed_wait_target_live(Process *target)
{
    Process *curr = g_proc_list;
    if (!curr)
        return false;
    do {
        if (curr == target)
            return true;
        curr = curr->next;
    } while (curr != g_proc_list);
    return false;
}

// Caller MUST hold g_sched_lock: this runs from scheduler_schedule_internal
// (the tick path that also drives the epoll deadline). The wake path is
// scheduler_wake_process_locked, which requires exactly that.
static void wake_expired_timed_waits(uint64_t now)
{
    if (g_timed_wake_deadline == UINT64_MAX || now < g_timed_wake_deadline)
        return;

    g_timed_wake_deadline = UINT64_MAX;
    uint64_t earliest = UINT64_MAX;
    bool unparked = false;
    for (auto &e : g_timed_waits) {
        if (!e.proc)
            continue;
        if (now < e.deadline) {
            earliest = (e.deadline < earliest) ? e.deadline : earliest;
            continue;
        }
        Process *target = e.proc;
        if (timed_wait_target_live(target)) {
            if (target->state == ProcessState_Blocked || target->state == ProcessState_Waiting) {
                e.proc = nullptr;
                target->timed_wake = true;
                scheduler_wake_process_locked(target);
            } else {
                // Live but not parked: either still between registration
                // and its scheduler_wait push, or already woken by its
                // futex wake and returning (it deregisters itself).
                // Dropping the entry here would lose a not-yet-parked
                // waiter's timeout — an unbounded hang. Keep it and look
                // again next tick.
                unparked = true;
            }
        } else {
            // The waiter died before its deadline; exit deregisters it,
            // so this is a backstop for a missed path.
            e.proc = nullptr;
        }
    }

    uint64_t rearm = earliest;
    if (unparked) {
        const uint64_t next_tick = now + 1;
        rearm = (next_tick < rearm) ? next_tick : rearm;
    }
    if (rearm != UINT64_MAX) {
        uint64_t cur = __atomic_load_n(&g_timed_wake_deadline, __ATOMIC_RELAXED);
        while (rearm < cur) {
            if (__sync_bool_compare_and_swap(&g_timed_wake_deadline, cur, rearm))
                break;
            cur = __atomic_load_n(&g_timed_wake_deadline, __ATOMIC_RELAXED);
        }
    }
}

void scheduler_notify_input_waiters()
{
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    // Input events also break epoll waits (its loop returns early when the
    // caller's own event queue is non-empty), so nudge the epoll queue like
    // scheduler_wake_all() does — otherwise an event that arrives via this
    // path never wakes a sleeping epoll waiter.
    if (g_epoll_wait_queue.head)
        wait_queue_wake_all(&g_epoll_wait_queue);

    Process *p = g_proc_list;
    if (p) {
        do {
            wait_queue_wake_all(&p->event_wait_queue);
            p = p->next;
        } while (p != g_proc_list);
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
    // Parked idle cores only learn about newly-ready work through the RESCHED
    // IPI; without it an input event waits up to a full tick for a core.
    scheduler_notify_idle_cpus();
}

extern "C" uint64_t g_kernel_scratch_rsp;

void scheduler_schedule_elapsed(uint32_t elapsed_jiffies)
{
    Process *current = current_proc();
    if (!current)
        return;
    if (current->stack_base) {
        if (current->stack_base[0] != 0xDEADBEEFDEADBEEFULL || current->stack_base[7] != 0xDEADBEEFDEADBEEFULL)
            panic("Stack overflow detected!");
    }

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    scheduler_schedule_internal(elapsed_jiffies);
    interrupts_restore(flags);
    reap_kernel_zombies();
}

void scheduler_schedule()
{
    scheduler_schedule_elapsed(1);
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
    proc_canary_stamp(child);

    child->pid = __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_SEQ_CST);
    child->leader_pid = child->pid;
    child->parent_pid = current_proc()->pid;
    child->uid = current_proc()->uid;
    child->state = ProcessState_Ready;
    child->priority = current_proc()->priority;
    child->time_slice = 0;
    child->last_run_time = timer_get_ticks();

    save_fpu_state(current_proc()->fpu_state);
    kstring::memcpy(child->fpu_state, current_proc()->fpu_state, FPU_STATE_SIZE);
    child->fpu_initialized = true;

    // Fork semantics: the child gets its own table (entries copied, per-vnode
    // refs bumped under the table's irqsave leaf lock).
    child->fdtab = fd_table_copy(current_proc()->fdtab);
    child->vmalist = vma_list_alloc();

    child->cursor_x = current_proc()->cursor_x;
    child->cursor_y = current_proc()->cursor_y;

    // Clone under the address-space lock: sibling threads may mmap/munmap or
    // fault concurrently, and the clone downgrades writable PTEs for COW. A
    // concurrent munmap between the clone's read and its refcount bump would
    // free a frame the child is about to map.
    uint64_t clone_flags = spinlock_acquire_irqsave(current_proc()->vma_lock_ptr);
    child->page_table = vmm_clone_address_space(current_proc()->page_table);
    spinlock_release_irqrestore(current_proc()->vma_lock_ptr, clone_flags);
    if (!child->page_table) {
        process_release_private_fds(child);
        aligned_free(child);
        return static_cast<uint64_t>(-1);
    }

    // The COW downgrade cleared W on the parent's PTEs; other cores running
    // the parent's threads may still cache writable translations. Invalidate
    // the whole user range everywhere before the child becomes runnable.
    vmm_invalidate_tlb_range(0, 0x0000800000000000ULL / 4096ULL);

    spinlock_init(&child->vma_lock);
    child->vma_lock_ptr = &child->vma_lock;
    uint64_t vma_clone_flags = spinlock_acquire_irqsave(current_proc()->vma_lock_ptr);
    VMA *cloned_head = vma_clone(current_proc()->vmalist->head);
    spinlock_release_irqrestore(current_proc()->vma_lock_ptr, vma_clone_flags);
    child->vmalist->head = cloned_head;
    child->vmalist->count = current_proc()->vmalist->count;

    if (current_proc()->vmalist->head && !child->vmalist->head) {
        process_release_private_fds(child);
        vmm_free_address_space(child->page_table);
        aligned_free(child);
        return static_cast<uint64_t>(-1);
    }

    const size_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void *stack_phys = pmm_alloc_frames(stack_pages);
    if (!stack_phys) {
        process_release_private_fds(child);
        if (child->vmalist->head)
            vma_free_all(child->vmalist->head);
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

    stack_top_hhdm -= sizeof(SyscallFrame);
    stack_top_hhdm &= ~static_cast<uint64_t>(alignof(SyscallFrame) - 1);
    SyscallFrame *child_frame = reinterpret_cast<SyscallFrame *>(stack_top_hhdm);

    stack_top_hhdm -= sizeof(Context);
    stack_top_hhdm &= ~static_cast<uint64_t>(alignof(Context) - 1);
    Context *child_context = reinterpret_cast<Context *>(stack_top_hhdm);

    *child_frame = *frame;
    kstring::zero_memory(child_context, sizeof(Context));
    child_context->rip = reinterpret_cast<uint64_t>(fork_ret);
    child->sp = stack_top_hhdm;

    // Capture before publishing: once the child is queued another core can
    // run and exit it (and reap can free the struct) before we return.
    const uint64_t child_pid = child->pid;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = child;
    g_proc_tail = child;
    child->next = g_proc_list;
    proc_list_check_locked("fork");

    child->children_list = nullptr;
    child->sibling_next = current_proc()->children_list;
    current_proc()->children_list = child;

    ready_queue_push(child);
    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
    scheduler_notify_idle_cpus();

    return child_pid;
}

void process_group_kill_siblings(Process *self)
{
    if (!self)
        return;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    // exit() ends the whole thread group. The locked signal variant wakes
    // blocked members atomically under the scheduler lock (an unlocked state
    // read can race scheduler_wait and lose the wakeup, leaving the sibling
    // unkillable). Zombies are skipped: their teardown already happened.
    Process *p = g_proc_list;
    if (p) {
        do {
            if (p != self && p->leader_pid == self->leader_pid && p->state != ProcessState_Zombie)
                signal_send_locked(p, SIGKILL);
            p = p->next;
        } while (p != g_proc_list);
    }

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
}

[[noreturn]] static void process_exit_common(int32_t status, bool group_kill);

void process_exit(int32_t status)
{
    // exit() from any thread terminates the whole group.
    process_exit_common(status, true);
}

void sys_thread_exit(int64_t status)
{
    Process *self = process_get_current();
    if (self && self->user_stack_lo != 0 && self->user_stack_size != 0) {
        // Strictly on the kernel stack here: the caller never returns to the
        // user stack, so the unmap (VMA nodes, PTEs, frames, futex waiters)
        // cannot pull memory out from under running code.
        if (!munmap_process_range(self, self->user_stack_lo, self->user_stack_size)) {
            // A refused unmap leaks the mapping until the group's address
            // space is torn down: loud at Error level (kept in release) so
            // the leak is diagnosable, never silent.
            KLOG(LogModule::Sched, LogLevel::Error, "thread exit: stack unmap refused for pid %llu (%s); mapping leaks",
                 (unsigned long long)self->pid, self->name);
        }
        self->user_stack_lo = 0;
        self->user_stack_size = 0;
    }
    // pthread_exit: this member only, the group stays alive.
    process_exit_common(static_cast<int32_t>(status), false);
}

[[noreturn]] static void process_exit_common(int32_t status, bool group_kill)
{
    DEBUG_INFO("Process %d (%s) exiting with status %d on cpu%u", current_proc()->pid, current_proc()->name, status,
               cpu_get_local()->cpu_id);

    // exit() from any thread terminates the whole group: signal the siblings
    // before releasing this member's resources so dying members cannot race
    // the shared fd-table teardown.
    if (group_kill)
        process_group_kill_siblings(current_proc());

    process_release_private_fds(current_proc());
    shm_cleanup_process(current_proc());

    const uint64_t flags = interrupts_save_disable();
    (void)flags;
    spinlock_acquire(&g_sched_lock);

    // A timed futex wait this thread abandoned by exiting must not leave
    // its registration behind: the walker would otherwise mark a recycled
    // Process at this address.
    clear_timed_wait_entries_locked(current_proc());

    current_proc()->state = ProcessState_Zombie;
    current_proc()->exit_status = status;

    if (current_proc()->children_list) {
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
            Process *last = current_proc()->children_list;
            while (true) {
                last->parent_pid = 1;
                if (!last->sibling_next)
                    break;
                last = last->sibling_next;
            }
            last->sibling_next = init->children_list;
            init->children_list = current_proc()->children_list;
        } else {
            // No init (early boot / ktest / init crashed): reparent to the
            // pid-0 kernel task so the orphans' zombies are still reaped by
            // the kernel-zombie path instead of leaking forever.
            Process *kernel = g_proc_list;
            if (kernel) {
                do {
                    if (kernel->pid == 0)
                        break;
                    kernel = kernel->next;
                } while (kernel != g_proc_list);
            }
            if (kernel && kernel->pid == 0) {
                Process *last = current_proc()->children_list;
                while (true) {
                    last->parent_pid = 0;
                    if (!last->sibling_next)
                        break;
                    last = last->sibling_next;
                }
                last->sibling_next = kernel->children_list;
                kernel->children_list = current_proc()->children_list;
            }
        }
        current_proc()->children_list = nullptr;
    }

    wait_queue_wake_all(&current_proc()->wait_queue);

    Process *parent = nullptr;
    Process *found_p = g_proc_list;
    if (found_p) {
        do {
            if (found_p->pid == current_proc()->parent_pid) {
                parent = found_p;
                break;
            }
            found_p = found_p->next;
        } while (found_p != g_proc_list);
    }

    if (parent) {
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
        Process *p = current_proc()->children_list;

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
                current_proc()->children_list = target->sibling_next;

            Process *prev = g_proc_list;
            while (prev->next != target && prev->next != g_proc_list)
                prev = prev->next;
            if (prev->next == target) {
                prev->next = target->next;
                if (g_proc_list == target)
                    g_proc_list = target->next;
                if (g_proc_tail == target)
                    g_proc_tail = prev;
            } else {
                // The zombie is in our children list but not in the global
                // list: it was already detached (kernel-zombie reap or a
                // corrupted list). Freeing it here would leave a dangling
                // node or double-free a deferred entry.
                KLOG(LogModule::Sched, LogLevel::Error,
                     "waitpid: pid %llu (%s) not linked in process list; refusing to reap",
                     (unsigned long long)target->pid, target->name);
                spinlock_release(&g_sched_lock);
                interrupts_restore(flags);
                return -1;
            }
            proc_list_check_locked("waitpid");

            spinlock_release(&g_sched_lock);
            interrupts_restore(flags);

            process_free_reaped(target);
            DEBUG_INFO("Reaped zombie PID %d", child_pid);
            return static_cast<int64_t>(child_pid);
        }

        if (nohang) {
            if (!current_proc()->children_list) {
                spinlock_release(&g_sched_lock);
                interrupts_restore(flags);
                return -1;
            }
            if (pid != -1) {
                Process *child = nullptr;
                Process *c = current_proc()->children_list;
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

        if (pid == -1) {
            // POSIX: blocking wait for ANY child with no children is ECHILD,
            // not an unresolvable sleep (nothing would ever wake us).
            if (!current_proc()->children_list) {
                spinlock_release(&g_sched_lock);
                interrupts_restore(flags);
                return -1; // ECHILD
            }
            current_proc()->state = ProcessState_Waiting;
            current_proc()->wait_for_pid = 0;
        } else {
            Process *child = nullptr;
            Process *c = current_proc()->children_list;
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
            wait_queue_push(&child->wait_queue, current_proc());
        }

        scheduler_schedule_internal();
        interrupts_restore(flags);
    }
}

void scheduler_sleep(uint64_t ticks)
{
    if (!current_proc())
        return;
    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    sleep_queue_push(current_proc(), ticks);
    scheduler_schedule_internal();
    interrupts_restore(flags);
}

void scheduler_sleep_ms(uint64_t ms)
{
    if (ms == 0)
        return;

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

[[nodiscard]] int64_t sys_thread_create(void (*entry)(), void *arg, void *stack_top, SyscallFrame *frame,
                                        uint64_t stack_lo, uint64_t stack_size, uint64_t flags)
{
    if (!entry || !stack_top || !frame) {
        return -22;
    }

    Process *parent = process_get_current();
    if (!parent) {
        return -1;
    }

    // Only the reserved bit is honored; unknown bits are ignored so a
    // caller that passes garbage in the flags register cannot fail or
    // silently detach.
    const bool create_detached = (flags & THREAD_DETACHED) != 0;

    Process *thread = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    if (!thread) {
        return -1;
    }
    kstring::zero_memory(thread, sizeof(Process));
    event_init(thread->event_queue);
    proc_canary_stamp(thread);

    thread->pid = __atomic_fetch_add(&g_next_pid, 1, __ATOMIC_SEQ_CST);
    thread->leader_pid = parent->leader_pid;
    thread->parent_pid = create_detached ? 0 : parent->pid;
    thread->thread_detached = create_detached;
    thread->user_stack_lo = stack_lo;
    thread->user_stack_size = stack_size;
    thread->uid = parent->uid;
    thread->state = ProcessState_Ready;
    thread->priority = parent->priority;
    thread->time_slice = 0;
    thread->last_run_time = timer_get_ticks();

    save_fpu_state(parent->fpu_state);
    kstring::memcpy(thread->fpu_state, parent->fpu_state, FPU_STATE_SIZE);
    thread->fpu_initialized = true;

    // Thread semantics: share the leader's table live (open/close in this
    // thread is visible to all siblings), no entry copying.
    thread->fdtab = fd_table_share(parent->fdtab);

    thread->cursor_x = parent->cursor_x;
    thread->cursor_y = parent->cursor_y;
    kstring::strncpy(thread->cwd, parent->cwd, sizeof(thread->cwd));

    thread->page_table = parent->page_table;
    thread->vmalist = parent->vmalist; // shared live: head + count
    spinlock_init(&thread->vma_lock);
    thread->vma_lock_ptr = parent->vma_lock_ptr;

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

    stack_top_hhdm -= sizeof(SyscallFrame);
    stack_top_hhdm &= ~static_cast<uint64_t>(alignof(SyscallFrame) - 1);
    SyscallFrame *child_frame = reinterpret_cast<SyscallFrame *>(stack_top_hhdm);

    stack_top_hhdm -= sizeof(Context);
    stack_top_hhdm &= ~static_cast<uint64_t>(alignof(Context) - 1);
    Context *child_context = reinterpret_cast<Context *>(stack_top_hhdm);

    kstring::zero_memory(child_frame, sizeof(SyscallFrame));
    child_frame->rip = reinterpret_cast<uint64_t>(entry);
    child_frame->rsp = reinterpret_cast<uint64_t>(stack_top);
    child_frame->cs = frame->cs;
    child_frame->ss = frame->ss;
    child_frame->rflags = frame->rflags;
    child_frame->arg6 = reinterpret_cast<uint64_t>(arg);

    kstring::zero_memory(child_context, sizeof(Context));
    child_context->rip = reinterpret_cast<uint64_t>(thread_ret);
    thread->sp = stack_top_hhdm;

    kstring::strncpy(thread->name, parent->name, 24);
    kstring::strncat(thread->name, "/thr", 7);

    // Capture before publishing (see process_fork): the thread may run and
    // exit on another core before this function returns.
    const int64_t thread_pid = static_cast<int64_t>(thread->pid);

    const uint64_t pub_flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);
    g_proc_tail->next = thread;
    g_proc_tail = thread;
    thread->next = g_proc_list;

    thread->children_list = nullptr;
    if (create_detached) {
        // Created detached: never waitable. Keep it out of every children
        // list — the kernel-zombie auto-reap (parent_pid == 0) owns its
        // zombie, exactly like sys_thread_detach's routing.
        thread->sibling_next = nullptr;
    } else {
        thread->sibling_next = parent->children_list;
        parent->children_list = thread;
    }

    ready_queue_push(thread);
    spinlock_release(&g_sched_lock);
    interrupts_restore(pub_flags);
    scheduler_notify_idle_cpus();

    return thread_pid;
}

int64_t sys_thread_detach(uint64_t tid)
{
    Process *cur = process_get_current();
    if (!cur)
        return -1;

    const uint64_t flags = interrupts_save_disable();
    spinlock_acquire(&g_sched_lock);

    Process *prev_sibling = nullptr;
    Process *target = cur->children_list;
    while (target) {
        if (target->pid == tid)
            break;
        prev_sibling = target;
        target = target->sibling_next;
    }

    if (!target || target->state == ProcessState_Zombie) {
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);
        return -10; // -ESRCH: not a live child thread
    }

    // Only threads of the caller's group: a forked child is its own group
    // leader (leader_pid == pid). Orphaning it into the auto-reap would
    // discard its exit status where the parent can never collect it.
    if (target->leader_pid == target->pid) {
        spinlock_release(&g_sched_lock);
        interrupts_restore(flags);
        return -10; // -ESRCH: not a child thread
    }

    // Orphan the thread: the kernel-zombie reaper collects parent_pid == 0
    // zombies without any waitpid, which is exactly the detached contract.
    if (prev_sibling)
        prev_sibling->sibling_next = target->sibling_next;
    else
        cur->children_list = target->sibling_next;
    target->sibling_next = nullptr;
    target->parent_pid = 0;
    target->thread_detached = true;

    spinlock_release(&g_sched_lock);
    interrupts_restore(flags);
    return 0;
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
