#include <kernel/debug.h>
#include <kernel/fs/fat32.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/syscall.h>
#include <libk/kstring.h>

namespace {

struct [[gnu::packed]] FAT32BootSector
{
    uint8_t jmp[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t dir_entries;
    uint16_t total_sectors_16;
    uint8_t media_desc;
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
    uint8_t reserved0[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8];
} __attribute__((packed));

struct [[gnu::packed]] FAT32DirEntry
{
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt_res;
    uint8_t crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t cluster_high;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t cluster_low;
    uint32_t size;
};

struct [[gnu::packed]] FAT32LFNEntry
{
    uint8_t ord;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster_low;
    uint16_t name3[2];
};

struct FAT32Node
{
    FAT32Filesystem *fs;
    uint32_t dir_cluster;
    uint32_t entry_start_index;
    uint32_t entry_count;
};

struct DirChain
{
    FAT32Filesystem *fs;
    uint32_t dir_cluster;
    uint32_t cluster_count;
    uint32_t cluster_size;
    uint32_t entries_per_cluster;
    uint32_t total_entries;
    uint32_t *clusters;
    uint8_t *data;
};

struct DirRecord
{
    char name[256];
    uint8_t short_name[11];
    uint8_t attr;
    bool is_dir;
    uint32_t cluster;
    uint32_t size;
    uint32_t entry_start_index;
    uint32_t entry_count;
};

static constexpr uint8_t FAT_ATTR_READ_ONLY = 0x01;
static constexpr uint8_t FAT_ATTR_HIDDEN = 0x02;
static constexpr uint8_t FAT_ATTR_SYSTEM = 0x04;
static constexpr uint8_t FAT_ATTR_VOLUME_ID = 0x08;
static constexpr uint8_t FAT_ATTR_DIRECTORY = 0x10;
static constexpr uint8_t FAT_ATTR_ARCHIVE = 0x20;
static constexpr uint8_t FAT_ATTR_LFN = 0x0F;

static constexpr uint32_t FAT_CLUSTER_EOF = 0x0FFFFFF8;
static constexpr uint32_t FAT32_FSINFO_LEAD_SIGNATURE = 0x41615252;
static constexpr uint32_t FAT32_FSINFO_STRUCT_SIGNATURE = 0x61417272;
static constexpr uint32_t FAT32_FSINFO_TRAIL_SIGNATURE = 0xAA550000;
static constexpr uint32_t FAT32_FSINFO_UNKNOWN = 0xFFFFFFFF;

static inline char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

static inline bool ascii_equal_ci(char a, char b)
{
    return ascii_upper(a) == ascii_upper(b);
}

static bool name_equal_ci(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    for (; *a && *b; a++, b++) {
        if (!ascii_equal_ci(*a, *b))
            return false;
    }
    return *a == '\0' && *b == '\0';
}

static void trim_label(char *out)
{
    size_t len = kstring::strlen(out);
    while (len > 0 && out[len - 1] == ' ') {
        out[len - 1] = '\0';
        len--;
    }
}

static uint64_t cluster_to_lba(FAT32Filesystem *fs, uint32_t cluster)
{
    uint64_t data_start = fs->reserved_sectors + (fs->fat_count * fs->sectors_per_fat);
    return data_start + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static uint32_t fat_next_cluster(FAT32Filesystem *fs, uint32_t cluster)
{
    uint32_t fat_offset = cluster * 4;
    uint64_t fat_sector = fs->reserved_sectors + (fat_offset / fs->bytes_per_sector);
    uint32_t entry_offset = fat_offset % fs->bytes_per_sector;

    uint8_t *sector_buf = static_cast<uint8_t *>(malloc(fs->bytes_per_sector));
    if (!sector_buf)
        return 0x0FFFFFFF;

    uint32_t result = 0x0FFFFFFF;
    if (fs->dev->read_blocks(fs->dev, fat_sector, 1, sector_buf) >= 0)
        result = (*reinterpret_cast<uint32_t *>(sector_buf + entry_offset)) & 0x0FFFFFFF;
    free(sector_buf);
    return result;
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static bool fat32_fsinfo_valid(const uint8_t *sector)
{
    return sector && read_le32(sector + 0) == FAT32_FSINFO_LEAD_SIGNATURE &&
           read_le32(sector + 484) == FAT32_FSINFO_STRUCT_SIGNATURE &&
           read_le32(sector + 508) == FAT32_FSINFO_TRAIL_SIGNATURE;
}

static void fat32_load_fsinfo(FAT32Filesystem *fs)
{
    if (!fs || fs->fsinfo_sector == 0 || fs->fsinfo_sector >= fs->reserved_sectors)
        return;

    uint8_t *sector_buf = static_cast<uint8_t *>(malloc(fs->bytes_per_sector));
    if (!sector_buf)
        return;

    if (fs->dev->read_blocks(fs->dev, fs->fsinfo_sector, 1, sector_buf) >= 0 && fat32_fsinfo_valid(sector_buf)) {
        uint32_t free_count = read_le32(sector_buf + 488);
        uint32_t next_free = read_le32(sector_buf + 492);
        if (free_count <= fs->cluster_count)
            fs->free_cluster_count = free_count;
        if (next_free >= 2 && next_free < fs->cluster_count + 2)
            fs->next_free_cluster = next_free;
    }

    free(sector_buf);
}

static void fat32_update_fsinfo(FAT32Filesystem *fs)
{
    if (!fs || fs->fsinfo_sector == 0 || fs->fsinfo_sector >= fs->reserved_sectors)
        return;

    uint8_t *sector_buf = static_cast<uint8_t *>(malloc(fs->bytes_per_sector));
    if (!sector_buf)
        return;

    if (fs->dev->read_blocks(fs->dev, fs->fsinfo_sector, 1, sector_buf) >= 0 && fat32_fsinfo_valid(sector_buf)) {
        write_le32(sector_buf + 488, fs->free_cluster_count);
        write_le32(sector_buf + 492, fs->next_free_cluster);
        fs->dev->write_blocks(fs->dev, fs->fsinfo_sector, 1, sector_buf);
    }

    free(sector_buf);
}

static void fat32_write_entry(FAT32Filesystem *fs, uint32_t cluster, uint32_t value)
{
    uint32_t fat_offset = cluster * 4;
    uint32_t sector_offset = fat_offset % fs->bytes_per_sector;
    uint8_t *sector_buf = static_cast<uint8_t *>(malloc(fs->bytes_per_sector));
    if (!sector_buf)
        return;

    for (uint32_t fat_num = 0; fat_num < fs->fat_count; fat_num++) {
        uint64_t sector = fs->reserved_sectors + (fat_num * fs->sectors_per_fat) + (fat_offset / fs->bytes_per_sector);
        if (fs->dev->read_blocks(fs->dev, sector, 1, sector_buf) < 0)
            continue;
        uint32_t existing = *reinterpret_cast<uint32_t *>(sector_buf + sector_offset);
        uint32_t new_val = (existing & 0xF0000000) | (value & 0x0FFFFFFF);
        *reinterpret_cast<uint32_t *>(sector_buf + sector_offset) = new_val;
        fs->dev->write_blocks(fs->dev, sector, 1, sector_buf);
    }

    free(sector_buf);
}

static uint32_t fat32_allocate_cluster(FAT32Filesystem *fs, uint32_t last_cluster)
{
    if (!fs || fs->cluster_count == 0)
        return 0x0FFFFFFF;

    uint32_t start = fs->next_free_cluster;
    if (start < 2 || start >= fs->cluster_count + 2)
        start = 2;

    for (uint32_t attempt = 0; attempt < fs->cluster_count; attempt++) {
        uint32_t cluster = start + attempt;
        if (cluster >= fs->cluster_count + 2)
            cluster = 2 + (cluster - (fs->cluster_count + 2));
        if (fat_next_cluster(fs, cluster) == 0) {
            fat32_write_entry(fs, cluster, 0x0FFFFFFF);
            if (last_cluster >= 2)
                fat32_write_entry(fs, last_cluster, cluster);

            uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
            uint8_t *zero = static_cast<uint8_t *>(malloc(cluster_size));
            if (zero) {
                kstring::zero_memory(zero, cluster_size);
                fs->dev->write_blocks(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, zero);
                free(zero);
            }
            fs->next_free_cluster = cluster + 1;
            if (fs->next_free_cluster >= fs->cluster_count + 2)
                fs->next_free_cluster = 2;
            if (fs->free_cluster_count != FAT32_FSINFO_UNKNOWN && fs->free_cluster_count > 0)
                fs->free_cluster_count--;
            fat32_update_fsinfo(fs);
            return cluster;
        }
    }
    return 0x0FFFFFFF;
}

static void fat32_free_chain(FAT32Filesystem *fs, uint32_t cluster)
{
    uint32_t first_freed = 0;
    uint32_t freed_count = 0;
    while (cluster >= 2 && cluster < FAT_CLUSTER_EOF) {
        uint32_t next = fat_next_cluster(fs, cluster);
        fat32_write_entry(fs, cluster, 0);
        if (first_freed == 0 || cluster < first_freed)
            first_freed = cluster;
        freed_count++;
        cluster = next;
    }
    if (first_freed != 0)
        fs->next_free_cluster = first_freed;
    if (fs->free_cluster_count != FAT32_FSINFO_UNKNOWN)
        fs->free_cluster_count += freed_count;
    fat32_update_fsinfo(fs);
}

static bool dir_chain_load(FAT32Filesystem *fs, uint32_t dir_cluster, DirChain *out)
{
    if (!fs || !out || dir_cluster < 2)
        return false;

    *out = {};
    out->fs = fs;
    out->dir_cluster = dir_cluster;
    out->cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    out->entries_per_cluster = out->cluster_size / sizeof(FAT32DirEntry);

    uint32_t cluster = dir_cluster;
    uint32_t cluster_count = 0;
    while (cluster >= 2 && cluster < FAT_CLUSTER_EOF) {
        cluster_count++;
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= FAT_CLUSTER_EOF)
            break;
        cluster = next;
    }

    out->clusters = static_cast<uint32_t *>(malloc(sizeof(uint32_t) * cluster_count));
    out->data = static_cast<uint8_t *>(malloc(out->cluster_size * cluster_count));
    if (!out->clusters || !out->data) {
        free(out->clusters);
        free(out->data);
        return false;
    }

    cluster = dir_cluster;
    for (uint32_t i = 0; i < cluster_count; i++) {
        out->clusters[i] = cluster;
        if (fs->dev->read_blocks(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster,
                                 out->data + i * out->cluster_size) < 0) {
            free(out->clusters);
            free(out->data);
            return false;
        }
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= FAT_CLUSTER_EOF)
            break;
        cluster = next;
    }

    out->cluster_count = cluster_count;
    out->total_entries = cluster_count * out->entries_per_cluster;
    return true;
}

static void dir_chain_free(DirChain *chain)
{
    if (!chain)
        return;
    free(chain->clusters);
    free(chain->data);
    *chain = {};
}

static FAT32DirEntry *dir_chain_entry(DirChain *chain, uint32_t entry_index)
{
    if (!chain || entry_index >= chain->total_entries)
        return nullptr;
    return reinterpret_cast<FAT32DirEntry *>(chain->data + entry_index * sizeof(FAT32DirEntry));
}

static bool dir_chain_save(DirChain *chain)
{
    if (!chain || !chain->fs)
        return false;
    for (uint32_t i = 0; i < chain->cluster_count; i++) {
        if (chain->fs->dev->write_blocks(chain->fs->dev, cluster_to_lba(chain->fs, chain->clusters[i]),
                                         chain->fs->sectors_per_cluster, chain->data + i * chain->cluster_size) < 0) {
            return false;
        }
    }
    return true;
}

static bool dir_chain_extend(DirChain *chain, uint32_t required_entries)
{
    if (!chain)
        return false;
    while (chain->total_entries < required_entries) {
        uint32_t last_cluster = chain->clusters[chain->cluster_count - 1];
        uint32_t new_cluster = fat32_allocate_cluster(chain->fs, last_cluster);
        if (new_cluster >= FAT_CLUSTER_EOF)
            return false;

        uint32_t *new_clusters =
            static_cast<uint32_t *>(realloc(chain->clusters, sizeof(uint32_t) * (chain->cluster_count + 1)));
        uint8_t *new_data =
            static_cast<uint8_t *>(realloc(chain->data, chain->cluster_size * (chain->cluster_count + 1)));
        if (!new_clusters || !new_data)
            return false;

        chain->clusters = new_clusters;
        chain->data = new_data;
        chain->clusters[chain->cluster_count] = new_cluster;
        kstring::zero_memory(chain->data + chain->cluster_size * chain->cluster_count, chain->cluster_size);
        chain->cluster_count++;
        chain->total_entries = chain->cluster_count * chain->entries_per_cluster;
    }
    return true;
}

static uint32_t dir_record_cluster(const FAT32DirEntry *entry)
{
    return ((uint32_t)entry->cluster_high << 16) | entry->cluster_low;
}

static void short_name_to_string(const uint8_t short_name[11], char *out, size_t out_size)
{
    size_t pos = 0;
    for (int i = 0; i < 8 && pos + 1 < out_size; i++) {
        if (short_name[i] == ' ')
            break;
        out[pos++] = (char)short_name[i];
    }
    if (short_name[8] != ' ' && pos + 2 < out_size) {
        out[pos++] = '.';
        for (int i = 8; i < 11 && pos + 1 < out_size; i++) {
            if (short_name[i] == ' ')
                break;
            out[pos++] = (char)short_name[i];
        }
    }
    out[pos] = '\0';
}

static uint8_t lfn_checksum(const uint8_t short_name[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i];
    return sum;
}

static void lfn_copy_name_part(const FAT32LFNEntry *lfn, uint16_t *dst)
{
    for (int i = 0; i < 5; i++)
        dst[i] = lfn->name1[i];
    for (int i = 0; i < 6; i++)
        dst[5 + i] = lfn->name2[i];
    for (int i = 0; i < 2; i++)
        dst[11 + i] = lfn->name3[i];
}

static int utf16_to_utf8(const uint16_t *src, int len, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return -1;
    size_t pos = 0;
    for (int i = 0; i < len; i++) {
        uint16_t cp = src[i];
        if (cp == 0 || cp == 0xFFFF)
            break;
        if (cp >= 0xD800 && cp <= 0xDFFF)
            return -1;
        if (cp < 0x80) {
            if (pos + 1 >= out_size)
                return -1;
            out[pos++] = (char)cp;
        } else if (cp < 0x800) {
            if (pos + 2 >= out_size)
                return -1;
            out[pos++] = (char)(0xC0 | (cp >> 6));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (pos + 3 >= out_size)
                return -1;
            out[pos++] = (char)(0xE0 | (cp >> 12));
            out[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[pos] = '\0';
    return (int)pos;
}

static int utf8_to_utf16_bmp(const char *src, uint16_t *out, int max_units)
{
    int count = 0;
    while (*src) {
        if (count >= max_units)
            return -1;
        uint8_t c = (uint8_t)*src++;
        if (c < 0x80) {
            out[count++] = c;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            uint8_t c2 = (uint8_t)*src++;
            if ((c2 & 0xC0) != 0x80)
                return -1;
            out[count++] = (uint16_t)(((c & 0x1F) << 6) | (c2 & 0x3F));
            continue;
        }
        if ((c & 0xF0) == 0xE0) {
            uint8_t c2 = (uint8_t)*src++;
            uint8_t c3 = (uint8_t)*src++;
            if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80)
                return -1;
            uint16_t cp = (uint16_t)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F));
            if (cp >= 0xD800 && cp <= 0xDFFF)
                return -1;
            out[count++] = cp;
            continue;
        }
        return -1;
    }
    return count;
}

static bool dir_scan_record(DirChain *chain, uint32_t *cursor, DirRecord *out)
{
    uint16_t lfn_utf16[260];
    uint32_t lfn_slots = 0;
    uint8_t lfn_sum = 0;
    bool have_lfn = false;

    while (*cursor < chain->total_entries) {
        FAT32DirEntry *entry = dir_chain_entry(chain, *cursor);
        if (!entry)
            return false;

        if (entry->name[0] == 0)
            return false;

        if (entry->name[0] == 0xE5) {
            have_lfn = false;
            lfn_slots = 0;
            (*cursor)++;
            continue;
        }

        if (entry->attr == FAT_ATTR_LFN) {
            auto *lfn = reinterpret_cast<FAT32LFNEntry *>(entry);
            uint8_t ord = (uint8_t)(lfn->ord & 0x1F);
            if ((lfn->ord & 0x40) != 0) {
                have_lfn = true;
                lfn_slots = ord;
                lfn_sum = lfn->checksum;
                for (uint32_t i = 0; i < 260; i++)
                    lfn_utf16[i] = 0xFFFF;
            }
            if (have_lfn && ord > 0 && ord <= 20) {
                uint16_t part[13];
                lfn_copy_name_part(lfn, part);
                uint32_t base = (ord - 1) * 13;
                for (uint32_t i = 0; i < 13 && base + i < 260; i++)
                    lfn_utf16[base + i] = part[i];
            }
            (*cursor)++;
            continue;
        }

        if ((entry->attr & FAT_ATTR_VOLUME_ID) != 0) {
            have_lfn = false;
            lfn_slots = 0;
            (*cursor)++;
            continue;
        }

        out->attr = entry->attr;
        out->is_dir = (entry->attr & FAT_ATTR_DIRECTORY) != 0;
        out->cluster = dir_record_cluster(entry);
        out->size = entry->size;
        kstring::memcpy(out->short_name, entry->name, 11);
        out->entry_count = have_lfn ? (lfn_slots + 1) : 1;
        out->entry_start_index = *cursor + 1 - out->entry_count;

        if (have_lfn && lfn_sum == lfn_checksum(entry->name)) {
            if (utf16_to_utf8(lfn_utf16, (int)(lfn_slots * 13), out->name, sizeof(out->name)) < 0)
                short_name_to_string(entry->name, out->name, sizeof(out->name));
        } else {
            short_name_to_string(entry->name, out->name, sizeof(out->name));
        }

        (*cursor)++;
        return true;
    }

    return false;
}

static bool dir_find_record(DirChain *chain, const char *name, DirRecord *out)
{
    uint32_t cursor = 0;
    DirRecord record = {};
    while (dir_scan_record(chain, &cursor, &record)) {
        if (name_equal_ci(record.name, name)) {
            if (out)
                *out = record;
            return true;
        }
    }
    return false;
}

static bool short_name_used(DirChain *chain, const uint8_t short_name[11])
{
    uint32_t cursor = 0;
    DirRecord record = {};
    while (dir_scan_record(chain, &cursor, &record)) {
        if (kstring::memcmp(record.short_name, short_name, 11) == 0)
            return true;
    }
    return false;
}

static bool name_requires_lfn(const char *name)
{
    if (!name || name[0] == '\0')
        return true;
    bool saw_dot = false;
    int base_len = 0;
    int ext_len = 0;
    bool saw_lower = false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (saw_dot)
                return true;
            saw_dot = true;
            continue;
        }
        bool allowed = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' || c == '%' || c == '\'' ||
                       c == '-' || c == '_' || c == '@' || c == '~' || c == '`' || c == '!' || c == '(' || c == ')' ||
                       c == '{' || c == '}' || c == '^' || c == '#' || c == '&';
        bool lower = (c >= 'a' && c <= 'z');
        if (lower)
            saw_lower = true;
        if (!allowed && !lower)
            return true;
        if (!saw_dot)
            base_len++;
        else
            ext_len++;
    }
    return saw_lower || base_len == 0 || base_len > 8 || ext_len > 3 || kstring::strcmp(name, ".") == 0 ||
           kstring::strcmp(name, "..") == 0;
}

