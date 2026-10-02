#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* x86-64 thread control block, lives at the thread pointer (fs base).
 * TLS codegen on this target loads %fs:0 and adds link-time negative
 * offsets into the static TLS block that ends exactly at this struct,
 * so self must hold the block's own address. */
typedef struct UniTcb
{
    uint64_t self; /* = address of this struct, i.e. the fs base */
    uint64_t tid;  /* kernel pid of this thread */
} UniTcb;

#ifdef __cplusplus
}
#endif
