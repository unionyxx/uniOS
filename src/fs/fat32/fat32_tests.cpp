#include <kernel/fs/block_dev.h>
#include <kernel/fs/fat32.h>
#include <kernel/fs/vfs.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
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

namespace {

// RAM-backed block device so the write path can be exercised without real
// storage. Geometry: 256 sectors of 512 B, 8 spc (4 KiB clusters),
// 32 reserved sectors, 2 FATs of 2 sectors. Data starts at sector 36 and
// there are (256 - 36) / 8 = 27 clusters (numbers 2..28).
struct RamDisk
{
    BlockDevice dev;
    uint8_t *data;
    uint64_t reads;
    uint64_t writes;
};

constexpr uint64_t RAMDISK_SECTORS = 256;
constexpr uint32_t RAMDISK_SPC = 8;
constexpr uint32_t RAMDISK_RESERVED = 32;
constexpr uint32_t RAMDISK_FAT_SECTORS = 2;
constexpr uint32_t RAMDISK_DATA_START = RAMDISK_RESERVED + 2 * RAMDISK_FAT_SECTORS;

static int64_t ramdisk_read(BlockDevice *dev, uint64_t lba, uint32_t count, void *buffer)
{
    auto *disk = static_cast<RamDisk *>(dev->private_data);
    if (lba + count > RAMDISK_SECTORS)
        return -1;
    kstring::memcpy(buffer, disk->data + lba * 512, static_cast<uint64_t>(count) * 512);
    disk->reads += count;
    return count;
}

static int64_t ramdisk_write(BlockDevice *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    auto *disk = static_cast<RamDisk *>(dev->private_data);
    if (lba + count > RAMDISK_SECTORS)
        return -1;
    kstring::memcpy(disk->data + lba * 512, buffer, static_cast<uint64_t>(count) * 512);
    disk->writes += count;
    return count;
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static uint32_t fat_entry(const RamDisk *disk, uint32_t fat_copy, uint32_t cluster)
{
    const uint64_t sector = RAMDISK_RESERVED + fat_copy * RAMDISK_FAT_SECTORS + (cluster * 4) / 512;
    return get_le32(disk->data + sector * 512 + (cluster * 4) % 512) & 0x0FFFFFFF;
}

static void ramdisk_format(RamDisk *disk)
{
    kstring::zero_memory(disk->data, RAMDISK_SECTORS * 512);
    uint8_t *boot = disk->data;
    boot[510] = 0x55;
    boot[511] = 0xAA;
    boot[11] = 0x00;
    boot[12] = 0x02;        // 512 bytes/sector
    boot[13] = RAMDISK_SPC; // sectors/cluster
    boot[14] = (uint8_t)RAMDISK_RESERVED;
    boot[16] = 0x02; // FAT count
    put_le32(boot + 32, RAMDISK_SECTORS);
    put_le32(boot + 36, RAMDISK_FAT_SECTORS);
    put_le32(boot + 44, 2); // root cluster
    boot[48] = 0x01;        // FSInfo sector

    uint8_t *fsinfo = disk->data + 512;
    put_le32(fsinfo + 0, 0x41615252);
    put_le32(fsinfo + 484, 0x61417272);
    put_le32(fsinfo + 488, 27); // free cluster count
    put_le32(fsinfo + 492, 3);  // next free cluster
    put_le32(fsinfo + 508, 0xAA550000);

    for (uint32_t copy = 0; copy < 2; copy++) {
        uint8_t *fat = disk->data + (RAMDISK_RESERVED + copy * RAMDISK_FAT_SECTORS) * 512;
        put_le32(fat + 0, 0x0FFFFFF8);
        put_le32(fat + 4, 0x0FFFFFFF);
        put_le32(fat + 8, 0x0FFFFFFF); // root directory cluster 2
    }
}

static RamDisk *ramdisk_create()
{
    auto *disk = static_cast<RamDisk *>(malloc(sizeof(RamDisk)));
    if (!disk)
        return nullptr;
    *disk = {};
    disk->data = static_cast<uint8_t *>(malloc(RAMDISK_SECTORS * 512));
    if (!disk->data) {
        free(disk);
        return nullptr;
    }
    disk->dev.name = "ramtest";
    disk->dev.block_size = 512;
    disk->dev.total_blocks = RAMDISK_SECTORS;
    disk->dev.read_blocks = ramdisk_read;
    disk->dev.write_blocks = ramdisk_write;
    disk->dev.private_data = disk;
    ramdisk_format(disk);
    return disk;
}

static void ramdisk_destroy(RamDisk *disk)
{
    if (!disk)
        return;
    free(disk->data);
    free(disk);
}

} // namespace

KTEST(fat32_write_coalesces_aligned_sectors_and_skips_rmw)
{
    RamDisk *disk = ramdisk_create();
    KTEST_EXPECT(disk != nullptr);
    if (!disk)
        return;

    FAT32Filesystem fs = {};
    KTEST_EXPECT(fat32_init(&disk->dev, &fs));
    VNode *root = fat32_get_root(&fs);
    KTEST_EXPECT(root != nullptr);

    KTEST_EXPECT_EQ(root->ops->create(root, "TEST.TXT"), 0);
    VNode *file = root->ops->lookup(root, "TEST.TXT");
    KTEST_EXPECT(file != nullptr);

    // Two 4 KiB writes through the aligned path; each must extend the chain
    // by one cluster and land as whole-sector writes.
    uint8_t pattern_a[4096], pattern_b[4096], readback[8192];
    for (uint32_t i = 0; i < 4096; i++) {
        pattern_a[i] = (uint8_t)(i * 7 + 1);
        pattern_b[i] = (uint8_t)(i * 13 + 5);
    }
    KTEST_EXPECT_EQ(file->ops->write(file, pattern_a, 4096, 0, nullptr), 4096);
    KTEST_EXPECT_EQ(file->ops->write(file, pattern_b, 4096, 4096, nullptr), 4096);
    KTEST_EXPECT_EQ(file->size, 8192u);

    // Fresh allocations follow the FSInfo hint: contiguous clusters 3 and 4
    // in both FAT copies.
    for (uint32_t copy = 0; copy < 2; copy++) {
        KTEST_EXPECT_EQ(fat_entry(disk, copy, 3), 4u);
        KTEST_EXPECT(fat_entry(disk, copy, 4) >= 0x0FFFFFF8u);
    }

    // Data integrity across the read path.
    KTEST_EXPECT_EQ(file->ops->read(file, readback, 8192, 0, nullptr), 8192);
    KTEST_EXPECT(kstring::memcmp(readback, pattern_a, 4096) == 0);
    KTEST_EXPECT(kstring::memcmp(readback + 4096, pattern_b, 4096) == 0);

    // Aligned overwrite of existing data must not pre-read anything: no
    // partial sectors, no size change, no directory reload.
    const uint64_t reads_before = disk->reads;
    KTEST_EXPECT_EQ(file->ops->write(file, pattern_b, 4096, 0, nullptr), 4096);
    KTEST_EXPECT_EQ(disk->reads, reads_before);
    KTEST_EXPECT_EQ(file->ops->read(file, readback, 4096, 0, nullptr), 4096);
    KTEST_EXPECT(kstring::memcmp(readback, pattern_b, 4096) == 0);

    // Partial-sector writes still take the read-modify-write path and must
    // preserve the surrounding bytes.
    uint8_t patch[100];
    kstring::memset(patch, 0xCC, sizeof(patch));
    KTEST_EXPECT_EQ(file->ops->write(file, patch, sizeof(patch), 50, nullptr), (int64_t)sizeof(patch));
    KTEST_EXPECT_EQ(file->ops->read(file, readback, 512, 0, nullptr), 512);
    KTEST_EXPECT(kstring::memcmp(readback, pattern_b, 50) == 0);
    KTEST_EXPECT(kstring::memcmp(readback + 50, patch, sizeof(patch)) == 0);
    KTEST_EXPECT(kstring::memcmp(readback + 150, pattern_b + 150, 512 - 150) == 0);

    // Unaligned write crossing the 4 KiB cluster boundary into cluster 4.
    uint8_t cross[200];
    kstring::memset(cross, 0xDD, sizeof(cross));
    KTEST_EXPECT_EQ(file->ops->write(file, cross, sizeof(cross), 4000, nullptr), (int64_t)sizeof(cross));
    KTEST_EXPECT_EQ(file->ops->read(file, readback, sizeof(cross), 4000, nullptr), (int64_t)sizeof(cross));
    KTEST_EXPECT(kstring::memcmp(readback, cross, sizeof(cross)) == 0);

    vfs_close_vnode(file);
    vfs_close_vnode(root);
    ramdisk_destroy(disk);
}

KTEST(fat32_fsinfo_updates_lazily_on_sync)
{
    RamDisk *disk = ramdisk_create();
    KTEST_EXPECT(disk != nullptr);
    if (!disk)
        return;

    FAT32Filesystem fs = {};
    KTEST_EXPECT(fat32_init(&disk->dev, &fs));
    VNode *root = fat32_get_root(&fs);
    KTEST_EXPECT(root != nullptr);

    const uint8_t *fsinfo_sector = disk->data + 512;
    KTEST_EXPECT_EQ(get_le32(fsinfo_sector + 488), 27u);

    // Allocation moves the in-memory free count but must not rewrite FSInfo.
    KTEST_EXPECT_EQ(root->ops->create(root, "A.TXT"), 0);
    VNode *file = root->ops->lookup(root, "A.TXT");
    KTEST_EXPECT(file != nullptr);
    uint8_t byte = 0x42;
    KTEST_EXPECT_EQ(file->ops->write(file, &byte, 1, 0, nullptr), 1);
    KTEST_EXPECT_EQ(get_le32(fsinfo_sector + 488), 27u);

    // Sync persists the lazy state.
    KTEST_EXPECT(root->ops->sync != nullptr);
    KTEST_EXPECT_EQ(root->ops->sync(root), 0);
    KTEST_EXPECT_EQ(get_le32(fsinfo_sector + 488), 26u);
    KTEST_EXPECT_EQ(get_le32(fsinfo_sector + 492), 4u);

    vfs_close_vnode(file);
    vfs_close_vnode(root);
    ramdisk_destroy(disk);
}
