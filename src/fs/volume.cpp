#include <kernel/fs/storage_guard.h>
#include <kernel/fs/volume.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>

static constexpr int MAX_VOLUMES = 16;

struct VolumeRecord
{
    bool used;
    VolumeInfo info;
};

static VolumeRecord g_volumes[MAX_VOLUMES];
static Spinlock g_volume_lock = SPINLOCK_INIT;

void volume_reset()
{
    spinlock_acquire(&g_volume_lock);
    for (int i = 0; i < MAX_VOLUMES; i++) {
        g_volumes[i].used = false;
        g_volumes[i].info = {};
    }
    spinlock_release(&g_volume_lock);
}

bool volume_register(const char *display_name, const char *mount_path, const char *source_device, uint32_t flags)
{
    spinlock_acquire(&g_volume_lock);
    for (int i = 0; i < MAX_VOLUMES; i++) {
        if (!g_volumes[i].used) {
            g_volumes[i].used = true;
            g_volumes[i].info = {};
            kstring::strncpy(g_volumes[i].info.display_name, display_name ? display_name : "",
                             sizeof(g_volumes[i].info.display_name) - 1);
            kstring::strncpy(g_volumes[i].info.mount_path, mount_path ? mount_path : "",
                             sizeof(g_volumes[i].info.mount_path) - 1);
            kstring::strncpy(g_volumes[i].info.source_device, source_device ? source_device : "",
                             sizeof(g_volumes[i].info.source_device) - 1);
            g_volumes[i].info.flags = flags;
            spinlock_release(&g_volume_lock);
            return true;
        }
    }
    spinlock_release(&g_volume_lock);
    return false;
}

int volume_list(VolumeInfo *out, int max_count)
{
    if (!out || max_count <= 0)
        return 0;

    uint32_t storage_mode = storage_get_mode();
    spinlock_acquire(&g_volume_lock);
    int count = 0;
    for (int i = 0; i < MAX_VOLUMES && count < max_count; i++) {
        if (!g_volumes[i].used)
            continue;
        VolumeInfo info = g_volumes[i].info;
        if ((info.flags & VOLUME_FLAG_STORAGE_DEVICE) != 0) {
            if (storage_mode == STORAGE_MODE_OFF)
                continue;
            if (storage_mode != STORAGE_MODE_WRITABLE)
                info.flags &= ~static_cast<uint32_t>(VOLUME_FLAG_WRITABLE);
        }
        out[count++] = info;
    }
    spinlock_release(&g_volume_lock);
    return count;
}
