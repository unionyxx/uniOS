#include <kernel/fs/storage_guard.h>
#include <kernel/sync/spinlock.h>
#include <uapi/fs.h>

static Spinlock g_storage_guard_lock = SPINLOCK_INIT;
static uint32_t g_storage_mode = STORAGE_MODE_WRITABLE;

uint32_t storage_get_mode()
{
    spinlock_acquire(&g_storage_guard_lock);
    uint32_t mode = g_storage_mode;
    spinlock_release(&g_storage_guard_lock);
    return mode;
}

void storage_set_mode(uint32_t mode)
{
    spinlock_acquire(&g_storage_guard_lock);
    if (mode > STORAGE_MODE_WRITABLE)
        mode = STORAGE_MODE_READ_ONLY;
    g_storage_mode = mode;
    spinlock_release(&g_storage_guard_lock);
}

bool storage_reads_allowed()
{
    return storage_get_mode() != STORAGE_MODE_OFF;
}

bool storage_writes_allowed()
{
    return storage_get_mode() == STORAGE_MODE_WRITABLE;
}