static char sanitize_short_char(char c)
{
    c = ascii_upper(c);
    bool allowed = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (allowed)
        return c;
    switch (c) {
        case '$':
        case '%':
        case '\'':
        case '-':
        case '_':
        case '@':
        case '~':
        case '`':
        case '!':
        case '(':
        case ')':
        case '{':
        case '}':
        case '^':
        case '#':
        case '&':
            return c;
        default:
            return '_';
    }
}

static void format_short_name(const char *name, uint32_t suffix, uint8_t out[11])
{
    if (!name || !out)
        return;
    char base[9];
    char ext[4];
    int base_len = 0;
    int ext_len = 0;
    bool seen_dot = false;

    kstring::memset(base, 0, sizeof(base));
    kstring::memset(ext, 0, sizeof(ext));

    for (const char *p = name; *p; p++) {
        if (*p == '.') {
            seen_dot = true;
            continue;
        }
        if (!seen_dot) {
            if (base_len < 8)
                base[base_len++] = sanitize_short_char(*p);
        } else if (ext_len < 3) {
            ext[ext_len++] = sanitize_short_char(*p);
        }
    }

    kstring::memset(out, ' ', 11);
    if (base_len == 0)
        base[base_len++] = '_';

    if (suffix == 0) {
        for (int i = 0; i < base_len && i < 8; i++)
            out[i] = (uint8_t)base[i];
        for (int i = 0; i < ext_len && i < 3; i++)
            out[8 + i] = (uint8_t)ext[i];
        return;
    }

    char suffix_buf[8];
    kstring::itoa((int)suffix, suffix_buf, 10);
    size_t suffix_len = kstring::strlen(suffix_buf);
    int prefix_len = 8 - 1 - (int)suffix_len;
    if (prefix_len < 1)
        prefix_len = 1;

    for (int i = 0; i < prefix_len && i < base_len; i++)
        out[i] = (uint8_t)base[i];
    out[prefix_len] = '~';
    for (size_t i = 0; i < suffix_len && (prefix_len + 1 + (int)i) < 8; i++)
        out[prefix_len + 1 + i] = (uint8_t)suffix_buf[i];
    for (int i = 0; i < ext_len && i < 3; i++)
        out[8 + i] = (uint8_t)ext[i];
}

