#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pthread_t is the kernel pid of the thread. */
typedef uint64_t pthread_t;

/* Error convention: 0 on success, raw negative errno on failure (the
 * extended-syscall convention - userspace has no errno). */

/* Create a thread running fn(arg) on a library-owned stack: 128 KiB usable
 * plus a PROT_NONE guard page, unmapped by the kernel when the thread exits.
 * attr is ignored in this sprint. */
int pthread_create(pthread_t *thread, const void *attr, void *(*fn)(void *), void *arg);

/* Wait for a joinable thread and collect its exit status. The status travels
 * through the 32-bit exit channel, so only values that fit in int32_t
 * round-trip (pointers into mmap'd regions do not). */
int pthread_join(pthread_t thread, void **retval);

/* Terminate only the calling thread; the group stays alive. Never returns. */
void pthread_exit(void *retval) __attribute__((noreturn));

/* Route a child thread to the kernel-zombie reaper: its exit needs no join. */
int pthread_detach(pthread_t thread);

/* The calling thread's id, read from the thread control block the kernel
 * keeps at the fs base (uapi/tcb.h). Every user thread has one (the
 * always-TCB invariant), so this never fails and needs no syscall. */
pthread_t pthread_self(void);

/* ---- Mutex: one word. 0 = unlocked, 1 = locked, 2 = locked with waiters.
 * The all-zero encoding makes a zero-initialized object a valid unlocked
 * mutex. attr is ignored in this sprint. */
typedef struct
{
    volatile uint32_t state;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {0}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
/* Unlocking an idle mutex (state 0) returns -1 (EPERM) with the state
 * unchanged; a non-holder unlock of a mutex another thread holds is not
 * detectable without owner tracking and still succeeds. */
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);

/* ---- Condvar: a sequence counter. Waiters record the counter, release the
 * mutex and futex-wait on the recorded value; every signal bumps the
 * counter, so a waiter that has not parked yet re-validates instead of
 * sleeping through it. Spurious wakeups are possible (and allowed). */
typedef struct
{
    volatile uint32_t seq;
} pthread_cond_t;

#define PTHREAD_COND_INITIALIZER {0}

int pthread_cond_init(pthread_cond_t *cond, const void *attr);
/* Returns 0 after re-acquiring the mutex; the caller owns the predicate. */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
/* Relative timeout in milliseconds (0 = wait forever); returns -110
 * (ETIMEDOUT) when the deadline fires, -28 (ENOSPC) when the kernel's
 * fixed timed-wait table is full and the wait never started. */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, uint64_t timeout_ms);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

/* ---- Once: 0 = not run, 1 = initializer running, 2 = done. init runs
 * exactly once no matter how many threads race here. */
typedef volatile uint32_t pthread_once_t;

#define PTHREAD_ONCE_INIT 0

int pthread_once(pthread_once_t *once, void (*fn)(void));

/* ---- Rwlock: one word - active reader count in the low bits, writer flags
 * above. A waiting writer blocks new readers, so a reader stream cannot
 * starve it. attr is ignored in this sprint. */
typedef struct
{
    volatile uint32_t state;
} pthread_rwlock_t;

#define PTHREAD_RWLOCK_INITIALIZER {0}

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const void *attr);
int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock);
int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock);

#ifdef __cplusplus
}
#endif
