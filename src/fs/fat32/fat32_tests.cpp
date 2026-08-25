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

namespace {

// Minimal valid geometry: 512 B/sector, 8 spc, 32 reserved, 2 FATs,
// 128 sectors/FAT, 131072 total sectors.
static void fill_valid_boot_sector(uint8_t *sector)
{
    kstring::zero_memory(sector, 512);
    sector[11] = 0x00;
    sector[12] = 0x02; // 512 bytes/sector
    sector[13] = 0x08; // sectors/cluster
    sector[14] = 0x20; // reserved sectors
    sector[16] = 0x02; // FAT count
    sector[36] = 0x80; // sectors per FAT
    sector[44] = 0x02; // root cluster
    sector[34] = 0x02; // total sectors = 131072
}

} // namespace

KTEST(fat32_parse_boot_sector_rejects_malformed_geometry)
{
    uint8_t sector[512];
    FAT32Filesystem fs = {};

    fill_valid_boot_sector(sector);
    sector[14] = 0x00; // no reserved sectors: FAT would cover the boot sector
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));

    fill_valid_boot_sector(sector);
    sector[13] = 0x03; // not a power of two
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));

    fill_valid_boot_sector(sector);
    sector[13] = 0xFF; // beyond the 128-sector cluster maximum
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));

    fill_valid_boot_sector(sector);
    sector[12] = 0x01; // 256 bytes/sector is unsupported
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));

    // reserved + fat_count * sectors_per_fat would wrap uint32 (2 * 0x80000001);
    // the parser must compute it in 64-bit and reject.
    fill_valid_boot_sector(sector);
    sector[36] = 0x01;
    sector[37] = 0x00;
    sector[38] = 0x00;
    sector[39] = 0x80; // sectors per FAT = 0x80000001
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));
}

KTEST(fat32_parse_boot_sector_rejects_out_of_range_root_cluster)
{
    uint8_t sector[512];
    fill_valid_boot_sector(sector);
    // cluster_count = (131072 - 288) / 8 = 16298; cluster 65535 does not exist.
    sector[44] = 0xFF;
    sector[45] = 0xFF;
    FAT32Filesystem fs = {};
    KTEST_EXPECT(!fat32_parse_boot_sector(sector, &fs));
}