static void generate_short_name(DirChain *chain, const char *name, uint8_t out[11])
{
    format_short_name(name, 0, out);
    if (!short_name_used(chain, out) && !name_requires_lfn(name))
        return;

    for (uint32_t suffix = 1; suffix < 1000; suffix++) {
        uint8_t candidate[11];
        format_short_name(name, suffix, candidate);
        if (!short_name_used(chain, candidate)) {
            kstring::memcpy(out, candidate, 11);
            return;
        }
    }

    kstring::memcpy(out, "FILE    BIN", 11);
}

static int build_dir_entries(DirChain *chain, const char *name, uint8_t attr, uint32_t cluster, uint32_t size,
                             FAT32DirEntry *out_entries, int max_entries)
{
    if (!chain || !name || !out_entries || max_entries <= 0)
        return -1;

    kstring::zero_memory(out_entries, sizeof(FAT32DirEntry) * max_entries);
    uint8_t short_name[11];
    generate_short_name(chain, name, short_name);
    FAT32DirEntry short_entry = {};
    kstring::memcpy(short_entry.name, short_name, 11);
    short_entry.attr = attr;
    short_entry.cluster_low = (uint16_t)(cluster & 0xFFFF);
    short_entry.cluster_high = (uint16_t)((cluster >> 16) & 0xFFFF);
    short_entry.size = size;

    bool needs_lfn = name_requires_lfn(name);
    if (!needs_lfn) {
        out_entries[0] = short_entry;
        return 1;
    }
    int max_possible = max_entries;

    uint16_t utf16_name[260];
    int utf16_len = utf8_to_utf16_bmp(name, utf16_name, 255);
    if (utf16_len < 0)
        return -1;
    int lfn_count = (utf16_len + 12) / 13;
    if (lfn_count + 1 > max_possible)
        return -1;

    for (int i = 0; i < max_possible; i++)
        out_entries[i] = {};
    FAT32DirEntry *short_slot = &out_entries[lfn_count];
    *short_slot = short_entry;
    uint8_t checksum = lfn_checksum(short_name);
    for (int entry_idx = 0; entry_idx < lfn_count; entry_idx++) {
        auto *lfn = reinterpret_cast<FAT32LFNEntry *>(&out_entries[entry_idx]);
        int ord = lfn_count - entry_idx;
        lfn->ord = (uint8_t)ord;
        if (entry_idx == 0)
            lfn->ord |= 0x40;
        lfn->attr = FAT_ATTR_LFN;
        lfn->type = 0;
        lfn->checksum = checksum;
        lfn->first_cluster_low = 0;

        int base = (ord - 1) * 13;
        uint16_t chars[13];
        for (int i = 0; i < 13; i++)
            chars[i] = 0xFFFF;
        for (int i = 0; i < 13; i++) {
            int idx = base + i;
            if (idx < utf16_len)
                chars[i] = utf16_name[idx];
            else if (idx == utf16_len)
                chars[i] = 0;
        }
        for (int i = 0; i < 5; i++)
            lfn->name1[i] = chars[i];
        for (int i = 0; i < 6; i++)
            lfn->name2[i] = chars[5 + i];
        for (int i = 0; i < 2; i++)
            lfn->name3[i] = chars[11 + i];
    }
    return lfn_count + 1;
}

