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

// Wake every waiter keyed on any of the given (just-unmapped) physical
// frames. munmap calls this before the frames return to the PMM: a waiter
// parked on a page that no longer exists must not sleep forever, and a
// recycled frame must not inherit stale waiters. The wakes are spurious by
// futex semantics; wakers re-validate their address on the next call.
void futex_notify_freed_frames(const uint64_t *frames, size_t count);
