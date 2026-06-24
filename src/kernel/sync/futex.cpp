#include <kernel/sync/futex.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/cpu.h>
#include <kernel/scheduler.h>
#include <libk/kstring.h>

#define STAC() \
    do { \
        if (g_cpu_features.has_smap) \
            asm volatile("stac" ::: "memory"); \
    } while (0)

#define CLAC() \
    do { \
        if (g_cpu_features.has_smap) \
            asm volatile("clac" ::: "memory"); \
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

int64_t sys_futex(volatile uint32_t *uaddr, int op, uint32_t val)
{
    static bool futex_inited = false;
    if (!futex_inited) {
        futex_init();
        futex_inited = true;
    }

    if (reinterpret_cast<uintptr_t>(uaddr) % sizeof(uint32_t) != 0) {
        return -22; // EINVAL: Unaligned access
    }

    Process *current = process_get_current();
    if (!current) {
        return -1;
    }

    // virt -> phys for the futex address
    uint64_t phys_addr = vmm_virt_to_phys_in(current->page_table, reinterpret_cast<uint64_t>(const_cast<uint32_t *>(uaddr)));
    if (phys_addr == 0)
        return -14; // -EFAULT

    uint32_t hash = futex_hash(phys_addr);
    FutexBucket *bucket = &g_futex_table[hash];

    if (op == FUTEX_WAIT) {
        uint64_t flags = spinlock_acquire_irqsave(&bucket->lock);

        STAC();
        uint32_t current_val = *uaddr;
        CLAC();

        if (current_val != val) {
            spinlock_release_irqrestore(&bucket->lock, flags);
            return -11; // EAGAIN
        }

        scheduler_wait(&bucket->wait_queue, &bucket->lock);
        spinlock_release_irqrestore(&bucket->lock, flags);
        return 0;
    }
    else if (op == FUTEX_WAKE) {
        uint64_t flags = spinlock_acquire_irqsave(&bucket->lock);

        int woken = 0;
        Process *curr = bucket->wait_queue.head;
        while (curr && woken < static_cast<int>(val)) {
            Process *next = curr->queue_next;
            scheduler_wake_process(curr);
            woken++;
            curr = next;
        }

        spinlock_release_irqrestore(&bucket->lock, flags);
        return woken;
    }

    return -22; // -EINVAL
}