static bool dir_find_free_range(DirChain *chain, uint32_t needed_entries, uint32_t *out_index)
{
    uint32_t run_start = 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < chain->total_entries; i++) {
        FAT32DirEntry *entry = dir_chain_entry(chain, i);
        if (!entry)
            break;
        if (entry->name[0] == 0 || entry->name[0] == 0xE5) {
            if (run == 0)
                run_start = i;
            run++;
            if (run >= needed_entries) {
                *out_index = run_start;
                return true;
            }
            continue;
        }
        run = 0;
    }

    uint32_t old_entries = chain->total_entries;
    if (!dir_chain_extend(chain, chain->total_entries + needed_entries))
        return false;
    *out_index = old_entries;
    return true;
}

static bool dir_update_short_entry(FAT32Filesystem *fs, FAT32Node *node_data,
                                   void (*mutator)(FAT32DirEntry *entry, void *ctx), void *ctx)
{
    DirChain chain;
    if (!dir_chain_load(fs, node_data->dir_cluster, &chain))
        return false;
    FAT32DirEntry *entry = dir_chain_entry(&chain, node_data->entry_start_index + node_data->entry_count - 1);
    if (!entry) {
        dir_chain_free(&chain);
        return false;
    }
    mutator(entry, ctx);
    bool ok = dir_chain_save(&chain);
    dir_chain_free(&chain);
    return ok;
}

