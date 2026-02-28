#pragma once
#include <stdint.h>

constexpr uint64_t SYS_READ     = 0;
constexpr uint64_t SYS_WRITE    = 1;
constexpr uint64_t SYS_OPEN     = 2;
constexpr uint64_t SYS_CLOSE    = 3;
constexpr uint64_t SYS_PIPE     = 22;
constexpr uint64_t SYS_GETPID   = 39;
constexpr uint64_t SYS_FORK     = 57;
constexpr uint64_t SYS_EXEC     = 59;
constexpr uint64_t SYS_EXIT     = 60;
constexpr uint64_t SYS_WAIT4    = 61;
constexpr uint64_t SYS_GETDENTS = 78;

constexpr int STDIN_FD  = 0;
constexpr int STDOUT_FD = 1;
constexpr int STDERR_FD = 2;

constexpr int MAX_OPEN_FILES = 32;

struct VNode;

struct FileDescriptor {
    bool used;
    struct VNode* vnode;
    uint64_t offset;
    uint64_t dir_pos;
    uint32_t last_cluster;
    uint64_t last_offset;
};

constexpr int O_RDONLY = 0;
constexpr int O_WRONLY = 1;
constexpr int O_RDWR   = 2;
constexpr int O_CREAT  = 64;
constexpr int O_TRUNC  = 512;
constexpr int O_APPEND = 1024;

struct SyscallFrame {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t rip, cs, rflags, rsp, ss;
};

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, SyscallFrame* frame);

[[nodiscard]] int64_t kernel_exec(const char* path);
[[nodiscard]] bool is_file_open(const char* filename);
