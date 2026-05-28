#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <uapi/syscalls.h>
#include <uapi/syscalls_ext.h>

#define MAP_FAILED    ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t len, int prot);
int memfd_create(const char *name, unsigned int flags);

#ifdef __cplusplus
}
#endif
