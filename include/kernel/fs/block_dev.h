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

    void *private_data;
    uint32_t registration_index;
    struct BlockDevice *next;
};

void block_dev_register(struct BlockDevice *dev);
struct BlockDevice *block_dev_get(const char *name);
struct BlockDevice *block_dev_first(void);
