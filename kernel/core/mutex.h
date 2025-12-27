#pragma once
#include <stdint.h>
#include "spinlock.h"
#include "process.h"

/**
 * @file mutex.h
 * @brief Sleeping mutex for kernel synchronization
 * 
 * Unlike spinlocks which busy-wait, mutexes block the calling thread
 * and yield the CPU to other tasks. Use mutexes for longer critical sections.
 * 
 * Usage:
 *   Mutex mtx = MUTEX_INIT;
 *   mutex_lock(&mtx);
 *   // critical section
 *   mutex_unlock(&mtx);
 */

struct Mutex {
    volatile uint32_t locked;       // 0 = unlocked, 1 = locked
    volatile uint64_t owner_pid;    // PID of owner (for debugging)
    Spinlock wait_lock;             // Protects the wait queue
    Process* wait_queue;            // Head of waiting process list
};

#define MUTEX_INIT {0, 0, SPINLOCK_INIT, nullptr}

// Initialize a mutex
static inline void mutex_init(Mutex* mtx) {
    mtx->locked = 0;
    mtx->owner_pid = 0;
    spinlock_init(&mtx->wait_lock);
    mtx->wait_queue = nullptr;
}

// Acquire mutex (blocks if held by another thread)
void mutex_lock(Mutex* mtx);

// Try to acquire mutex without blocking
bool mutex_try_lock(Mutex* mtx);

// Release mutex (wakes one waiting thread if any)
void mutex_unlock(Mutex* mtx);

// Check if mutex is locked
static inline bool mutex_is_locked(Mutex* mtx) {
    return mtx->locked != 0;
}