static void set_entry_size(FAT32DirEntry *entry, void *ctx)
{
    entry->size = *static_cast<uint32_t *>(ctx);
}

static void set_entry_cluster(FAT32DirEntry *entry, void *ctx)
{
    uint32_t cluster = *static_cast<uint32_t *>(ctx);
    entry->cluster_low = (uint16_t)(cluster & 0xFFFF);
    entry->cluster_high = (uint16_t)((cluster >> 16) & 0xFFFF);
}

static int fat32_vfs_truncate(VNode *node, uint64_t size)
{
    if (node->is_dir || size != 0)
        return -1;
    auto *node_data = static_cast<FAT32Node *>(node->fs_data);
    if (!node_data)
        return -1;
    FAT32Filesystem *fs = node_data->fs;

    fat32_free_chain(fs, (uint32_t)node->inode_id);
    node->inode_id = 0;
    node->size = 0;
    uint32_t zero = 0;
    dir_update_short_entry(fs, node_data, set_entry_cluster, &zero);
    dir_update_short_entry(fs, node_data, set_entry_size, &zero);
    return 0;
}

static void fat32_vfs_close(VNode *node)
{
    free(node->fs_data);
    node->fs_data = nullptr;
}

static int64_t fat32_vfs_read(VNode *node, void *buf, uint64_t size, uint64_t offset, FileDescriptor *fd)
{
    (void)fd;
    auto *node_data = static_cast<FAT32Node *>(node->fs_data);
    if (!node_data)
        return -1;
    FAT32Filesystem *fs = node_data->fs;
    if (!fs || offset >= node->size || node->inode_id == 0)
        return 0;

    uint64_t to_read = (size < node->size - offset) ? size : (node->size - offset);
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    uint32_t cluster = (uint32_t)node->inode_id;
    uint64_t skip = offset / cluster_size;

    for (uint64_t i = 0; i < skip; i++) {
        cluster = fat_next_cluster(fs, cluster);
        if (cluster >= FAT_CLUSTER_EOF)
            return -1;
    }

    uint8_t *cluster_buf = static_cast<uint8_t *>(malloc(cluster_size));
    if (!cluster_buf)
        return -1;
    uint8_t *out = static_cast<uint8_t *>(buf);
    uint64_t bytes_read = 0;
    uint64_t cluster_offset = offset % cluster_size;

    while (bytes_read < to_read && cluster >= 2 && cluster < FAT_CLUSTER_EOF) {
        if (fs->dev->read_blocks(fs->dev, cluster_to_lba(fs, cluster), fs->sectors_per_cluster, cluster_buf) < 0)
            break;
        uint64_t chunk = cluster_size - cluster_offset;
        if (chunk > to_read - bytes_read)
            chunk = to_read - bytes_read;
        kstring::memcpy(out + bytes_read, cluster_buf + cluster_offset, chunk);
        bytes_read += chunk;
        cluster_offset = 0;
        if (bytes_read < to_read)
            cluster = fat_next_cluster(fs, cluster);
    }

    free(cluster_buf);
    return (int64_t)bytes_read;
}

static int64_t fat32_vfs_write(VNode *node, const void *buf, uint64_t size, uint64_t offset, FileDescriptor *fd)
{
    (void)fd;
    auto *node_data = static_cast<FAT32Node *>(node->fs_data);
    if (!node_data)
        return -1;
    FAT32Filesystem *fs = node_data->fs;
    uint32_t cluster_size = fs->bytes_per_sector * fs->sectors_per_cluster;
    uint32_t cluster = (uint32_t)node->inode_id;

    if (cluster == 0) {
        cluster = fat32_allocate_cluster(fs, 0);
        if (cluster >= FAT_CLUSTER_EOF)
            return -1;
        node->inode_id = cluster;
        dir_update_short_entry(fs, node_data, set_entry_cluster, &cluster);
    }

    uint32_t cluster_index = offset / cluster_size;
    for (uint32_t i = 0; i < cluster_index; i++) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= FAT_CLUSTER_EOF) {
            next = fat32_allocate_cluster(fs, cluster);
            if (next >= FAT_CLUSTER_EOF)
                return -1;
        }
        cluster = next;
    }

    uint8_t *sector_buf = static_cast<uint8_t *>(malloc(fs->bytes_per_sector));
    if (!sector_buf)
        return -1;
    const uint8_t *src = static_cast<const uint8_t *>(buf);
    uint64_t bytes_written = 0;

    while (bytes_written < size) {
        uint32_t cluster_offset = (uint32_t)((offset + bytes_written) % cluster_size);
        uint32_t sector_in_cluster = cluster_offset / fs->bytes_per_sector;
        uint32_t sector_offset = cluster_offset % fs->bytes_per_sector;
        uint32_t chunk = fs->bytes_per_sector - sector_offset;
        if (chunk > size - bytes_written)
            chunk = (uint32_t)(size - bytes_written);

        uint64_t lba = cluster_to_lba(fs, cluster) + sector_in_cluster;
        if (fs->dev->read_blocks(fs->dev, lba, 1, sector_buf) < 0)
            break;
        kstring::memcpy(sector_buf + sector_offset, src + bytes_written, chunk);
        if (fs->dev->write_blocks(fs->dev, lba, 1, sector_buf) < 0)
            break;

        bytes_written += chunk;
        if (bytes_written < size && (cluster_offset + chunk) == cluster_size) {
            uint32_t next = fat_next_cluster(fs, cluster);
            if (next >= FAT_CLUSTER_EOF) {
                next = fat32_allocate_cluster(fs, cluster);
                if (next >= FAT_CLUSTER_EOF)
                    break;
            }
            cluster = next;
        }
    }

    free(sector_buf);

    if (offset + bytes_written > node->size) {
        node->size = offset + bytes_written;
        uint32_t new_size = (uint32_t)node->size;
        dir_update_short_entry(fs, node_data, set_entry_size, &new_size);
    }

    return (int64_t)bytes_written;
}

