#pragma once
#include <kernel/cpu.h>
#include <stddef.h>
#include <stdint.h>

constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ULL;

/// Validate that [ptr, ptr+size) lies entirely in the current process's
/// user address space with the required permissions. Shared by every
/// syscall that copies through a user pointer.
[[nodiscard]] bool validate_user_ptr(const void *ptr, size_t size, bool write = false);

/// SMAP lift/restore around supervisor accesses to user pages. Kernel code
/// outside the syscall dispatcher (sys_* implementations callable from ktest
/// context) needs the same guards.
#define KSTAC()                                                                                                        \
    if (g_cpu_features.has_smap)                                                                                       \
    asm volatile("stac" ::: "memory")
#define KCLAC()                                                                                                        \
    if (g_cpu_features.has_smap)                                                                                       \
    asm volatile("clac" ::: "memory")

/// Fault-fixup copies: return false instead of faulting when the access
/// hits an unmapped or SMAP-protected page. Still require KSTAC() when
/// SMAP is active.
extern "C" [[nodiscard]] bool safe_copy_from_user(void *dest, const void *src, size_t n);
extern "C" [[nodiscard]] bool safe_copy_to_user(void *dest, const void *src, size_t n);
