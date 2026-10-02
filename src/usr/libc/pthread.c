#include "pthread.h"

#include <stdint.h>
#include <sys/mman.h>
#include <uapi/syscalls.h>

#include "syscall.h"
#include "unistd.h"

/* Stack geometry: one anonymous RW mapping per thread with a PROT_NONE guard
 * page below 128 KiB of usable stack. SYS_THREAD_EXIT unmaps the recorded
 * range, so the whole mapping belongs to the thread and is reclaimed without
 * a join. */
#define PTHREAD_GUARD_SIZE 0x1000ULL
#define PTHREAD_STACK_SIZE 0x20000ULL
#define PTHREAD_MAP_SIZE (PTHREAD_GUARD_SIZE + PTHREAD_STACK_SIZE)

_Static_assert((PTHREAD_MAP_SIZE & 0xFFFULL) == 0, "thread stack map must be page aligned");
_Static_assert(PTHREAD_GUARD_SIZE == 0x1000ULL, "the guard is exactly one page");

/* The kernel builds the entry frame from the caller's live syscall frame, so
 * the thread starts with the caller's real segments (CS 0x23, SS 0x1B,
 * RFLAGS 0x202) - nothing is fabricated here. When fn returns, the shim
 * address at the entry rsp bounces its return value into pthread_exit. */
extern void __thread_exit_shim(void);

int pthread_create(pthread_t *thread, const void *attr, void *(*fn)(void *), void *arg)
{
    (void)attr;

    if (!thread || !fn)
        return -22; // EINVAL

    uint8_t *base = mmap(NULL, (size_t)PTHREAD_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return -12; // ENOMEM

    if (mprotect(base, (size_t)PTHREAD_GUARD_SIZE, PROT_NONE) != 0) {
        munmap(base, (size_t)PTHREAD_MAP_SIZE);
        return -12; // ENOMEM
    }

    uint64_t top = (uint64_t)(uintptr_t)base + PTHREAD_MAP_SIZE;
    /* fn's return address: entering with rsp = top - 8 keeps the SysV entry
     * alignment (rsp % 16 == 8) and makes a plain return fall into the shim. */
    *(uint64_t *)(uintptr_t)(top - 8) = (uint64_t)(uintptr_t)__thread_exit_shim;

    /* arg4 is unused by the kernel; arg5/arg6 record the stack range for the
     * exit-time self-unmap. */
    int64_t tid = (int64_t)syscall6(SYS_THREAD_CREATE, (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)arg, top - 8, 0,
                                    (uint64_t)(uintptr_t)base, PTHREAD_MAP_SIZE);
    if (tid < 0) {
        munmap(base, (size_t)PTHREAD_MAP_SIZE);
        return (int)tid;
    }

    *thread = (pthread_t)tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    int status = 0;
    if (waitpid((int)thread, &status) < 0)
        return -10; // ESRCH: not a joinable child of this process
    if (retval)
        *retval = (void *)(intptr_t)status;
    return 0;
}

void pthread_exit(void *retval)
{
    /* The value travels through the 32-bit exit status channel. */
    syscall1(SYS_THREAD_EXIT, (uint64_t)(intptr_t)retval);
    while (1)
        ; // unreachable: the thread never returns from the exit syscall
}

int pthread_detach(pthread_t thread)
{
    return (int)syscall1(SYS_THREAD_DETACH, thread);
}

pthread_t pthread_self(void)
{
    return (pthread_t)syscall1(SYS_GETPID, 0);
}
/* ---- Synchronization primitives --------------------------------------- */

/* Kernel futex contract (src/kernel/sync/futex.cpp), which these algorithms
 * are built against:
 *  - WAIT re-checks the expected value under the bucket lock and returns
 *    -11 (EAGAIN) without sleeping when it moved. That closes the
 *    record-then-park window: anything that changes the word between a
 *    waiter's read and its park shows up as EAGAIN, never as a lost wake.
 *  - WAKE matches waiters by physical address, not by value, and returns
 *    how many it woke. A woken waiter whose expected value went stale
 *    simply re-runs its loop (a spurious wakeup, which POSIX allows).
 *  - WAIT's 4th argument is the timeout: 0 = forever, expiry = -110. */

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void)attr;
    if (!mutex)
        return -22; // EINVAL
    mutex->state = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    /* Fast path: 0 -> 1, locked with no waiters. */
    if (__sync_val_compare_and_swap(&mutex->state, 0u, 1u) == 0u)
        return 0;

    for (;;) {
        uint32_t state = mutex->state;
        if (state == 0u) {
            /* Acquire straight into the contended state: we (and possibly
             * others) have been waiting, so unlockers must keep waking. */
            if (__sync_val_compare_and_swap(&mutex->state, 0u, 2u) == 0u)
                return 0;
        } else {
            /* 1 -> 2 tells the unlocker a waiter exists; then park on 2.
             * A racing unlocker makes the CAS fail - re-evaluate instead
             * of parking on a state that just went away. */
            if (state == 1u && __sync_val_compare_and_swap(&mutex->state, 1u, 2u) != 1u)
                continue;
            futex(&mutex->state, FUTEX_WAIT, 2u);
        }
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    /* 1 -> 0 uncontended; 2 -> 0 hands off: one woken waiter re-acquires
     * via CAS(0 -> 2), which re-arms the wake for the next unlock. The
     * exchange is a full barrier publishing the critical section's writes;
     * a wake against an empty bucket is the benign lost-wake race - the
     * would-be waiter is still before its park and gets EAGAIN instead. */
    if (__sync_lock_test_and_set(&mutex->state, 0u) != 1u)
        futex(&mutex->state, FUTEX_WAKE, 1u);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    if (__sync_val_compare_and_swap(&mutex->state, 0u, 1u) == 0u)
        return 0;
    return -16; // EBUSY
}
