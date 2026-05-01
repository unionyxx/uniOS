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

[[nodiscard]] int64_t kernel_exec(const char *path);
[[nodiscard]] bool is_file_open(const char *filename);
void shm_cleanup_process(struct Process *proc);
