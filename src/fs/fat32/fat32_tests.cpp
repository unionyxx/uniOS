#include <kernel/fs/fat32.h>
#include <kernel/ktest.h>
#include <libk/kstring.h>

KTEST(fat32_parse_boot_sector_accepts_valid_sector)
{
    uint8_t sector[512];
    kstring::zero_memory(sector, sizeof(sector));
    sector[11] = 0x00;
    sector[12] = 0x02; // 512 bytes/sector
    sector[13] = 0x08; // sectors/cluster
    sector[14] = 0x20; // reserved sectors low
    sector[16] = 0x02; // FAT count
    sector[36] = 0x80; // sectors per fat
    sector[37] = 0x00;
    sector[44] = 0x02; // root cluster
    sector[48] = 0x01; // fsinfo
    sector[32] = 0x00;
    sector[33] = 0x00;
    sector[34] = 0x02; // total sectors = 131072
    kstring::memcpy(&sector[71], "UNI_OS     ", 11);

    FAT32Filesystem fs = {};
    KTEST_EXPECT(fat32_parse_boot_sector(sector, &fs));
    KTEST_EXPECT_EQ(fs.bytes_per_sector, 512u);
    KTEST_EXPECT_EQ(fs.sectors_per_cluster, 8u);
    KTEST_EXPECT_EQ(fs.fat_count, 2u);
    KTEST_EXPECT_EQ(fs.root_dir_cluster, 2u);
    KTEST_EXPECT(kstring::strcmp(fs.volume_label, "UNI_OS") == 0);
}

KTEST(fat32_parse_boot_sector_rejects_invalid_cluster_size)
{
    uint8_t sector[512];
    kstring::zero_memory(sector, sizeof(sector));
    sector[11] = 0x00;
    sector[12] = 0x02;
    sector[13] = 0x00;
    sector[16] = 0x02;
    sector[36] = 0x20;
    sector[44] = 0x02;

    FAT32Filesystem fs = {};
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));
}

KTEST(fat32_format_short_name_handles_plain_and_tilde_forms)
{
    uint8_t short_name[11];
    fat32_format_short_name("notes.txt", 0, short_name);
    KTEST_EXPECT(kstring::memcmp(short_name, "NOTES   TXT", 11) == 0);

    fat32_format_short_name("longdocument.txt", 3, short_name);
    KTEST_EXPECT(kstring::memcmp(short_name, "LONGDO~3TXT", 11) == 0);
    KTEST_EXPECT(fat32_name_requires_lfn("longdocument.txt"));
    KTEST_EXPECT(!fat32_name_requires_lfn("README.TXT"));
}
