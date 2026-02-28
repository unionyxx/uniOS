#include <kernel/fs/fat32.h>
#include <kernel/fs/vfs.h>
#include <kernel/syscall.h>
#include <kernel/debug.h>
#include <libk/kstring.h>
#include <kernel/mm/heap.h>
#include <libk/kstd.h>

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
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
} __attribute__((packed));

struct FAT32DirEntry {
    char     name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t cluster_high;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed));

struct FAT32Node {
    FAT32Filesystem* fs;
    uint64_t entry_sector;
    uint32_t entry_idx;
};

static uint64_t cluster_to_lba(FAT32Filesystem* fs, uint32_t cluster) {
    uint64_t data_start = fs->reserved_sectors + (fs->fat_count * fs->sectors_per_fat);
    return data_start + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t fat_next_cluster(FAT32Filesystem* fs, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint64_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t entry_offset = fat_offset % fs->bytes_per_sector;
    
    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) return 0x0FFFFFFF; // Error
    
    if (fs->dev->read_blocks(fs->dev, fat_sector, 1, sector_buf.get()) < 0) {
        return 0x0FFFFFFF; // Error
    }
    
    return (*(uint32_t*)(sector_buf.get() + entry_offset)) & 0x0FFFFFFF;
}

static void fat32_write_entry(FAT32Filesystem* fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;
    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) return;

    for (uint32_t fat_num = 0; fat_num < fs->fat_count; fat_num++) {
        uint64_t sector = fs->reserved_sectors + (fat_num * fs->sectors_per_fat) + (fat_offset / fs->bytes_per_sector);
        if (fs->dev->read_blocks(fs->dev, sector, 1, sector_buf.get()) < 0) continue;
        
        uint32_t existing = *(uint32_t*)(sector_buf.get() + sector_offset);
        uint32_t new_val = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
        *(uint32_t*)(sector_buf.get() + sector_offset) = new_val;
        
        fs->dev->write_blocks(fs->dev, sector, 1, sector_buf.get());
    }
}

static uint32_t fat32_allocate_cluster(FAT32Filesystem* fs, uint32_t last_cluster) {
    uint64_t data_start = fs->reserved_sectors + (fs->fat_count * fs->sectors_per_fat);
    uint32_t data_sectors = fs->total_sectors - data_start;
    uint32_t total_clusters = data_sectors / fs->sectors_per_cluster;
    
    for (uint32_t cluster = 2; cluster < total_clusters + 2; cluster++) {
        if (fat_next_cluster(fs, cluster) == 0) {
            fat32_write_entry(fs, cluster, 0x0FFFFFFF); // EOF
            if (last_cluster >= 2) {
                fat32_write_entry(fs, last_cluster, cluster);
            }
            return cluster;
        }
    }
    return 0x0FFFFFFF; // Full
}

static void fat32_update_dir_entry_size(FAT32Filesystem* fs, VNode* node, uint32_t new_size) {
    FAT32Node* node_data = (FAT32Node*)node->fs_data;
    if (!node_data || node_data->entry_sector == 0) return;

    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) return;

    if (fs->dev->read_blocks(fs->dev, node_data->entry_sector, 1, sector_buf.get()) < 0) return;

    FAT32DirEntry* entry = (FAT32DirEntry*)(sector_buf.get()) + node_data->entry_idx;
    entry->size = new_size;

    fs->dev->write_blocks(fs->dev, node_data->entry_sector, 1, sector_buf.get());
}

static void fat32_update_dir_entry_cluster(FAT32Filesystem* fs, VNode* node, uint32_t cluster) {
    FAT32Node* node_data = (FAT32Node*)node->fs_data;
    if (!node_data || node_data->entry_sector == 0) return;

    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) return;

    if (fs->dev->read_blocks(fs->dev, node_data->entry_sector, 1, sector_buf.get()) < 0) return;

    FAT32DirEntry* entry = (FAT32DirEntry*)(sector_buf.get()) + node_data->entry_idx;
    entry->cluster_low = cluster & 0xFFFF;
    entry->cluster_high = (cluster >> 16) & 0xFFFF;

    fs->dev->write_blocks(fs->dev, node_data->entry_sector, 1, sector_buf.get());
}

static void fat32_vfs_close(VNode* node) {
    if (node->fs_data) {
        free(node->fs_data);
        node->fs_data = nullptr;
    }
}