static int fat32_vfs_readdir(VNode *node, uint64_t index, char *name_out);
static VNode *fat32_vfs_lookup(VNode *dir, const char *name);
static int fat32_vfs_create(VNode *dir, const char *name);
static int fat32_vfs_mkdir(VNode *dir, const char *name);
static int fat32_vfs_unlink(VNode *dir, const char *name);
static int fat32_vfs_rename(VNode *old_dir, const char *old_name, VNode *new_dir, const char *new_name);

} // namespace

VNodeOps fat32_file_ops = {.read = fat32_vfs_read,
                                  .write = fat32_vfs_write,
                                  .readdir = nullptr,
                                  .lookup = nullptr,
                                  .create = nullptr,
                                  .mkdir = nullptr,
                                  .unlink = nullptr,
                                  .rename = nullptr,
                                  .truncate = fat32_vfs_truncate,
                                  .sync = nullptr,
                                  .close = fat32_vfs_close};

namespace {

static VNodeOps fat32_dir_ops = {.read = nullptr,
                                 .write = nullptr,
                                 .readdir = fat32_vfs_readdir,
                                 .lookup = fat32_vfs_lookup,
                                 .create = fat32_vfs_create,
                                 .mkdir = fat32_vfs_mkdir,
                                 .unlink = fat32_vfs_unlink,
                                 .rename = fat32_vfs_rename,
                                 .truncate = nullptr,
                                 .sync = nullptr,
                                 .close = fat32_vfs_close};

static VNode *make_vnode_from_record(FAT32Filesystem *fs, uint32_t dir_cluster, const DirRecord &record)
{
    auto *node_data = static_cast<FAT32Node *>(malloc(sizeof(FAT32Node)));
    if (!node_data)
        return nullptr;
    node_data->fs = fs;
    node_data->dir_cluster = dir_cluster;
    node_data->entry_start_index = record.entry_start_index;
    node_data->entry_count = record.entry_count;
    return vfs_create_vnode(record.cluster, record.size, record.is_dir,
                            record.is_dir ? &fat32_dir_ops : &fat32_file_ops, node_data);
}

static VNode *fat32_vfs_lookup(VNode *dir, const char *name)
{
    if (!dir || !dir->is_dir)
        return nullptr;
    auto *dir_data = static_cast<FAT32Node *>(dir->fs_data);
    if (!dir_data)
        return nullptr;

    DirChain chain;
    if (!dir_chain_load(dir_data->fs, (uint32_t)dir->inode_id, &chain))
        return nullptr;
    DirRecord record = {};
    bool found = dir_find_record(&chain, name, &record);
    dir_chain_free(&chain);
    return found ? make_vnode_from_record(dir_data->fs, (uint32_t)dir->inode_id, record) : nullptr;
}

static int fat32_vfs_readdir(VNode *node, uint64_t index, char *name_out)
{
    if (!node || !node->is_dir || !name_out)
        return -1;
    auto *node_data = static_cast<FAT32Node *>(node->fs_data);
    if (!node_data)
        return -1;
    DirChain chain;
    if (!dir_chain_load(node_data->fs, (uint32_t)node->inode_id, &chain))
        return -1;

    uint32_t cursor = 0;
    uint64_t current = 0;
    DirRecord record = {};
    while (dir_scan_record(&chain, &cursor, &record)) {
        if (current++ == index) {
            kstring::strncpy(name_out, record.name, 255);
            dir_chain_free(&chain);
            return 0;
        }
    }

    dir_chain_free(&chain);
    return -1;
}

static int fat32_vfs_create(VNode *dir, const char *name)
{
    if (!dir || !name || name[0] == '\0')
        return -1;
    auto *dir_data = static_cast<FAT32Node *>(dir->fs_data);
    if (!dir_data)
        return -1;

    DirChain chain;
    if (!dir_chain_load(dir_data->fs, (uint32_t)dir->inode_id, &chain))
        return -1;
    if (dir_find_record(&chain, name, nullptr)) {
        dir_chain_free(&chain);
        return -1;
    }

    FAT32DirEntry entries[21];
    int entry_count = build_dir_entries(&chain, name, FAT_ATTR_ARCHIVE, 0, 0, entries, 21);
    if (entry_count <= 0) {
        dir_chain_free(&chain);
        return -1;
    }

    uint32_t start_index = 0;
    if (!dir_find_free_range(&chain, (uint32_t)entry_count, &start_index)) {
        dir_chain_free(&chain);
        return -1;
    }
    for (int i = 0; i < entry_count; i++)
        *dir_chain_entry(&chain, start_index + (uint32_t)i) = entries[i];

    bool ok = dir_chain_save(&chain);
    dir_chain_free(&chain);
    return ok ? 0 : -1;
}

static int fat32_vfs_mkdir(VNode *dir, const char *name)
{
    if (!dir || !name || name[0] == '\0')
        return -1;
    auto *dir_data = static_cast<FAT32Node *>(dir->fs_data);
    if (!dir_data)
        return -1;
    FAT32Filesystem *fs = dir_data->fs;

    DirChain chain;
    if (!dir_chain_load(fs, (uint32_t)dir->inode_id, &chain))
        return -1;
    if (dir_find_record(&chain, name, nullptr)) {
        dir_chain_free(&chain);
        return -1;
    }

    uint32_t new_cluster = fat32_allocate_cluster(fs, 0);
    if (new_cluster >= FAT_CLUSTER_EOF) {
        dir_chain_free(&chain);
        return -1;
    }

    FAT32DirEntry entries[21];
    int entry_count = build_dir_entries(&chain, name, FAT_ATTR_DIRECTORY, new_cluster, 0, entries, 21);
    if (entry_count <= 0) {
        dir_chain_free(&chain);
        return -1;
    }

    uint32_t start_index = 0;
    if (!dir_find_free_range(&chain, (uint32_t)entry_count, &start_index)) {
        dir_chain_free(&chain);
        return -1;
    }
    for (int i = 0; i < entry_count; i++)
        *dir_chain_entry(&chain, start_index + (uint32_t)i) = entries[i];
    if (!dir_chain_save(&chain)) {
        dir_chain_free(&chain);
        return -1;
    }
    dir_chain_free(&chain);

    DirChain new_dir_chain;
    if (!dir_chain_load(fs, new_cluster, &new_dir_chain))
        return -1;
    FAT32DirEntry *dot = dir_chain_entry(&new_dir_chain, 0);
    FAT32DirEntry *dotdot = dir_chain_entry(&new_dir_chain, 1);
    if (!dot || !dotdot) {
        dir_chain_free(&new_dir_chain);
        return -1;
    }
    kstring::zero_memory(dot, sizeof(FAT32DirEntry));
    kstring::zero_memory(dotdot, sizeof(FAT32DirEntry));
    kstring::memcpy(dot->name, ".          ", 11);
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->cluster_low = (uint16_t)(new_cluster & 0xFFFF);
    dot->cluster_high = (uint16_t)((new_cluster >> 16) & 0xFFFF);
    kstring::memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = FAT_ATTR_DIRECTORY;
    uint32_t parent_cluster = (uint32_t)dir->inode_id;
    if (parent_cluster == fs->root_dir_cluster)
        parent_cluster = 0;
    dotdot->cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
    dotdot->cluster_high = (uint16_t)((parent_cluster >> 16) & 0xFFFF);
    bool ok = dir_chain_save(&new_dir_chain);
    dir_chain_free(&new_dir_chain);
    return ok ? 0 : -1;
}

static bool fat32_dir_is_empty(FAT32Filesystem *fs, uint32_t cluster)
{
    DirChain chain;
    if (!dir_chain_load(fs, cluster, &chain))
        return false;
    uint32_t cursor = 0;
    DirRecord record = {};
    while (dir_scan_record(&chain, &cursor, &record)) {
        if (kstring::strcmp(record.name, ".") == 0 || kstring::strcmp(record.name, "..") == 0)
            continue;
        dir_chain_free(&chain);
        return false;
    }
    dir_chain_free(&chain);
    return true;
}

static int fat32_vfs_unlink(VNode *dir, const char *name)
{
    if (!dir || !name || name[0] == '\0')
        return -1;
    auto *dir_data = static_cast<FAT32Node *>(dir->fs_data);
    if (!dir_data)
        return -1;
    FAT32Filesystem *fs = dir_data->fs;

    DirChain chain;
    if (!dir_chain_load(fs, (uint32_t)dir->inode_id, &chain))
        return -1;
    DirRecord record = {};
    if (!dir_find_record(&chain, name, &record)) {
        dir_chain_free(&chain);
        return -1;
    }
    if (record.is_dir && !fat32_dir_is_empty(fs, record.cluster)) {
        dir_chain_free(&chain);
        return -1;
    }

    for (uint32_t i = 0; i < record.entry_count; i++)
        dir_chain_entry(&chain, record.entry_start_index + i)->name[0] = 0xE5;
    bool ok = dir_chain_save(&chain);
    dir_chain_free(&chain);
    if (!ok)
        return -1;

    if (record.cluster >= 2)
        fat32_free_chain(fs, record.cluster);
    return 0;
}

static bool update_directory_parent(FAT32Filesystem *fs, uint32_t dir_cluster, uint32_t new_parent_cluster)
{
    DirChain chain;
    if (!dir_chain_load(fs, dir_cluster, &chain))
        return false;
    FAT32DirEntry *dotdot = dir_chain_entry(&chain, 1);
    if (!dotdot) {
        dir_chain_free(&chain);
        return false;
    }
    if (new_parent_cluster == fs->root_dir_cluster)
        new_parent_cluster = 0;
    dotdot->cluster_low = (uint16_t)(new_parent_cluster & 0xFFFF);
    dotdot->cluster_high = (uint16_t)((new_parent_cluster >> 16) & 0xFFFF);
    bool ok = dir_chain_save(&chain);
    dir_chain_free(&chain);
    return ok;
}

static int fat32_vfs_rename(VNode *old_dir, const char *old_name, VNode *new_dir, const char *new_name)
{
    if (!old_dir || !new_dir || !old_name || !new_name)
        return -1;
    auto *old_data = static_cast<FAT32Node *>(old_dir->fs_data);
    auto *new_data = static_cast<FAT32Node *>(new_dir->fs_data);
    if (!old_data || !new_data || old_data->fs != new_data->fs)
        return -1;
    FAT32Filesystem *fs = old_data->fs;

    DirChain old_chain;
    if (!dir_chain_load(fs, (uint32_t)old_dir->inode_id, &old_chain))
        return -1;
    DirRecord record = {};
    if (!dir_find_record(&old_chain, old_name, &record)) {
        dir_chain_free(&old_chain);
        return -1;
    }

    if ((uint32_t)old_dir->inode_id == (uint32_t)new_dir->inode_id) {
        bool case_only_rename = name_equal_ci(old_name, new_name) && kstring::strcmp(old_name, new_name) != 0;
        if (!case_only_rename && name_equal_ci(old_name, new_name)) {
            dir_chain_free(&old_chain);
            return 0;
        }
        if (!case_only_rename && dir_find_record(&old_chain, new_name, nullptr)) {
            dir_chain_free(&old_chain);
            return -1;
        }
        for (uint32_t i = 0; i < record.entry_count; i++)
            dir_chain_entry(&old_chain, record.entry_start_index + i)->name[0] = 0xE5;
        FAT32DirEntry entries[21];
        int entry_count =
            build_dir_entries(&old_chain, new_name, record.attr, record.cluster, record.size, entries, 21);
        if (entry_count <= 0) {
            dir_chain_free(&old_chain);
            return -1;
        }
        uint32_t start = 0;
        if (!dir_find_free_range(&old_chain, (uint32_t)entry_count, &start)) {
            dir_chain_free(&old_chain);
            return -1;
        }
        for (int i = 0; i < entry_count; i++)
            *dir_chain_entry(&old_chain, start + (uint32_t)i) = entries[i];
        bool ok = dir_chain_save(&old_chain);
        dir_chain_free(&old_chain);
        return ok ? 0 : -1;
    }

    DirChain new_chain;
    if (!dir_chain_load(fs, (uint32_t)new_dir->inode_id, &new_chain)) {
        dir_chain_free(&old_chain);
        return -1;
    }
    if (dir_find_record(&new_chain, new_name, nullptr)) {
        dir_chain_free(&old_chain);
        dir_chain_free(&new_chain);
        return -1;
    }

    FAT32DirEntry entries[21];
    int entry_count = build_dir_entries(&new_chain, new_name, record.attr, record.cluster, record.size, entries, 21);
    if (entry_count <= 0) {
        dir_chain_free(&old_chain);
        dir_chain_free(&new_chain);
        return -1;
    }
    uint32_t new_start = 0;
    if (!dir_find_free_range(&new_chain, (uint32_t)entry_count, &new_start)) {
        dir_chain_free(&old_chain);
        dir_chain_free(&new_chain);
        return -1;
    }
    for (int i = 0; i < entry_count; i++)
        *dir_chain_entry(&new_chain, new_start + (uint32_t)i) = entries[i];
    if (!dir_chain_save(&new_chain)) {
        dir_chain_free(&old_chain);
        dir_chain_free(&new_chain);
        return -1;
    }
    dir_chain_free(&new_chain);

    for (uint32_t i = 0; i < record.entry_count; i++)
        dir_chain_entry(&old_chain, record.entry_start_index + i)->name[0] = 0xE5;
    bool old_ok = dir_chain_save(&old_chain);
    dir_chain_free(&old_chain);
    if (!old_ok)
        return -1;

    if (record.is_dir)
        update_directory_parent(fs, record.cluster, (uint32_t)new_dir->inode_id);
    return 0;
}

} // namespace

