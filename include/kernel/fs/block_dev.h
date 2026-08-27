#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct BlockDevice
{
    const char *name;
    char model[64];
    char display_name[64];
    uint64_t block_size;
    uint64_t total_blocks;
    bool is_partition;
    bool has_partitions;
    uint32_t partition_index;
    uint64_t start_lba;
    struct BlockDevice *parent;

    // Returns number of blocks read/written, or -1 on error
    int64_t (*read_blocks)(struct BlockDevice *dev, uint64_t lba, uint32_t count, void *buffer);
    int64_t (*write_blocks)(struct BlockDevice *dev, uint64_t lba, uint32_t count, const void *buffer);
    // Commits the device's volatile write cache to media. Returns 0 on
    // success, -1 on error. NULL for devices without a volatile cache.
    int (*flush)(struct BlockDevice *dev);
    // Set by the driver when accepted data may still sit in the volatile
    // write cache; cleared by block_dev_flush once the cache is committed.
    bool cache_dirty;

    void *private_data;
    uint32_t registration_index;
    struct BlockDevice *next;
};

void block_dev_register(struct BlockDevice *dev);
struct BlockDevice *block_dev_get(const char *name);
struct BlockDevice *block_dev_first(void);

// Commits the device's volatile write cache if it holds unflushed writes.
// Returns 0 on success (or when there is nothing to flush), -1 on error.
int block_dev_flush(struct BlockDevice *dev);
// Flushes every registered block device. Intended for sync/shutdown paths.
void block_dev_flush_all(void);
