#pragma once
#include <stdint.h>
#include <stddef.h>

struct BlockDevice {
    const char* name;
    uint64_t block_size;
    uint64_t total_blocks;
    
    // Returns number of blocks read/written, or -1 on error
    int64_t (*read_blocks)(struct BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer);
    int64_t (*write_blocks)(struct BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer);
    
    void* private_data;
    struct BlockDevice* next;
};

void block_dev_register(struct BlockDevice* dev);
struct BlockDevice* block_dev_get(const char* name);