bool fat32_name_requires_lfn(const char *name)
{
    return name_requires_lfn(name);
}

void fat32_format_short_name(const char *name, uint32_t suffix, uint8_t out[11])
{
    format_short_name(name, suffix, out);
}

bool fat32_parse_boot_sector(const uint8_t *boot_sector, FAT32Filesystem *fs_out)
{
    if (!boot_sector || !fs_out)
        return false;

    auto *bs = reinterpret_cast<const FAT32BootSector *>(boot_sector);
    if ((bs->bytes_per_sector != 512 && bs->bytes_per_sector != 4096) || bs->sectors_per_cluster == 0 ||
        bs->sectors_per_fat_32 == 0 || bs->root_cluster < 2 || bs->fat_count == 0) {
        return false;
    }

    *fs_out = {};
    fs_out->bytes_per_sector = bs->bytes_per_sector;
    fs_out->sectors_per_cluster = bs->sectors_per_cluster;
    fs_out->reserved_sectors = bs->reserved_sectors;
    fs_out->fat_count = bs->fat_count;
    fs_out->sectors_per_fat = bs->sectors_per_fat_32;
    fs_out->root_dir_cluster = bs->root_cluster;
    fs_out->fsinfo_sector = bs->fsinfo_sector;
    fs_out->total_sectors = (bs->total_sectors_16 != 0) ? bs->total_sectors_16 : bs->total_sectors_32;
    uint32_t data_start = fs_out->reserved_sectors + (fs_out->fat_count * fs_out->sectors_per_fat);
    if (fs_out->total_sectors <= data_start)
        return false;
    fs_out->cluster_count = (fs_out->total_sectors - data_start) / fs_out->sectors_per_cluster;
    fs_out->next_free_cluster = 2;
    fs_out->free_cluster_count = FAT32_FSINFO_UNKNOWN;
    kstring::memcpy(fs_out->volume_label, bs->volume_label, 11);
    fs_out->volume_label[11] = '\0';
    trim_label(fs_out->volume_label);
    return true;
}

