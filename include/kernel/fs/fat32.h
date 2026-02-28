#pragma once
#include <stdint.h>
#include <kernel/fs/block_dev.h>

struct FAT32Filesystem {
    BlockDevice* dev;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_dir_cluster;
};

struct VNode;

// Initialize FAT32 on a block device
bool fat32_init(BlockDevice* dev, FAT32Filesystem* fs_out);

// Get root VNode for VFS integration
VNode* fat32_get_root(FAT32Filesystem* fs);
