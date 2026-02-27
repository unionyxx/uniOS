#pragma once
#include <stdint.h>

// Syscall numbers (Linux-compatible where possible)
#define SYS_READ   0
#define SYS_WRITE  1
#define SYS_OPEN   2
#define SYS_CLOSE  3
#define SYS_PIPE   22
#define SYS_GETPID 39
#define SYS_FORK   57
#define SYS_EXEC   59
#define SYS_EXIT   60
#define SYS_WAIT4  61

// File descriptor constants
#define STDIN_FD   0
#define STDOUT_FD  1
#define STDERR_FD  2

// Max open files per process
#define MAX_OPEN_FILES 16

// File descriptor entry
struct FileDescriptor {
    bool in_use;
    const char* filename;
    uint64_t position;
    uint64_t size;
    const uint8_t* data;
};

// Represents the stack frame passed to syscall_handler
struct SyscallFrame {
    // Callee-saved pushed by isr128
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    
    // Pushed by CPU on interrupt
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

extern "C" uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3, SyscallFrame* frame);

// Kernel-mode exec (for shell to call directly)
int64_t kernel_exec(const char* path);

// Check if a file is currently open (for use by filesystem)
bool is_file_open(const char* filename);
