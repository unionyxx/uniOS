#pragma once
#include <stdint.h>

// System V AMD64 Syscall ABI:
// RAX = Syscall Number
// Arguments: RDI, RSI, RDX, R10, R8, R9
// Return: RAX
// Clobbered: RCX, R11

#ifdef __CPPCHECK__
#define SYSCALL_REG(reg)
#else
#define SYSCALL_REG(reg) __asm__(reg)
#endif

static inline uint64_t syscall0(uint64_t n)
{
    uint64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(n)
                         : "rcx", "r11", "rdi", "rsi", "rdx", "r10", "r8", "r9", "memory");
    return ret;
}

static inline uint64_t syscall1(uint64_t n, uint64_t a1)
{
    register uint64_t rax SYSCALL_REG("rax") = n;
    register uint64_t rdi SYSCALL_REG("rdi") = a1;
    __asm__ __volatile__("syscall" : "+a"(rax), "+D"(rdi) : : "rcx", "r11", "rsi", "rdx", "r10", "r8", "r9", "memory");
    return rax;
}

static inline uint64_t syscall2(uint64_t n, uint64_t a1, uint64_t a2)
{
    register uint64_t rax SYSCALL_REG("rax") = n;
    register uint64_t rdi SYSCALL_REG("rdi") = a1;
    register uint64_t rsi SYSCALL_REG("rsi") = a2;
    __asm__ __volatile__("syscall"
                         : "+a"(rax), "+D"(rdi), "+S"(rsi)
                         :
                         : "rcx", "r11", "rdx", "r10", "r8", "r9", "memory");
    return rax;
}

static inline uint64_t syscall3(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t rax SYSCALL_REG("rax") = n;
    register uint64_t rdi SYSCALL_REG("rdi") = a1;
    register uint64_t rsi SYSCALL_REG("rsi") = a2;
    register uint64_t rdx SYSCALL_REG("rdx") = a3;
    __asm__ __volatile__("syscall"
                         : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx)
                         :
                         : "rcx", "r11", "r10", "r8", "r9", "memory");
    return rax;
}

static inline uint64_t syscall6(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                uint64_t a6)
{
    register uint64_t rax SYSCALL_REG("rax") = n;
    register uint64_t rdi SYSCALL_REG("rdi") = a1;
    register uint64_t rsi SYSCALL_REG("rsi") = a2;
    register uint64_t rdx SYSCALL_REG("rdx") = a3;
    register uint64_t r10 SYSCALL_REG("r10") = a4;
    register uint64_t r8 SYSCALL_REG("r8") = a5;
    register uint64_t r9 SYSCALL_REG("r9") = a6;
    __asm__ __volatile__("syscall"
                         : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+r"(r10), "+r"(r8), "+r"(r9)
                         :
                         : "rcx", "r11", "memory");
    return rax;
}
