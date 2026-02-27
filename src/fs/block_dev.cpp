#include <kernel/fs/block_dev.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>

static BlockDevice* dev_list = nullptr;
static Spinlock dev_lock = SPINLOCK_INIT;

void block_dev_register(BlockDevice* dev) {
    if (!dev) return;
    
    spinlock_acquire(&dev_lock);
    dev->next = dev_list;
    dev_list = dev;
    spinlock_release(&dev_lock);
}

BlockDevice* block_dev_get(const char* name) {
    if (!name) return nullptr;
    
    spinlock_acquire(&dev_lock);
    BlockDevice* current = dev_list;
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