static int64_t fat32_vfs_read(VNode* node, void* buf, uint64_t size, uint64_t offset, FileDescriptor* fd) {
    FAT32Node* node_data = (FAT32Node*)node->fs_data;
    if (!node_data) return -1;
    FAT32Filesystem* fs = node_data->fs;
    if (!fs) return -1;
    
    if (offset >= node->size) return 0;
    uint64_t to_read = (size < node->size - offset) ? size : node->size - offset;
    
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    uint32_t cluster = 0;
    uint64_t clusters_to_skip = 0;

    if (fd && fd->last_cluster != 0 && offset >= fd->last_offset) {
        cluster = fd->last_cluster;
        clusters_to_skip = (offset - fd->last_offset) / cluster_size;
    } else {
        cluster = (uint32_t)node->inode_id;
        clusters_to_skip = offset / cluster_size;
    }
    
    for (uint64_t i = 0; i < clusters_to_skip; i++) {
        cluster = fat_next_cluster(fs, cluster);
        if (cluster >= 0x0FFFFFF8) return -1;
    }
    
    uint64_t bytes_read = 0;
    uint64_t cluster_offset = offset % cluster_size;
    uint8_t* out = (uint8_t*)buf;
    
    kstd::KBuffer<uint8_t> cluster_buf(cluster_size);
    if (!cluster_buf) return -1;
    
    while (bytes_read < to_read) {
        uint64_t lba = cluster_to_lba(fs, cluster);
        if (fs->dev->read_blocks(fs->dev, lba, fs->sectors_per_cluster, cluster_buf.get()) < 0) {
            break;
        }
        
        uint64_t chunk = cluster_size - cluster_offset;
        if (chunk > to_read - bytes_read) chunk = to_read - bytes_read;
        
        kstring::memcpy(out + bytes_read, cluster_buf.get() + cluster_offset, chunk);
        
        bytes_read += chunk;
        
        if (fd) {
            fd->last_cluster = cluster;
            fd->last_offset = (offset + bytes_read) - ((offset + bytes_read) % cluster_size);
        }

        cluster_offset = 0;
        
        if (bytes_read < to_read) {
            cluster = fat_next_cluster(fs, cluster);
            if (cluster >= 0x0FFFFFF8) break;
        }
    }
    
    return bytes_read;
}

static int64_t fat32_vfs_write(VNode* node, const void* buf, uint64_t size, uint64_t offset, FileDescriptor* fd) {
    FAT32Node* node_data = (FAT32Node*)node->fs_data;
    if (!node_data) return -1;
    FAT32Filesystem* fs = node_data->fs;
    
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    uint32_t cluster = (uint32_t)node->inode_id;
    
    if (cluster == 0) {
        cluster = fat32_allocate_cluster(fs, 0);
        if (cluster >= 0x0FFFFFF8) return -1;
        node->inode_id = cluster;
        fat32_update_dir_entry_cluster(fs, node, cluster);
    }
    
    uint32_t cluster_index = offset / cluster_size;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= 0x0FFFFFF8) {
            next = fat32_allocate_cluster(fs, cluster);
            if (next >= 0x0FFFFFF8) return -1;
        }
        cluster = next;
    }
    
    uint64_t bytes_written = 0;
    const uint8_t* src = (const uint8_t*)buf;
    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) return -1;
    
    while (bytes_written < size) {
        uint32_t cluster_offset = (offset + bytes_written) % cluster_size;
        uint32_t sector_in_cluster = cluster_offset / fs->bytes_per_sector;
        uint32_t sector_offset = cluster_offset % fs->bytes_per_sector;
        uint32_t chunk = fs->bytes_per_sector - sector_offset;
        if (chunk > size - bytes_written) chunk = size - bytes_written;
        
        uint64_t lba = cluster_to_lba(fs, cluster) + sector_in_cluster;
        if (fs->dev->read_blocks(fs->dev, lba, 1, sector_buf.get()) < 0) break;
        
        kstring::memcpy(sector_buf.get() + sector_offset, (void*)(src + bytes_written), chunk);
        if (fs->dev->write_blocks(fs->dev, lba, 1, sector_buf.get()) < 0) break;
        
        bytes_written += chunk;
        if (bytes_written < size && (cluster_offset + chunk) == cluster_size) {
            uint32_t next = fat_next_cluster(fs, cluster);
            if (next >= 0x0FFFFFF8) {
                next = fat32_allocate_cluster(fs, cluster);
                if (next >= 0x0FFFFFF8) break;
            }
            cluster = next;
        }
    }
    
    if (offset + bytes_written > node->size) {
        node->size = offset + bytes_written;
        fat32_update_dir_entry_size(fs, node, (uint32_t)node->size);
    }
    
    return bytes_written;
}

