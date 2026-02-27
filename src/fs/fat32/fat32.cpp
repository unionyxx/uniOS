#include <kernel/fs/fat32.h>
#include <kernel/debug.h>
#include <libk/kstring.h>

// Boot sector layout (partial)
struct FAT32BootSector {
    uint8_t  jmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t dir_entries;
    uint16_t total_sectors_16;
    uint8_t  media_desc;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
} __attribute__((packed));

bool fat32_init(BlockDevice* dev, FAT32Filesystem* fs_out) {
    if (!dev || !fs_out) return false;

    uint8_t boot_sector[512];
    
    // Read the first sector
    if (dev->read_blocks(dev, 0, 1, boot_sector) < 0) {
        DEBUG_ERROR("FAT32: Failed to read boot sector from %s", dev->name);
        return false;
    }

    FAT32BootSector* bs = (FAT32BootSector*)boot_sector;

    // Validate (rough check)
    if (bs->bytes_per_sector != 512 && bs->bytes_per_sector != 4096) {
        DEBUG_ERROR("FAT32: Invalid bytes per sector: %d", bs->bytes_per_sector);
        return false;
    }

    fs_out->dev = dev;
    fs_out->bytes_per_sector = bs->bytes_per_sector;
    fs_out->sectors_per_cluster = bs->sectors_per_cluster;
    fs_out->reserved_sectors = bs->reserved_sectors;
    fs_out->fat_count = bs->fat_count;
    fs_out->sectors_per_fat = bs->sectors_per_fat_32;
    fs_out->root_dir_cluster = bs->root_cluster;

    DEBUG_INFO("FAT32: Initialized on %s (Cluster size: %d bytes)", 
               dev->name, fs_out->bytes_per_sector * fs_out->sectors_per_cluster);
               
    return true;
}

int64_t fat32_read_file(FAT32Filesystem* fs, const char* path, void* buffer, uint32_t buffer_size) {
    if (!fs || !path || !buffer) return -1;
    // For now, this is just a skeleton. A full implementation would traverse the
    // directory tree and read the file's clusters.
    DEBUG_WARN("FAT32: Reading file %s is not yet implemented.", path);
    return -1;
}
