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

uint32_t test_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Fills header_size and a valid header CRC so the parser accepts the header.
void finalize_gpt_header(TestGptHeader *header)
{
    header->header_size = sizeof(TestGptHeader);
    header->header_crc32 = 0;
    header->header_crc32 = test_crc32(reinterpret_cast<const uint8_t *>(header), header->header_size);
}

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

KTEST(partition_parse_mbr_rejects_malformed_entries)
{
    uint8_t sector[512];
    kstring::zero_memory(sector, sizeof(sector));
    PartitionScanEntry parsed[4];

    // Missing 0x55AA signature.
    KTEST_EXPECT_EQ(partition_parse_mbr_entries(sector, parsed, 4), 0);

    sector[510] = 0x55;
    sector[511] = 0xAA;
    auto *entries = reinterpret_cast<TestMbrPartitionEntry *>(sector + 446);

    // Extended partitions and LBA 0 (the MBR sector) must be skipped.
    entries[0].type = 0x05; // CHS extended
    entries[0].first_lba = 2048;
    entries[0].sector_count = 4096;
    entries[1].type = 0x0F; // LBA extended
    entries[1].first_lba = 8192;
    entries[1].sector_count = 4096;
    entries[2].type = 0x83;
    entries[2].first_lba = 0;
    entries[2].sector_count = 4096;
    entries[3].type = 0x83;
    entries[3].first_lba = 16384;
    entries[3].sector_count = 2048;
    KTEST_EXPECT_EQ(partition_parse_mbr_entries(sector, parsed, 4), 1);
    KTEST_EXPECT_EQ(parsed[0].start_lba, 16384u);

    // Buffer/output guards.
    KTEST_EXPECT_EQ(partition_parse_mbr_entries(nullptr, parsed, 4), 0);
    KTEST_EXPECT_EQ(partition_parse_mbr_entries(sector, nullptr, 4), 0);
    KTEST_EXPECT_EQ(partition_parse_mbr_entries(sector, parsed, 0), 0);
}

KTEST(partition_parse_gpt_header_rejects_malformed)
{
    uint8_t sector[512];
    kstring::zero_memory(sector, sizeof(sector));
    auto *header = reinterpret_cast<TestGptHeader *>(sector);
    header->signature = 0x5452415020494645ULL;
    header->partition_entries_lba = 2;
    header->partition_entry_count = 4;
    header->partition_entry_size = sizeof(TestGptEntry);
    finalize_gpt_header(header);

    PartitionGptInfo info = {};
    KTEST_EXPECT(partition_parse_gpt_header(sector, 512, &info));

    TestGptHeader bad;

    // A header whose CRC no longer matches its contents must be rejected even
    // when every structural field is otherwise valid.
    bad = *header;
    bad.partition_entries_lba = 40; // valid range, but CRC now stale
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    bad = *header;
    bad.signature = 0; // wrong magic
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    bad = *header;
    bad.partition_entries_lba = 1; // collides with MBR/GPT header
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    bad = *header;
    bad.partition_entry_count = 0;
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    bad = *header;
    bad.partition_entry_count = 100000; // unbounded-scan guard
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    bad = *header;
    bad.partition_entry_size = 64; // below the minimum entry size
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    bad = *header;
    bad.partition_entry_size = 1024; // larger than the block size
    kstring::memcpy(sector, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 512, &info));

    KTEST_EXPECT(!partition_parse_gpt_header(nullptr, 512, &info));
    KTEST_EXPECT(!partition_parse_gpt_header(sector, 256, &info)); // block too small
}

KTEST(partition_parse_gpt_entry_rejects_malformed)
{
    uint8_t entry_bytes[128];
    kstring::zero_memory(entry_bytes, sizeof(entry_bytes));
    auto *entry = reinterpret_cast<TestGptEntry *>(entry_bytes);
    entry->type_guid[0] = 0xA2;
    entry->first_lba = 4096;
    entry->last_lba = 8191;

    PartitionScanEntry parsed = {};
    KTEST_EXPECT(partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry), 1, &parsed));

    TestGptEntry bad;

    bad = *entry;
    kstring::zero_memory(bad.type_guid, sizeof(bad.type_guid)); // empty slot
    kstring::memcpy(entry_bytes, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry), 1, &parsed));

    bad = *entry;
    bad.first_lba = 8192;
    bad.last_lba = 4096; // inverted range
    kstring::memcpy(entry_bytes, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry), 1, &parsed));

    bad = *entry;
    bad.first_lba = 1; // inside MBR/GPT header area
    bad.last_lba = 4096;
    kstring::memcpy(entry_bytes, &bad, sizeof(bad));
    KTEST_EXPECT(!partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry), 1, &parsed));

    KTEST_EXPECT(!partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry) - 1, 1, &parsed));
    KTEST_EXPECT(!partition_parse_gpt_entry(entry_bytes, sizeof(TestGptEntry), 0, &parsed));
    KTEST_EXPECT(!partition_parse_gpt_entry(nullptr, sizeof(TestGptEntry), 1, &parsed));
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
    finalize_gpt_header(header);

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
