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

    alignas(64) Spinlock fd_lock;
    FileDescriptor fd_table[MAX_OPEN_FILES];

    alignas(64) Spinlock vma_lock;
    VMA *vma_list;
    uint32_t vma_count;
    uint32_t _pad_vma[7]; // Maintain 64-byte alignment or at least clear padding

    uint64_t cursor_x;
    uint64_t cursor_y;
    char cwd[256];
    uint64_t exec_entry;

    uint32_t time_slice;
    uint64_t last_run_time;
    uint64_t block_start_time;

    SignalControl signals;

    struct Process *children_list;
    struct Process *sibling_next;
    struct Process *next;       // Global process list
    struct Process *queue_next; // Ready/sleep/wait queue next
    WaitQueue *waiting_queue;   // Owning wait queue when blocked on a queue
    WaitQueue wait_queue;       // Child/other waiters blocked on this process
    WaitQueue event_wait_queue; // Waiters blocked in SYS_GET_EVENT for this process
    EventQueue event_queue;
};

extern "C" void switch_to_task(Process *current, Process *next);

[[nodiscard]] Process *process_get_current();
[[nodiscard]] Process *process_find_by_pid(uint64_t pid);
[[nodiscard]] uint64_t process_fork(struct SyscallFrame *frame);
void process_init();
void process_exit(int32_t status);
[[nodiscard]] int64_t process_waitpid(int64_t pid, int32_t *status);

void system_reboot();
void system_poweroff();
