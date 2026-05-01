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

    spinlock_acquire(&dev_lock);
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
    spinlock_release(&dev_lock);
}

BlockDevice *block_dev_get(const char *name)
{
    if (!name)
        return nullptr;

    spinlock_acquire(&dev_lock);
    BlockDevice *current = dev_list;
    while (current) {
        if (kstring::strcmp(current->name, name) == 0) {
            spinlock_release(&dev_lock);
            return current;
        }
        current = current->next;
    }
    spinlock_release(&dev_lock);
    return nullptr;
}

BlockDevice *block_dev_first(void)
{
    spinlock_acquire(&dev_lock);
    BlockDevice *first = dev_list;
    spinlock_release(&dev_lock);
    return first;
}
