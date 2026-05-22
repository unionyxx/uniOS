#pragma once
#include <stdint.h>
#include <kernel/sync/spinlock.h>
#include <kernel/process.h>

struct FutexBucket {
    Spinlock lock;
    WaitQueue wait_queue;
};

constexpr size_t FUTEX_HASH_SIZE = 256;
extern FutexBucket g_futex_table[FUTEX_HASH_SIZE];

void futex_init();
int64_t sys_futex(volatile uint32_t *uaddr, int op, uint32_t val);
