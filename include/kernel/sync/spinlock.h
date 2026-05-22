#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * @file spinlock.h
 * @brief Interrupt-safe spinlock primitives for kernel synchronization
 *
 * Spinlocks provide mutual exclusion in the kernel.
 *
 * Two API styles are provided:
 *
 *   1. IRQ-save (recommended for most callers):
 *        uint64_t flags = spinlock_acquire_irqsave(&lock);
 *        // critical section
 *        spinlock_release_irqrestore(&lock, flags);
 *
 *   2. Raw (for callers that manage interrupts themselves):
 *        spinlock_acquire(&lock);
 *        // critical section — caller must ensure IRQs are already disabled
 *        spinlock_release(&lock);
 *
 * The IRQ-save variant returns the saved RFLAGS on the *caller's stack*,
 * eliminating the SMP race that existed when flags were stored inside the
 * Spinlock struct (where a second core could overwrite them before the
 * first core released).
 */

struct Spinlock
{
    volatile uint32_t locked; // 0 = unlocked, 1 = locked
};

#define SPINLOCK_INIT {0}

/**
 * @brief Save interrupt state and disable interrupts
 * @return The previous RFLAGS value
 */
static inline uint64_t interrupts_save_disable()
{
    uint64_t flags;
    asm volatile("pushfq\n\t"
                 "pop %0\n\t"
                 "cli"
                 : "=r"(flags)
                 :
                 : "memory");
    return flags;
}

/**
 * @brief Restore interrupt state from saved flags
 * @param flags Previously saved RFLAGS value
 */
static inline void interrupts_restore(uint64_t flags)
{
    asm volatile("push %0\n\t"
                 "popfq"
                 :
                 : "r"(flags)
                 : "memory", "cc");
}

/**
 * @brief Check if interrupts are currently enabled
 * @return true if interrupts are enabled
 */
static inline bool interrupts_enabled()
{
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0; // IF flag is bit 9
}

/**
 * @brief Initialize a spinlock
 * @param sl Pointer to the spinlock to initialize
 */
static inline void spinlock_init(Spinlock *sl)
{
    if (!sl)
        return;
    sl->locked = 0;
}

/**
 * @brief Acquire a spinlock with IRQ save (recommended)
 *
 * Disables interrupts and spins until the lock is acquired.
 * Returns the saved interrupt state which MUST be passed to
 * spinlock_release_irqrestore() when the lock is released.
 *
 * @param sl Pointer to the spinlock to acquire
 * @return Saved RFLAGS (pass to spinlock_release_irqrestore)
 */
static inline uint64_t spinlock_acquire_irqsave(Spinlock *sl)
{
    uint64_t flags = interrupts_save_disable();
    if (!sl)
        return flags;

    // Spin until we acquire the lock
    while (__sync_lock_test_and_set(&sl->locked, 1)) {
        // Spin with pause instruction to reduce bus contention
        asm volatile("pause" ::: "memory");
    }

    // Memory barrier
    asm volatile("" ::: "memory");
    return flags;
}

/**
 * @brief Release a spinlock with IRQ restore (recommended)
 *
 * Releases the lock and restores the interrupt state from the
 * flags value returned by spinlock_acquire_irqsave().
 *
 * @param sl Pointer to the spinlock to release
 * @param flags Saved RFLAGS from spinlock_acquire_irqsave()
 */
static inline void spinlock_release_irqrestore(Spinlock *sl, uint64_t flags)
{
    if (!sl) {
        interrupts_restore(flags);
        return;
    }
    // Memory barrier
    asm volatile("" ::: "memory");

    // Release the lock
    __sync_lock_release(&sl->locked);

    // Restore interrupt state
    interrupts_restore(flags);
}

/**
 * @brief Acquire a spinlock (raw — does NOT disable interrupts)
 *
 * Spins until the lock is acquired. The caller is responsible for
 * managing interrupt state (e.g., via interrupts_save_disable()).
 *
 * @param sl Pointer to the spinlock to acquire
 */
static inline void spinlock_acquire(Spinlock *sl)
{
    if (!sl)
        return;

    // Spin until we acquire the lock
    while (__sync_lock_test_and_set(&sl->locked, 1)) {
        asm volatile("pause" ::: "memory");
    }

    // Memory barrier
    asm volatile("" ::: "memory");
}

/**
 * @brief Release a spinlock (raw — does NOT restore interrupts)
 *
 * Releases the lock. The caller is responsible for restoring
 * interrupt state.
 *
 * @param sl Pointer to the spinlock to release
 */
static inline void spinlock_release(Spinlock *sl)
{
    if (!sl)
        return;
    // Memory barrier
    asm volatile("" ::: "memory");

    // Release the lock
    __sync_lock_release(&sl->locked);
}

/**
 * @brief Try to acquire a spinlock with IRQ save without blocking
 *
 * @param sl Pointer to the spinlock to try to acquire
 * @param out_flags Pointer to store the saved RFLAGS on success
 * @return true if the lock was acquired, false if it was already held
 */
static inline bool spinlock_try_acquire_irqsave(Spinlock *sl, uint64_t *out_flags)
{
    if (!sl)
        return false;
    uint64_t flags = interrupts_save_disable();

    if (__sync_lock_test_and_set(&sl->locked, 1) == 0) {
        if (out_flags)
            *out_flags = flags;
        asm volatile("" ::: "memory");
        return true;
    }

    // Failed to acquire - restore interrupts
    interrupts_restore(flags);
    return false;
}

/**
 * @brief Try to acquire a spinlock without blocking (raw)
 *
 * @param sl Pointer to the spinlock to try to acquire
 * @return true if the lock was acquired, false if it was already held
 */
static inline bool spinlock_try_acquire(Spinlock *sl)
{
    if (!sl)
        return false;

    if (__sync_lock_test_and_set(&sl->locked, 1) == 0) {
        asm volatile("" ::: "memory");
        return true;
    }

    return false;
}

/**
 * @brief Release a spinlock without restoring interrupts
 *
 * Releases the lock but does NOT restore the interrupt state.
 * This is useful for nested locking where interrupts should remain disabled.
 * Equivalent to spinlock_release() in the new API.
 *
 * @param sl Pointer to the spinlock to release
 */
static inline void spinlock_release_no_restore(Spinlock *sl)
{
    if (!sl)
        return;
    asm volatile("" ::: "memory");
    __sync_lock_release(&sl->locked);
}

/**
 * @brief Check if a spinlock is currently held
 * @param sl Pointer to the spinlock to check
 * @return true if the lock is held
 */
static inline bool spinlock_is_locked(Spinlock *sl)
{
    if (!sl)
        return false;
    return sl->locked != 0;
}
