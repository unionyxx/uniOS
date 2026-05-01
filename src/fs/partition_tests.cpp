#include <kernel/fs/partition.h>
#include <kernel/ktest.h>
#include <libk/kstring.h>

namespace {

struct [[gnu::packed]] TestMbrPartitionEntry
{
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t first_lba;
    uint32_t sector_count;
};

struct [[gnu::packed]] TestGptHeader
{
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entries_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entries_crc32;
};

struct [[gnu::packed]] TestGptEntry
{
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
};

} // namespace

KTEST(partition_parse_mbr_primary_entries)
{
    uint8_t sector[512];
    kstring::zero_memory(sector, sizeof(sector));
    sector[510] = 0x55;
    sector[511] = 0xAA;

    auto *entries = reinterpret_cast<TestMbrPartitionEntry *>(sector + 446);
    entries[0].type = 0x0C;
    entries[0].first_lba = 2048;
    entries[0].sector_count = 4096;
    entries[1].type = 0xEE;
    entries[1].first_lba = 1;
    entries[1].sector_count = 100;
    entries[2].type = 0x83;
    entries[2].first_lba = 8192;
    entries[2].sector_count = 2048;

    PartitionScanEntry parsed[4];
    int count = partition_parse_mbr_entries(sector, parsed, 4);
    KTEST_EXPECT_EQ(count, 2);
    KTEST_EXPECT_EQ(parsed[0].partition_index, 1u);
    KTEST_EXPECT_EQ(parsed[0].start_lba, 2048u);
    KTEST_EXPECT_EQ(parsed[0].block_count, 4096u);
    KTEST_EXPECT_EQ(parsed[1].partition_index, 3u);
    KTEST_EXPECT_EQ(parsed[1].start_lba, 8192u);
    KTEST_EXPECT_EQ(parsed[1].block_count, 2048u);
}

KTEST(partition_parse_gpt_header_and_entry)
{
    uint8_t sector[512];
    kstring::zero_memory(sector, sizeof(sector));
    auto *header = reinterpret_cast<TestGptHeader *>(sector);
    header->signature = 0x5452415020494645ULL;
    header->partition_entries_lba = 2;
    header->partition_entry_count = 4;
    header->partition_entry_size = sizeof(TestGptEntry);

    PartitionGptInfo info = {};
    KTEST_EXPECT(partition_parse_gpt_header(sector, 512, &info));
    KTEST_EXPECT_EQ(info.entries_lba, 2u);
    KTEST_EXPECT_EQ(info.entry_count, 4u);
    KTEST_EXPECT_EQ(info.entry_size, (uint32_t)sizeof(TestGptEntry));

    uint8_t entry_bytes[128];
    kstring::zero_memory(entry_bytes, sizeof(entry_bytes));
    auto *entry = reinterpret_cast<TestGptEntry *>(entry_bytes);
    entry->type_guid[0] = 0xA2;
    entry->first_lba = 4096;
    entry->last_lba = 16383;
    entry->name[0] = 'D';
    entry->name[1] = 'A';
    entry->name[2] = 'T';
    entry->name[3] = 'A';

    PartitionScanEntry parsed = {};
    KTEST_EXPECT(partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry), 1, &parsed));
    KTEST_EXPECT_EQ(parsed.partition_index, 1u);
    KTEST_EXPECT_EQ(parsed.start_lba, 4096u);
    KTEST_EXPECT_EQ(parsed.block_count, 12288u);
    KTEST_EXPECT(kstring::strcmp(parsed.label, "DATA") == 0);
}
