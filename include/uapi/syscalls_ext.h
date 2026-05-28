#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <uapi/syscalls.h>

#define THREAD_DETACHED    (1u << 0)
#define THREAD_INHERIT_FDS (1u << 1)

typedef struct thread_attr {
    uint32_t flags;
    uint32_t priority;
    size_t stack_size;
    void*  stack_addr;
} thread_attr_t;

#define MFD_CLOEXEC       0x0001u
#define MFD_ALLOW_SEALING 0x0002u

#define MAP_SHARED    0x01u
#define MAP_PRIVATE   0x02u
#define MAP_ANONYMOUS 0x20u

#ifdef __cplusplus
}
#endif