static void fat32_to_83(const char* name, char* out) {
    kstring::memset(out, ' ', 11);
    int i = 0;
    while (name[i] && i < 8 && name[i] != '.') {
        out[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 'a' + 'A' : name[i];
        i++;
    }

    const char* dot = nullptr;
    for (int k = 0; name[k]; k++) if (name[k] == '.') dot = &name[k];
    if (dot) {
        dot++;
        int k = 8;
        while (*dot && k < 11) {
            out[k] = (*dot >= 'a' && *dot <= 'z') ? *dot - 'a' + 'A' : *dot;
            dot++; k++;
        }
    }
}

static int fat32_vfs_readdir(VNode* node, uint64_t index, char* name_out);
static VNode* fat32_vfs_lookup(VNode* dir, const char* name);
static int fat32_vfs_create(VNode* dir, const char* name);
static int fat32_vfs_mkdir(VNode* dir, const char* name);
static int fat32_vfs_unlink(VNode* dir, const char* name);

static VNodeOps fat32_file_ops = {
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .readdir = nullptr,
    .lookup = nullptr,
    .create = nullptr,
    .mkdir = nullptr,
    .unlink = nullptr,
    .close = fat32_vfs_close
};

static VNodeOps fat32_dir_ops = {
    .read = nullptr,
    .write = nullptr,
    .readdir = fat32_vfs_readdir,
    .lookup = fat32_vfs_lookup,
    .create = fat32_vfs_create,
    .mkdir = fat32_vfs_mkdir,
    .unlink = fat32_vfs_unlink,
    .close = fat32_vfs_close
};

static VNode* fat32_vfs_lookup(VNode* dir, const char* name) {
    if (!dir->is_dir) return nullptr;
    FAT32Node* dir_data = (FAT32Node*)dir->fs_data;
    if (!dir_data) return nullptr;
    FAT32Filesystem* fs = dir_data->fs;
    
    char name83[11];
    fat32_to_83(name, name83);
    
    uint32_t cluster = (uint32_t)dir->inode_id;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    kstd::KBuffer<uint8_t> cluster_buf(cluster_size);
    if (!cluster_buf) return nullptr;
    
    while (cluster < 0x0FFFFFF8) {
        uint64_t lba = cluster_to_lba(fs, cluster);
        if (fs->dev->read_blocks(fs->dev, lba, fs->sectors_per_cluster, cluster_buf.get()) < 0) break;
        
        FAT32DirEntry* entries = (FAT32DirEntry*)cluster_buf.get();
        for (uint32_t i = 0; i < cluster_size / sizeof(FAT32DirEntry); i++) {
            if (entries[i].name[0] == 0) return nullptr;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr == 0x0F) continue; // LFN entry
            
            if (kstring::memcmp(entries[i].name, name83, 11) == 0) {
                uint32_t file_cluster = ((uint32_t)entries[i].cluster_high << 16) | entries[i].cluster_low;
                bool is_subdir = (entries[i].attr & 0x10) != 0;
                VNodeOps* ops = is_subdir ? &fat32_dir_ops : &fat32_file_ops;
                
                FAT32Node* node_data = (FAT32Node*)malloc(sizeof(FAT32Node));
                if (!node_data) return nullptr;
                node_data->fs = fs;
                node_data->entry_sector = lba + (i * sizeof(FAT32DirEntry) / fs->bytes_per_sector);
                node_data->entry_idx = i % (fs->bytes_per_sector / sizeof(FAT32DirEntry));
                
                return vfs_create_vnode(file_cluster, entries[i].size, is_subdir, ops, node_data);
            }
        }
        
        cluster = fat_next_cluster(fs, cluster);
    }

    return nullptr;
}

