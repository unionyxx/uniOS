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

    /* arg4 is the create-flags word: 0 = joinable, THREAD_DETACHED would
     * create the thread detached. arg5/arg6 record the stack range for
     * the exit-time self-unmap. */
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
 *  - WAKE is word-matched: it reaches only waiters parked on the exact
 *    same 32-bit futex word and returns how many it woke. A woken waiter
 *    whose expected value went stale simply re-runs its loop (a spurious
 *    wakeup, which POSIX allows).
 *  - WAIT's 4th argument is the timeout: 0 = forever, expiry = -110, a
 *    full timed-wait table = -28 (ENOSPC). */

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
     * a wake that finds no waiter parked on this word is the benign
     * lost-wake race - the would-be waiter is still before its park and
     * gets EAGAIN instead. */
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

int pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    (void)attr;
    if (!cond)
        return -22; // EINVAL
    cond->seq = 0;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    uint32_t seq = cond->seq;
    pthread_mutex_unlock(mutex);
    /* Any return is a valid outcome: a real wake, EAGAIN (a signal bumped
     * seq after we recorded it - it was meant for us), or a spurious wake.
     * The caller's predicate loop is the actual synchronization. */
    futex(&cond->seq, FUTEX_WAIT, seq);
    return pthread_mutex_lock(mutex);
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t timeout_ms)
{
    uint32_t seq = cond->seq;
    pthread_mutex_unlock(mutex);
    int err = futex_wait_timeout(&cond->seq, seq, timeout_ms);
    int lock_err = pthread_mutex_lock(mutex);
    if (lock_err != 0)
        return lock_err;
    /* Only the kernel deadline walker reports -110. EAGAIN and EINTR both
     * end as a successful (possibly spurious) wait - the caller re-checks
     * its predicate. -28 means the timed-wait table was full and the wait
     * never started: propagate it, or a saturated table silently turns
     * the caller's deadline loop into a busy spin. */
    if (err == -110 || err == -28)
        return err;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    /* Bump before waking: the bump invalidates every recorded wait value,
     * so an in-flight waiter that has not parked yet returns EAGAIN rather
     * than sleeping through this signal. */
    __sync_fetch_and_add(&cond->seq, 1u);
    futex(&cond->seq, FUTEX_WAKE, 1u);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    __sync_fetch_and_add(&cond->seq, 1u);
    /* The kernel futex has no REQUEUE (the uapi constant exists but
     * sys_futex implements WAIT/WAKE only), so wake every waiter here and
     * let each re-acquire the mutex through its own contended path - the
     * end state a requeue would produce, at thundering-herd cost. */
    futex(&cond->seq, FUTEX_WAKE, 0x7FFFFFFFu);
    return 0;
}

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    if (!once || !fn)
        return -22; // EINVAL
    if (*once == 2u) {
        /* Acquire pair for the initializer's release, so fn's writes are
         * visible before anything this thread does after the call. */
        __sync_synchronize();
        return 0;
    }
    if (__sync_val_compare_and_swap(once, 0u, 1u) == 0u) {
        fn();
        /* A locked op publishes the done state: full barrier, so every
         * write fn made is visible before the flag, and atomic, so no
         * reader observes a torn transition. */
        __sync_val_compare_and_swap(once, 1u, 2u);
        futex(once, FUTEX_WAKE, 0x7FFFFFFFu);
        return 0;
    }
    /* Another thread is running the initializer: sleep on the running
     * state - the 1 -> 2 transition turns the value check into EAGAIN and
     * exits the loop (a plain read of 2 exits it without parking at all). */
    while (*once != 2u)
        futex(once, FUTEX_WAIT, 1u);
    __sync_synchronize();
    return 0;
}

/* Rwlock word: bits 0-29 = active readers, bit 30 = a writer holds the
 * lock, bit 31 = a writer wants it. The wanting bit makes arriving readers
 * queue behind the waiter instead of starving it. Sleepers only ever park
 * on values where the lock is held (readers > 0 or writer set), so every
 * release-to-idle invalidates or wakes them; a waiter parked on a stale
 * value re-runs its loop when woken. */
#define RWLOCK_RD_MASK 0x3FFFFFFFu
#define RWLOCK_WRITER 0x40000000u
#define RWLOCK_WWAIT 0x80000000u
#define RWLOCK_WAKE_ALL 0x7FFFFFFFu

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const void *attr)
{
    (void)attr;
    if (!rwlock)
        return -22; // EINVAL
    rwlock->state = 0;
    return 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    for (;;) {
        uint32_t state = rwlock->state;
        if ((state & (RWLOCK_WRITER | RWLOCK_WWAIT)) == 0u) {
            if (__sync_val_compare_and_swap(&rwlock->state, state, state + 1u) == state)
                return 0;
            continue; // a writer arrived: re-evaluate before queueing
        }
        futex(&rwlock->state, FUTEX_WAIT, state);
    }
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    for (;;) {
        uint32_t state = rwlock->state;
        if ((state & (RWLOCK_RD_MASK | RWLOCK_WRITER)) == 0u) {
            /* Acquiring clears the wanting bit: we are the writer now. */
            if (__sync_val_compare_and_swap(&rwlock->state, state, RWLOCK_WRITER) == state)
                return 0;
            continue;
        }
        if ((state & RWLOCK_WWAIT) == 0u) {
            if (__sync_val_compare_and_swap(&rwlock->state, state, state | RWLOCK_WWAIT) != state)
                continue; // state moved: re-evaluate
            state |= RWLOCK_WWAIT;
        }
        futex(&rwlock->state, FUTEX_WAIT, state);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    for (;;) {
        uint32_t state = rwlock->state;
        if (state & (RWLOCK_WRITER | RWLOCK_WWAIT))
            return -16; // EBUSY
        if (__sync_val_compare_and_swap(&rwlock->state, state, state + 1u) == state)
            return 0;
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    for (;;) {
        uint32_t state = rwlock->state;
        if (state & (RWLOCK_RD_MASK | RWLOCK_WRITER))
            return -16; // EBUSY
        if (__sync_val_compare_and_swap(&rwlock->state, state, RWLOCK_WRITER) == state)
            return 0;
    }
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    uint32_t state = rwlock->state;
    if (state & RWLOCK_WRITER) {
        /* Drop the writer bit, keep the wanting-writer hint (other writers
         * may be queued). The CAS can only race a waiter setting the hint
         * - the writer bit itself is ours until we clear it. */
        for (;;) {
            uint32_t cur = rwlock->state;
            if (__sync_val_compare_and_swap(&rwlock->state, cur, cur & ~RWLOCK_WRITER) == cur)
                break;
        }
        futex(&rwlock->state, FUTEX_WAKE, RWLOCK_WAKE_ALL);
        return 0;
    }
    if ((state & RWLOCK_RD_MASK) == 0u)
        return -1; // EPERM: not read-held (no owner tracking to say more)
    /* The subtraction cannot borrow into the writer bits: read-held means
     * the low field is nonzero, and the high bits only change under their
     * own CAS rules. */
    uint32_t prev = __sync_fetch_and_sub(&rwlock->state, 1u);
    if ((prev & RWLOCK_RD_MASK) == 1u)
        futex(&rwlock->state, FUTEX_WAKE, RWLOCK_WAKE_ALL); // last reader out
    return 0;
}
