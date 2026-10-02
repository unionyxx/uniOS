#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr uint64_t USER_SPACE_MAX = 0x0000800000000000ULL;

/// Validate that [ptr, ptr+size) lies entirely in the current process's
/// user address space with the required permissions. Shared by every
/// syscall that copies through a user pointer.
[[nodiscard]] bool validate_user_ptr(const void *ptr, size_t size, bool write = false);
