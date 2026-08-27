#pragma once
#include <kernel/fs/block_dev.h>
#include <kernel/sync/spinlock.h>
#include <stdint.h>

struct FAT32Filesystem
{
    BlockDevice *dev;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_dir_cluster;
    uint32_t fsinfo_sector;
    uint32_t total_sectors;
    uint32_t cluster_count;
    uint32_t next_free_cluster;
    uint32_t free_cluster_count;
    // Set when in-memory free-space state diverges from the on-disk FSInfo
    // sector; the sector is rewritten lazily on sync instead of on every
    // cluster allocation/free.
    bool fsinfo_dirty;
    // Serializes every FAT/directory mutation (and the read-modify-write
    // walks behind them). Zero-initialized mounts are valid: {0} ==
    // SPINLOCK_INIT.
    Spinlock lock;
    char volume_label[64];
};

struct VNode;

bool fat32_parse_boot_sector(const uint8_t *boot_sector, FAT32Filesystem *fs_out);
bool fat32_name_requires_lfn(const char *name);
void fat32_format_short_name(const char *name, uint32_t suffix, uint8_t out[11]);
bool fat32_init(BlockDevice *dev, FAT32Filesystem *fs_out);

VNode *fat32_get_root(FAT32Filesystem *fs);

// Returns the block device backing a FAT32 file vnode, or nullptr.
BlockDevice *fat32_vnode_device(VNode *node);
