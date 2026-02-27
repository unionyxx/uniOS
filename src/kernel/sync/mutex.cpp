#include <kernel/sync/mutex.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>

extern void scheduler_yield();
extern Process* process_get_current();

void mutex_lock(Mutex* mtx) {
    Process* current = process_get_current();
    if (!current) {
        while (__sync_lock_test_and_set(&mtx->locked, 1)) {
            asm volatile("pause" ::: "memory");
        }
        return;
    }
    
    while (true) {
        if (__sync_lock_test_and_set(&mtx->locked, 1) == 0) {
            mtx->owner_pid = current->pid;
            return;
        }
        
        spinlock_acquire(&mtx->wait_lock);
        
        if (mtx->locked == 0) {
            spinlock_release(&mtx->wait_lock);
            continue;
        }
        
        current->state = PROCESS_BLOCKED;
        current->next = mtx->wait_queue;
        mtx->wait_queue = current;
        
        spinlock_release(&mtx->wait_lock);
        
        scheduler_yield();
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
    mtx->owner_pid = 0;
    
    __sync_lock_release(&mtx->locked);
    
    spinlock_acquire(&mtx->wait_lock);
    
    if (mtx->wait_queue) {
        Process* to_wake = mtx->wait_queue;
        mtx->wait_queue = to_wake->next;
        to_wake->next = nullptr;
        to_wake->state = PROCESS_READY;
    }
    
    spinlock_release(&mtx->wait_lock);
}
