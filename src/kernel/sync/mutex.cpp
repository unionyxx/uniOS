#include <kernel/sync/mutex.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>

// Forward declaration
extern void scheduler_yield();
extern Process* process_get_current();

void mutex_lock(Mutex* mtx) {
    Process* current = process_get_current();
    if (!current) {
        // No scheduler running yet - use spinlock behavior
        while (__sync_lock_test_and_set(&mtx->locked, 1)) {
            asm volatile("pause" ::: "memory");
        }
        return;
    }
    
    while (true) {
        // Try to acquire the lock atomically
        if (__sync_lock_test_and_set(&mtx->locked, 1) == 0) {
            // Got the lock!
            mtx->owner_pid = current->pid;
            return;
        }
        
        // Lock is held by someone else - block and yield
        spinlock_acquire(&mtx->wait_lock);
        
        // Double-check: maybe it was released while we were acquiring wait_lock
        if (mtx->locked == 0) {
            spinlock_release(&mtx->wait_lock);
            continue;  // Try again
        }
        
        // Add ourselves to the wait queue
        current->state = PROCESS_BLOCKED;
        current->next = mtx->wait_queue;
        mtx->wait_queue = current;
        
        spinlock_release(&mtx->wait_lock);
        
        // Yield CPU to other tasks
        scheduler_yield();
        
        // We've been woken up - try to acquire the lock again
    }
}

bool mutex_try_lock(Mutex* mtx) {
    if (__sync_lock_test_and_set(&mtx->locked, 1) == 0) {
        Process* current = process_get_current();
        if (current) {
            mtx->owner_pid = current->pid;
        }
        return true;
    }
    return false;
}

void mutex_unlock(Mutex* mtx) {
    // Clear owner
    mtx->owner_pid = 0;
    
    // Release the lock
    __sync_lock_release(&mtx->locked);
    
    // Wake one waiting process if any
    spinlock_acquire(&mtx->wait_lock);
    
    if (mtx->wait_queue) {
        Process* to_wake = mtx->wait_queue;
        mtx->wait_queue = to_wake->next;
        to_wake->next = nullptr;
        to_wake->state = PROCESS_READY;
    }
    
    spinlock_release(&mtx->wait_lock);
}
