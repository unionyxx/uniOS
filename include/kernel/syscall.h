#pragma once
#include <stdint.h>
#include <uapi/fs.h>
#include <uapi/syscalls.h>

constexpr int MAX_OPEN_FILES = 128;

struct VNode;
struct Process;

struct FileDescriptor
{
    bool used;
    uint8_t flags;
    uint8_t reserved[6];
    struct VNode *vnode;
    uint64_t offset;
    uint64_t dir_pos;
};

#define FD_FLAG_STORAGE_GUARDED_WRITE 0x01
#define FD_FLAG_STORAGE_GUARDED 0x02
#define FD_FLAG_APPEND 0x04

struct SyscallFrame
{
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t arg6, arg5, arg4; // r9, r8, r10
    uint64_t rip, cs, rflags, rsp, ss;
};

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                    SyscallFrame *frame);
extern "C" void signal_check(SyscallFrame *frame);
extern "C" void signal_send_current(int sig);

struct Process;
// Signal delivery under the caller-held scheduler lock (group-kill path):
// wakes Blocked/Waiting targets so a fatal signal cannot be slept through.
void signal_send_locked(Process *p, int sig);

// Unmap a user range (VMA nodes, PTEs, frames, futex-waiter notification).
// Returns false on protected/invalid ranges.
[[nodiscard]] bool munmap_process_range(Process *p, uint64_t addr, size_t length);

[[nodiscard]] int64_t sys_socket_state(uint64_t handle);

[[nodiscard]] int64_t kernel_exec(const char *path);
[[nodiscard]] bool is_file_open(const char *filename);
void shm_cleanup_process(struct Process *proc);
// Virtual address of shm slot `id` (the fixed-offset SHM area base).
uint64_t shm_slot_address(int id);