bool fat32_init(BlockDevice *dev, FAT32Filesystem *fs_out)
{
    if (!dev || !fs_out)
        return false;

    uint32_t boot_buffer_size = dev->block_size > 512 ? static_cast<uint32_t>(dev->block_size) : 512u;
    uint8_t *boot_sector = static_cast<uint8_t *>(malloc(boot_buffer_size));
    if (!boot_sector)
        return false;

    if (dev->read_blocks(dev, 0, 1, boot_sector) < 0) {
        DEBUG_ERROR("fat32: failed to read boot sector from %s", dev->name ? dev->name : "(unnamed)");
        free(boot_sector);
        return false;
    }

    FAT32Filesystem parsed = {};
    if (!fat32_parse_boot_sector(boot_sector, &parsed)) {
        free(boot_sector);
        return false;
    }
    free(boot_sector);

    if (dev->block_size != parsed.bytes_per_sector) {
        DEBUG_WARN("fat32: %s block size %llu does not match FAT sector size %u", dev->name ? dev->name : "(unnamed)",
                   dev->block_size, parsed.bytes_per_sector);
        return false;
    }

    *fs_out = parsed;
    fs_out->dev = dev;
    fat32_load_fsinfo(fs_out);

    DEBUG_INFO("fat32: initialized on %s label=%s", dev->name ? dev->name : "(unnamed)",
               fs_out->volume_label[0] ? fs_out->volume_label : "(none)");
    return true;
}

VNode *fat32_get_root(FAT32Filesystem *fs)
{
    if (!fs)
        return nullptr;
    auto *node_data = static_cast<FAT32Node *>(malloc(sizeof(FAT32Node)));
    if (!node_data)
        return nullptr;
    node_data->fs = fs;
    node_data->dir_cluster = fs->root_dir_cluster;
    node_data->entry_start_index = 0;
    node_data->entry_count = 0;
    return vfs_create_vnode(fs->root_dir_cluster, 0, true, &fat32_dir_ops, node_data);
}
