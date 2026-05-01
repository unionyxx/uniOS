#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/mutex.h>
#include <kernel/sync/spinlock.h>

void mutex_lock(Mutex *mtx)
{
    Process *current = process_get_current();
    if (!current) {
        // Kernel-only spinlock fallback (no scheduler yet)
        while (__sync_lock_test_and_set(&mtx->locked, 1)) {
            asm volatile("pause" ::: "memory");
        }
        return;
    }

    while (true) {
        spinlock_acquire(&mtx->wait_lock);

        if (__sync_lock_test_and_set(&mtx->locked, 1) == 0) {
            mtx->owner_pid = current->pid;
            spinlock_release(&mtx->wait_lock);
            return;
        }

        // Use unified scheduler waiting mechanism
        scheduler_wait(&mtx->wait_queue, &mtx->wait_lock);
    }
}

bool mutex_try_lock(Mutex *mtx)
{
    if (__sync_lock_test_and_set(&mtx->locked, 1) == 0) {
        Process *current = process_get_current();
        if (current) {
            mtx->owner_pid = current->pid;
        }
        return true;
    }
    return false;
}

void mutex_unlock(Mutex *mtx)
{
    spinlock_acquire(&mtx->wait_lock);
    mtx->owner_pid = 0;
    __sync_lock_release(&mtx->locked);

    // Wake all waiting processes (they will contend for the lock again)
    // Alternatively, we could wake just one, but then we must ensure it gets the lock.
    // Given the current MLFQ, wake_all is simpler and prevents starvation.
    scheduler_wake_all(&mtx->wait_queue);

    spinlock_release(&mtx->wait_lock);
}