static int fat32_vfs_readdir(VNode* node, uint64_t index, char* name_out) {
    if (!node->is_dir) return -1;
    FAT32Node* node_data = (FAT32Node*)node->fs_data;
    if (!node_data) return -1;
    FAT32Filesystem* fs = node_data->fs;
    
    uint32_t cluster = (uint32_t)node->inode_id;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    kstd::KBuffer<uint8_t> cluster_buf(cluster_size);
    if (!cluster_buf) return -1;
    
    uint64_t current_idx = 0;
    while (cluster < 0x0FFFFFF8) {
        uint64_t lba = cluster_to_lba(fs, cluster);
        if (fs->dev->read_blocks(fs->dev, lba, fs->sectors_per_cluster, cluster_buf.get()) < 0) break;
        
        FAT32DirEntry* entries = (FAT32DirEntry*)cluster_buf.get();
        for (uint32_t i = 0; i < cluster_size / sizeof(FAT32DirEntry); i++) {
            if (entries[i].name[0] == 0) return -1;
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr & 0x08) continue; // Volume label
            if (entries[i].attr == 0x0F) continue; // LFN entry
            
            if (current_idx == index) {
                int p = 0;
                for (int k = 0; k < 8; k++) {
                    if (entries[i].name[k] != ' ') name_out[p++] = entries[i].name[k];
                }
                if (entries[i].name[8] != ' ') {
                    name_out[p++] = '.';
                    for (int k = 8; k < 11; k++) {
                        if (entries[i].name[k] != ' ') name_out[p++] = entries[i].name[k];
                    }
                }
                name_out[p] = '\0';
                return 0;
            }
            current_idx++;
        }
        cluster = fat_next_cluster(fs, cluster);
    }

    return -1;
}

static int fat32_vfs_create(VNode* dir, const char* name) {
    FAT32Node* dir_data = (FAT32Node*)dir->fs_data;
    if (!dir_data) return -1;
    FAT32Filesystem* fs = dir_data->fs;
    
    char name83[11];
    fat32_to_83(name, name83);
    
    uint32_t cluster = (uint32_t)dir->inode_id;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    kstd::KBuffer<uint8_t> cluster_buf(cluster_size);
    if (!cluster_buf) return -1;
    
    while (cluster < 0x0FFFFFF8) {
        uint64_t lba = cluster_to_lba(fs, cluster);
        if (fs->dev->read_blocks(fs->dev, lba, fs->sectors_per_cluster, cluster_buf.get()) < 0) break;
        
        FAT32DirEntry* entries = (FAT32DirEntry*)cluster_buf.get();
        for (uint32_t i = 0; i < cluster_size / sizeof(FAT32DirEntry); i++) {
            if ((uint8_t)entries[i].name[0] == 0 || (uint8_t)entries[i].name[0] == 0xE5) {
                kstring::memset(&entries[i], 0, sizeof(FAT32DirEntry));
                kstring::memcpy(entries[i].name, name83, 11);
                entries[i].attr = 0x20;
                
                if (fs->dev->write_blocks(fs->dev, lba, fs->sectors_per_cluster, cluster_buf.get()) < 0) return -1;
                return 0;
            }
        }
        
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= 0x0FFFFFF8) {
            next = fat32_allocate_cluster(fs, cluster);
            if (next >= 0x0FFFFFF8) return -1;
            kstring::memset(cluster_buf.get(), 0, cluster_size);
            if (fs->dev->write_blocks(fs->dev, cluster_to_lba(fs, next), fs->sectors_per_cluster, cluster_buf.get()) < 0) return -1;
            cluster = next;
        } else {
            cluster = next;
        }
    }
    return -1;
}

