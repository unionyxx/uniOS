#pragma once
#include <stdint.h>

/**
 * @file spinlock.h
 * @brief Interrupt-safe spinlock primitives for kernel synchronization
 * 
 * Spinlocks provide mutual exclusion in the kernel. They disable interrupts
 * to prevent preemption while the lock is held, making them safe to use
 * in interrupt handlers.
 * 
 * Usage:
 *   Spinlock lock = SPINLOCK_INIT;
 *   spinlock_acquire(&lock);
 *   // critical section
 *   spinlock_release(&lock);
 */

struct Spinlock {
    volatile uint32_t locked;   // 0 = unlocked, 1 = locked
    uint64_t saved_flags;       // Saved RFLAGS for interrupt state
};

#define SPINLOCK_INIT {0, 0}

/**
 * @brief Save interrupt state and disable interrupts
 * @return The previous RFLAGS value
 */
static inline uint64_t interrupts_save_disable() {
    uint64_t flags;
    asm volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

/**
 * @brief Restore interrupt state from saved flags
 * @param flags Previously saved RFLAGS value
 */
static inline void interrupts_restore(uint64_t flags) {
    asm volatile(
        "push %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

/**
 * @brief Check if interrupts are currently enabled
 * @return true if interrupts are enabled
 */
static inline bool interrupts_enabled() {
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;  // IF flag is bit 9
}

/**
 * @brief Initialize a spinlock
 * @param sl Pointer to the spinlock to initialize
 */
static inline void spinlock_init(Spinlock* sl) {
    sl->locked = 0;
    sl->saved_flags = 0;
}

/**
 * @brief Acquire a spinlock (blocking)
 * 
 * Disables interrupts and spins until the lock is acquired.
 * The interrupt state is saved and will be restored when the lock is released.
 * 
 * @param sl Pointer to the spinlock to acquire
 */
static inline void spinlock_acquire(Spinlock* sl) {
    // Save flags and disable interrupts first
    uint64_t flags = interrupts_save_disable();
    
    // Spin until we acquire the lock
    while (__sync_lock_test_and_set(&sl->locked, 1)) {
        // Spin with pause instruction to reduce bus contention
        asm volatile("pause" ::: "memory");
    }
    
    // Store saved flags
    sl->saved_flags = flags;
    
    // Memory barrier
    asm volatile("" ::: "memory");
}

/**
 * @brief Try to acquire a spinlock without blocking
 * 
 * @param sl Pointer to the spinlock to try to acquire
 * @return true if the lock was acquired, false if it was already held
 */
static inline bool spinlock_try_acquire(Spinlock* sl) {
    uint64_t flags = interrupts_save_disable();
    
    if (__sync_lock_test_and_set(&sl->locked, 1) == 0) {
        sl->saved_flags = flags;
        asm volatile("" ::: "memory");
        return true;
    }
    
    // Failed to acquire - restore interrupts
    interrupts_restore(flags);
    return false;
}

/**
 * @brief Release a spinlock
 * 
 * Releases the lock and restores the interrupt state that was saved
 * when the lock was acquired.
 * 
 * @param sl Pointer to the spinlock to release
 */
static inline void spinlock_release(Spinlock* sl) {
    // Memory barrier
    asm volatile("" ::: "memory");
    
    // Save flags before releasing (so we have them even after unlock)
    uint64_t flags = sl->saved_flags;
    
    // Release the lock
    __sync_lock_release(&sl->locked);
    
    // Restore interrupt state
    interrupts_restore(flags);
}

/**
 * @brief Check if a spinlock is currently held
 * @param sl Pointer to the spinlock to check
 * @return true if the lock is held
 */
static inline bool spinlock_is_locked(Spinlock* sl) {
    return sl->locked != 0;
}
