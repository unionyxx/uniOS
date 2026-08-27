#include <kernel/fs/block_dev.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>

static BlockDevice *dev_list = nullptr;
static Spinlock dev_lock = SPINLOCK_INIT;
static uint32_t g_next_registration_index = 0;

void block_dev_register(BlockDevice *dev)
{
    if (!dev)
        return;

    uint64_t flags = spinlock_acquire_irqsave(&dev_lock);
    dev->registration_index = g_next_registration_index++;
    dev->next = nullptr;
    if (!dev_list) {
        dev_list = dev;
    } else {
        BlockDevice *tail = dev_list;
        while (tail->next)
            tail = tail->next;
        tail->next = dev;
    }
    spinlock_release_irqrestore(&dev_lock, flags);
}

BlockDevice *block_dev_get(const char *name)
{
    if (!name)
        return nullptr;

    uint64_t flags = spinlock_acquire_irqsave(&dev_lock);
    BlockDevice *current = dev_list;
    while (current) {
        if (kstring::strcmp(current->name, name) == 0) {
            spinlock_release_irqrestore(&dev_lock, flags);
            return current;
        }
        current = current->next;
    }
    spinlock_release_irqrestore(&dev_lock, flags);
    return nullptr;
}

BlockDevice *block_dev_first(void)
{
    uint64_t flags = spinlock_acquire_irqsave(&dev_lock);
    BlockDevice *first = dev_list;
    spinlock_release_irqrestore(&dev_lock, flags);
    return first;
}

int block_dev_flush(BlockDevice *dev)
{
    if (!dev || !dev->flush)
        return 0;
    if (!dev->cache_dirty)
        return 0;
    int res = dev->flush(dev);
    if (res == 0)
        dev->cache_dirty = false;
    return res;
}

void block_dev_flush_all(void)
{
    // Snapshot the head under the lock, then flush with the lock dropped:
    // a device flush is real I/O and must not run with IRQs masked. The list
    // is append-only, so walking from a snapshotted head is safe even if a
    // new device registers mid-iteration (it simply misses this pass).
    uint64_t flags = spinlock_acquire_irqsave(&dev_lock);
    BlockDevice *head = dev_list;
    spinlock_release_irqrestore(&dev_lock, flags);

    for (BlockDevice *dev = head; dev; dev = dev->next)
        block_dev_flush(dev);
}
