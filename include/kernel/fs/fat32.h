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

// Initialize FAT32 on a block device
bool fat32_init(BlockDevice* dev, FAT32Filesystem* fs_out);

// Read a file from FAT32 (simplified)
int64_t fat32_read_file(FAT32Filesystem* fs, const char* path, void* buffer, uint32_t buffer_size);
