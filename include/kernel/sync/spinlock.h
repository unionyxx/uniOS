#pragma once
#include <stdint.h>
#include <stdbool.h>

void preempt_disable();
void preempt_enable();

struct Spinlock
{
    volatile uint32_t locked;
};

#define SPINLOCK_INIT {0}

/* Save RFLAGS and disable interrupts on local core */
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

/* Restore RFLAGS and interrupt state on local core */
static inline void interrupts_restore(uint64_t flags)
{
    asm volatile("push %0\n\t"
                 "popfq"
                 :
                 : "r"(flags)
                 : "memory", "cc");
}

/* Check if interrupts are enabled (IF flag, bit 9) */
static inline bool interrupts_enabled()
{
    uint64_t flags;
    asm volatile("pushfq; pop %0" : "=r"(flags));
    return (flags & 0x200) != 0;
}

static inline void spinlock_init(Spinlock *sl)
{
    if (!sl)
        return;
    sl->locked = 0;
}

/* Acquire lock, disabling interrupts and preemption on local core */
static inline uint64_t spinlock_acquire_irqsave(Spinlock *sl)
{
    uint64_t flags = interrupts_save_disable();
    preempt_disable();
    if (!sl)
        return flags;

    while (__sync_lock_test_and_set(&sl->locked, 1)) {
        while (sl->locked) {
            asm volatile("pause" ::: "memory");
        }
    }

    asm volatile("" ::: "memory");
    return flags;
}

/* Release lock, restoring preemption and interrupts on local core */
static inline void spinlock_release_irqrestore(Spinlock *sl, uint64_t flags)
{
    if (!sl) {
        preempt_enable();
        interrupts_restore(flags);
        return;
    }
    asm volatile("" ::: "memory");
    __sync_lock_release(&sl->locked);
    preempt_enable();
    interrupts_restore(flags);
}

/* Acquire raw lock and disable preemption (interrupt state unchanged) */
static inline void spinlock_acquire(Spinlock *sl)
{
    preempt_disable();
    if (!sl)
        return;

    while (__sync_lock_test_and_set(&sl->locked, 1)) {
        while (sl->locked) {
            asm volatile("pause" ::: "memory");
        }
    }

    asm volatile("" ::: "memory");
}

/* Release raw lock and restore preemption */
static inline void spinlock_release(Spinlock *sl)
{
    if (!sl) {
        preempt_enable();
        return;
    }
    asm volatile("" ::: "memory");
    __sync_lock_release(&sl->locked);
    preempt_enable();
}

/* Attempt non-blocking acquire with IRQ save */
static inline bool spinlock_try_acquire_irqsave(Spinlock *sl, uint64_t *out_flags)
{
    if (!sl)
        return false;
    uint64_t flags = interrupts_save_disable();

    if (__sync_lock_test_and_set(&sl->locked, 1) == 0) {
        preempt_disable();
        if (out_flags)
            *out_flags = flags;
        asm volatile("" ::: "memory");
        return true;
    }

    interrupts_restore(flags);
    return false;
}

/* Attempt non-blocking acquire without disabling interrupts */
static inline bool spinlock_try_acquire(Spinlock *sl)
{
    if (!sl)
        return false;

    if (__sync_lock_test_and_set(&sl->locked, 1) == 0) {
        preempt_disable();
        asm volatile("" ::: "memory");
        return true;
    }

    return false;
}

/* Release raw lock without restoring interrupts */
static inline void spinlock_release_no_restore(Spinlock *sl)
{
    if (!sl) {
        preempt_enable();
        return;
    }
    asm volatile("" ::: "memory");
    __sync_lock_release(&sl->locked);
    preempt_enable();
}

static inline bool spinlock_is_locked(const Spinlock *sl)
{
    if (!sl)
        return false;
    return sl->locked != 0;
}
