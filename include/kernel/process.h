#pragma once
#include <kernel/event.h>
#include <kernel/mm/vma.h>
#include <kernel/sync/spinlock.h>
#include <kernel/syscall.h>
#include <stdint.h>
#include <uapi/signal.h>
#include <uapi/sysinfo.h>

using ProcessState = ProcessStateU;

struct WaitQueue
{
    struct Process *head;
    struct Process *tail;
};

struct SignalControl
{
    uint64_t pending;
    uint64_t blocked;
    sighandler_t handlers[32];
    uint64_t restorer;
};

struct Context
{
    uint64_t r15, r14, r13, r12, rbp, rbx, rip;
};

constexpr size_t FPU_STATE_SIZE = 4096; // Increased to 4K for safety

// Shared, refcounted-by-lifetime VMA list. Threads share the leader's list
// object live (the head field is shared, so unlinking the first node is
// visible to every member); fork clones the list into a fresh object (COW).
// Lifetime follows the existing deferred-free rules in the scheduler
// (compare vmalist pointers), like page tables.
struct VmaList
{
    VMA *head;
    uint32_t count;
};

[[nodiscard]] VmaList *vma_list_alloc();
void vma_list_free(VmaList *list);

// Shared, refcounted file-descriptor table. Fork deep-copies the entries
// (per-vnode refs bumped); threads created by sys_thread_create share the
// leader's table live, so fd operations in one thread are visible to all
// siblings. The table is freed when its last holder exits.
struct FdTable
{
    alignas(64) Spinlock lock;
    uint64_t refs;
    FileDescriptor fds[MAX_OPEN_FILES];
};

[[nodiscard]] FdTable *fd_table_alloc(bool stdio_marks = true);
[[nodiscard]] FdTable *fd_table_copy(FdTable *src);
[[nodiscard]] FdTable *fd_table_share(FdTable *t);
void fd_table_release(FdTable *t);

struct Process
{
    // === Fields accessed by assembly (PROC_* in process.asm) ===
    // MUST keep these first and carefully aligned!

    // Offset 0
    uint32_t uid;
    uint32_t _pad0;
    uint64_t _pad1;

    // Offset 16..63
    uint64_t _padding_fpu[6];

    // Offset 64 (64-byte aligned)
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(64)));

    // Offset 64 + 4096 = 4160
    uint64_t pid;
    uint64_t parent_pid;
    char name[32];
    uint64_t cpu_time;
    uint64_t sp; // Kernel stack pointer (used in switch_to_task)

    // === Fields NOT accessed by assembly (no fixed offset required) ===

    uint64_t *stack_base;
    uint64_t stack_phys;
    uint64_t *page_table;
    ProcessState state;
    int32_t exit_status;
    uint64_t wait_for_pid;
    uint64_t wake_time;
    bool fpu_initialized;

    bool exec_done;
    int32_t exec_exit_status;
    uint8_t priority;
    uint8_t _pad_priority[7]; // Explicit padding to force 8-byte alignment

    FdTable *fdtab;

    alignas(64) Spinlock vma_lock;
    VmaList *vmalist; // shared with threads, cloned on fork
    Spinlock *vma_lock_ptr;
    uint32_t _pad_vma[5]; // Maintain 64-byte alignment or at least clear padding

    uint64_t cursor_x;
    uint64_t cursor_y;
    char cwd[256];
    uint64_t exec_entry;

    uint32_t time_slice;
    uint64_t last_run_time;
    uint64_t block_start_time;

    // Thread-group state: leader_pid is the process's own pid for leaders
    // and plain processes, the leader's pid for threads created by
    // sys_thread_create. exit() group-kills every live member.
    uint64_t leader_pid;
    uint64_t user_stack_lo; // recorded thread stack (0 = none)
    uint64_t user_stack_size;
    bool thread_detached; // detached at exit: routes to the kernel-zombie auto-reap
    bool timed_wake;      // woken by the deadline walker (futex timeouts)

    SignalControl signals;

    struct Process *children_list;
    struct Process *sibling_next;
    struct Process *next;       // Global process list
    struct Process *queue_next; // Ready/sleep/wait queue next
    WaitQueue *waiting_queue;   // Owning wait queue when blocked on a queue
    bool in_ready_queue;
    bool on_cpu;
    WaitQueue wait_queue;       // Child/other waiters blocked on this process
    WaitQueue event_wait_queue; // Waiters blocked in SYS_GET_EVENT for this process
    EventQueue event_queue;

    uint32_t preempt_count;
    uint32_t preempt_pending;
#ifdef DEBUG
    // Trailing canary: a wild write into the struct (heap overflow from a
    // neighbour, stale-pointer reuse) corrupts this before the scheduler
    // fields that keep the process list walkable. Checked by the scheduler's
    // list-integrity guard.
    uint64_t debug_canary;
#endif
};

extern "C" void switch_to_task(Process *current, Process *next);

[[nodiscard]] Process *process_get_current();
[[nodiscard]] Process *process_find_by_pid(uint64_t pid);
[[nodiscard]] uint64_t process_fork(struct SyscallFrame *frame);
void process_init();
void process_exit(int32_t status);
[[nodiscard]] int64_t process_waitpid(int64_t pid, int32_t *status, int options);

// SIGKILL every live member of the caller's thread group (caller excluded,
// zombies skipped). Wakes blocked members through the signal path; called by
// process_exit before the caller zombifies.
void process_group_kill_siblings(Process *self);

// Terminate only the calling thread: unmap the recorded user stack while on
// the kernel stack, then exit without group-kill. Never returns.
[[noreturn]] void sys_thread_exit(int64_t status);

// Mark a child thread detached (ESRH/-10 if not a live child): it leaves the
// caller's children list and the kernel-zombie reaper takes it on exit.
int64_t sys_thread_detach(uint64_t tid);

void system_reboot();
void system_poweroff();
