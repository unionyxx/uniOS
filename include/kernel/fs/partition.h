#pragma once
#include <stdint.h>

struct PartitionScanEntry
{
    uint32_t partition_index;
    uint64_t start_lba;
    uint64_t block_count;
    char label[64];
};

struct PartitionGptInfo
{
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
};

int partition_parse_mbr_entries(const uint8_t *sector, PartitionScanEntry *out, int max_entries);
bool partition_parse_gpt_header(const uint8_t *sector, uint32_t block_size, PartitionGptInfo *out);
bool partition_parse_gpt_entry(const uint8_t *entry_bytes, uint32_t entry_size, uint32_t partition_index,
                               PartitionScanEntry *out);
void partition_scan_all();
