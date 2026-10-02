#include <kernel/user_ptr.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/sync/spinlock.h>

[[nodiscard]] bool validate_user_ptr(const void *ptr, size_t size, bool write)
{
    const uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    if (addr == 0 || addr >= USER_SPACE_MAX)
        return false;
    if (size == 0)
        return true;

    const uint64_t end = addr + size;
    if (end < addr || end > USER_SPACE_MAX)
        return false;

    Process *p = process_get_current();
    if (!p)
        return false;

    // Threads share the leader's VMA list: always lock through vma_lock_ptr,
    // never the embedded lock, which is private to each thread's Process.
    uint64_t sl_flags = spinlock_acquire_irqsave(p->vma_lock_ptr);
    uint64_t current = addr;
    while (current < end) {
        VMA *vma = vma_find(p->vma_list, current);
        if (!vma) {
            spinlock_release_irqrestore(p->vma_lock_ptr, sl_flags);
            return false;
        }
        if (write && !(vma->flags & PTE_WRITABLE)) {
            spinlock_release_irqrestore(p->vma_lock_ptr, sl_flags);
            return false;
        }
        current = vma->end;
    }
    spinlock_release_irqrestore(p->vma_lock_ptr, sl_flags);
    return true;
}