static int fat32_vfs_mkdir(VNode* dir, const char* name) {
    FAT32Node* dir_data = (FAT32Node*)dir->fs_data;
    if (!dir_data) return -1;
    FAT32Filesystem* fs = dir_data->fs;
    
    if (fat32_vfs_create(dir, name) != 0) return -1;
    
    VNode* new_dir = fat32_vfs_lookup(dir, name);
    if (!new_dir) return -1;
    
    uint32_t new_cluster = fat32_allocate_cluster(fs, 0);
    if (new_cluster >= 0x0FFFFFF8) {
        vfs_close_vnode(new_dir);
        return -1;
    }
    
    FAT32Node* new_dir_data = (FAT32Node*)new_dir->fs_data;
    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) { vfs_close_vnode(new_dir); return -1; }
    
    fs->dev->read_blocks(fs->dev, new_dir_data->entry_sector, 1, sector_buf.get());
    FAT32DirEntry* entry = (FAT32DirEntry*)sector_buf.get() + new_dir_data->entry_idx;
    entry->attr = 0x10;
    entry->cluster_low = new_cluster & 0xFFFF;
    entry->cluster_high = (new_cluster >> 16) & 0xFFFF;
    fs->dev->write_blocks(fs->dev, new_dir_data->entry_sector, 1, sector_buf.get());
    
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    kstd::KBuffer<uint8_t> cluster_buf(cluster_size);
    if (!cluster_buf) { vfs_close_vnode(new_dir); return -1; }
    kstring::memset(cluster_buf.get(), 0, cluster_size);
    
    FAT32DirEntry* entries = (FAT32DirEntry*)cluster_buf.get();
    kstring::memcpy(entries[0].name, ".          ", 11);
    entries[0].attr = 0x10;
    entries[0].cluster_low = new_cluster & 0xFFFF;
    entries[0].cluster_high = (new_cluster >> 16) & 0xFFFF;
    
    kstring::memcpy(entries[1].name, "..         ", 11);
    entries[1].attr = 0x10;
    uint32_t parent_cluster = (uint32_t)dir->inode_id;
    if (parent_cluster == fs->root_dir_cluster) parent_cluster = 0;
    entries[1].cluster_low = parent_cluster & 0xFFFF;
    entries[1].cluster_high = (parent_cluster >> 16) & 0xFFFF;
    
    fs->dev->write_blocks(fs->dev, cluster_to_lba(fs, new_cluster), fs->sectors_per_cluster, cluster_buf.get());
    
    vfs_close_vnode(new_dir);
    return 0;
}

static int fat32_vfs_unlink(VNode* dir, const char* name) {
    VNode* node = fat32_vfs_lookup(dir, name);
    if (!node) return -1;
    if (node->is_dir) {
        vfs_close_vnode(node);
        return -1;
    }
    
    FAT32Node* node_data = (FAT32Node*)node->fs_data;
    FAT32Filesystem* fs = node_data->fs;
    
    kstd::KBuffer<uint8_t> sector_buf(fs->bytes_per_sector);
    if (!sector_buf) { vfs_close_vnode(node); return -1; }
    
    fs->dev->read_blocks(fs->dev, node_data->entry_sector, 1, sector_buf.get());
    FAT32DirEntry* entry = (FAT32DirEntry*)sector_buf.get() + node_data->entry_idx;
    entry->name[0] = (char)0xE5;
    fs->dev->write_blocks(fs->dev, node_data->entry_sector, 1, sector_buf.get());
    
    uint32_t cluster = (uint32_t)node->inode_id;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = fat_next_cluster(fs, cluster);
        fat32_write_entry(fs, cluster, 0);
        cluster = next;
    }
    
    vfs_close_vnode(node);
    return 0;
}

bool fat32_init(BlockDevice* dev, FAT32Filesystem* fs_out) {
    if (!dev || !fs_out) return false;

    uint8_t boot_sector[512];
    if (dev->read_blocks(dev, 0, 1, boot_sector) < 0) {
        DEBUG_ERROR("FAT32: Failed to read boot sector from %s", dev->name);
        return false;
    }

    FAT32BootSector* bs = (FAT32BootSector*)boot_sector;
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
    fs_out->fsinfo_sector = bs->fsinfo_sector;
    fs_out->total_sectors = (bs->total_sectors_16 != 0) ? bs->total_sectors_16 : bs->total_sectors_32;

    DEBUG_INFO("FAT32: Initialized on %s (Cluster size: %d bytes)", 
               dev->name, fs_out->bytes_per_sector * fs_out->sectors_per_cluster);
               
    return true;
}

VNode* fat32_get_root(FAT32Filesystem* fs) {
    if (!fs) return nullptr;
    FAT32Node* node_data = (FAT32Node*)malloc(sizeof(FAT32Node));
    if (!node_data) return nullptr;
    node_data->fs = fs;
    node_data->entry_sector = 0;
    node_data->entry_idx = 0;
    
    return vfs_create_vnode(fs->root_dir_cluster, 0, true, &fat32_dir_ops, node_data);
}
