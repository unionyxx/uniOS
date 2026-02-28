#pragma once
#include <stdint.h>
#include <kernel/syscall.h>
#include <kernel/mm/vma.h>

enum class ProcessState {
    Ready,
    Running,
    Blocked,
    Sleeping,
    Zombie,
    Waiting
};

struct Context {
    uint64_t r15, r14, r13, r12, rbp, rbx, rflags, rip;
};

constexpr size_t FPU_STATE_SIZE = 512;

struct Process {
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    
    uint64_t pid;
    uint64_t parent_pid;
    char name[32];
    uint64_t cpu_time;
    uint64_t sp;
    uint64_t* stack_base;
    uint64_t stack_phys;
    uint64_t* page_table;
    ProcessState state;
    int32_t exit_status;
    uint64_t wait_for_pid;
    uint64_t wake_time;
    bool fpu_initialized;
    FileDescriptor fd_table[MAX_OPEN_FILES];
    VMA* vma_list;
    uint64_t cursor_x;
    uint64_t cursor_y;
    char cwd[256];
    uint64_t exec_entry;
    bool     exec_done;
    int32_t  exec_exit_status;
    Process* next;
};

extern "C" void switch_to_task(Process* current, Process* next);

[[nodiscard]] Process* process_get_current();
[[nodiscard]] Process* process_find_by_pid(uint64_t pid);
[[nodiscard]] uint64_t process_fork(struct SyscallFrame* frame);
void process_exit(int32_t status);
[[nodiscard]] int64_t process_waitpid(int64_t pid, int32_t* status);
