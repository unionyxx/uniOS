#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <kernel/time/timer.h>
#include <kernel/user_ptr.h>
#include <libk/kstring.h>

#define STAC()                                                                                                         \
    do {                                                                                                               \
        if (g_cpu_features.has_smap)                                                                                   \
            asm volatile("stac" ::: "memory");                                                                         \
    } while (0)

#define CLAC()                                                                                                         \
    do {                                                                                                               \
        if (g_cpu_features.has_smap)                                                                                   \
            asm volatile("clac" ::: "memory");                                                                         \
    } while (0)

FutexBucket g_futex_table[FUTEX_HASH_SIZE];

void futex_init()
{
    for (size_t i = 0; i < FUTEX_HASH_SIZE; i++) {
        spinlock_init(&g_futex_table[i].lock);
        g_futex_table[i].wait_queue = {nullptr, nullptr};
    }
}

static inline uint32_t futex_hash(uint64_t phys_addr)
{
    return (phys_addr >> 12) % FUTEX_HASH_SIZE;
}

// Read the futex word with the VMA pinned: munmap removes the VMA metadata
// under vma_lock_ptr before it clears any PTEs, so holding the lock across
// lookup + read means the page cannot disappear between translation and the
// user access (the old translate-then-read window faulted unfaultably in
// kernel mode when a sibling thread raced an munmap).
static bool futex_read_user_value(Process *p, volatile uint32_t *uaddr, uint32_t *out)
{
    const uint64_t addr = reinterpret_cast<uint64_t>(uaddr);
    uint64_t flags = spinlock_acquire_irqsave(p->vma_lock_ptr);
    VMA *vma = vma_find(p->vmalist->head, addr);
    if (!vma || vma->start > addr || vma->end < addr + sizeof(uint32_t)) {
        spinlock_release_irqrestore(p->vma_lock_ptr, flags);
        return false;
    }
    STAC();
    *out = *uaddr;
    CLAC();
    spinlock_release_irqrestore(p->vma_lock_ptr, flags);
    return true;
}

void futex_notify_freed_frames(const uint64_t *frames, size_t count)
{
    if (!frames || count == 0)
        return;
    for (size_t i = 0; i < count; i++) {
        FutexBucket *bucket = &g_futex_table[futex_hash(frames[i])];
        uint64_t flags = spinlock_acquire_irqsave(&bucket->lock);
        scheduler_wake_waiters_under_leaf(&bucket->wait_queue, 0);
        spinlock_release_irqrestore(&bucket->lock, flags);
    }
}

int64_t sys_futex(volatile uint32_t *uaddr, int op, uint32_t val, uint64_t timeout_ms)
{
    // Buckets are initialized at boot (kmain, single-core). A lazy init here
    // raced on SMP: two cores could initialize a bucket lock while a third
    // was already acquiring it.

    if (reinterpret_cast<uintptr_t>(uaddr) % sizeof(uint32_t) != 0) {
        return -22; // EINVAL: Unaligned access
    }

    // Kernel-space addresses must never become futex keys: the kernel half
    // is mapped in every page table, so a kernel pointer would resolve to a
    // shared, attacker-chosen bucket (a comparison oracle and a socket for
    // parking unrelated waiters).
    if (reinterpret_cast<uintptr_t>(uaddr) >= USER_SPACE_MAX)
        return -14; // -EFAULT

    Process *current = process_get_current();
    if (!current) {
        return -1;
    }

    // virt -> phys for the futex address
    uint64_t phys_addr =
        vmm_virt_to_phys_in(current->page_table, reinterpret_cast<uint64_t>(const_cast<uint32_t *>(uaddr)));
    if (phys_addr == 0)
        return -14; // -EFAULT

    uint32_t hash = futex_hash(phys_addr);
    FutexBucket *bucket = &g_futex_table[hash];

    if (op == FUTEX_WAIT) {
        uint64_t flags = spinlock_acquire_irqsave(&bucket->lock);

        uint32_t current_val = 0;
        if (!futex_read_user_value(current, uaddr, &current_val)) {
            spinlock_release_irqrestore(&bucket->lock, flags);
            return -14; // -EFAULT: unmapped while we held the bucket lock
        }

        if (current_val != val) {
            spinlock_release_irqrestore(&bucket->lock, flags);
            return -11; // EAGAIN
        }

        if (scheduler_fatal_signal_pending(current)) {
            spinlock_release_irqrestore(&bucket->lock, flags);
            return -4; // -EINTR: do not sleep through a fatal signal
        }

        if (timeout_ms != 0) {
            // Wait queues have no native timeout: arm the tick-driven
            // deadline walker. Round UP to whole ticks and saturate: a
            // truncated sub-tick delay would arm a deadline that is
            // already expired at registration, and an unsaturated
            // multiply can wrap it arbitrarily far into the past. Both
            // end a wait that never really timed out.
            const uint64_t freq = timer_get_frequency() ? static_cast<uint64_t>(timer_get_frequency()) : 1000;
            uint64_t ticks;
            if (timeout_ms > (UINT64_MAX - 999) / freq)
                ticks = UINT64_MAX;
            else
                ticks = (timeout_ms * freq + 999) / 1000;
            if (ticks == 0)
                ticks = 1;
            const uint64_t now = timer_get_ticks();
            const uint64_t deadline = (ticks > UINT64_MAX - now) ? UINT64_MAX : now + ticks;

            if (!scheduler_note_wake_deadline(current, deadline)) {
                spinlock_release_irqrestore(&bucket->lock, flags);
                // The fixed timed-wait table is full: an honest error
                // beats a wait that silently lost its timeout (an
                // unbounded hang nobody can diagnose in release builds).
                return -28; // -ENOSPC
            }
            scheduler_wait(&bucket->wait_queue, &bucket->lock);
            spinlock_release_irqrestore(&bucket->lock, flags);

            // The wait ended (wake, timeout or signal): drop the
            // registration before returning. A lingering entry would let
            // the walker mark this process's next blocked wait as timed
            // out, or a recycled Process at this address inherit it.
            scheduler_clear_wake_deadline(current);

            if (current->timed_wake) {
                current->timed_wake = false;
                return -110; // -ETIMEDOUT
            }
        } else {
            scheduler_wait(&bucket->wait_queue, &bucket->lock);
            spinlock_release_irqrestore(&bucket->lock, flags);
        }

        // Woken by a signal delivery rather than a futex wake: report the
        // interruption instead of a spurious success.
        if (scheduler_fatal_signal_pending(current))
            return -4; // -EINTR

        return 0;
    } else if (op == FUTEX_WAKE) {
        uint64_t flags = spinlock_acquire_irqsave(&bucket->lock);
        int woken = scheduler_wake_waiters_under_leaf(&bucket->wait_queue, val);
        spinlock_release_irqrestore(&bucket->lock, flags);
        return woken;
    }

    return -22; // -EINVAL
}
