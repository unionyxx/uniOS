#include <kernel/debug.h>
#include <kernel/fs/block_dev.h>
#include <kernel/fs/partition.h>
#include <kernel/fs/storage_guard.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>

namespace {

struct PartitionDeviceData
{
    BlockDevice *parent;
    uint64_t start_lba;
};

struct [[gnu::packed]] MbrPartitionEntry
{
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t first_lba;
    uint32_t sector_count;
};

struct [[gnu::packed]] GptHeader
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

struct [[gnu::packed]] GptPartitionEntry
{
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
};

static bool guid_all_zero(const uint8_t *guid)
{
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0)
            return false;
    }
    return true;
}

static int64_t partition_read_blocks(BlockDevice *dev, uint64_t lba, uint32_t count, void *buffer)
{
    if (!dev || !dev->private_data || !dev->parent)
        return -1;
    auto *data = static_cast<PartitionDeviceData *>(dev->private_data);
    if (lba + count > dev->total_blocks)
        return -1;
    return dev->parent->read_blocks(dev->parent, data->start_lba + lba, count, buffer);
}

static int64_t partition_write_blocks(BlockDevice *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!dev || !dev->private_data || !dev->parent)
        return -1;
    if (!storage_writes_allowed())
        return -1;
    auto *data = static_cast<PartitionDeviceData *>(dev->private_data);
    if (lba + count > dev->total_blocks)
        return -1;
    return dev->parent->write_blocks(dev->parent, data->start_lba + lba, count, buffer);
}

static char *dup_string(const char *src)
{
    size_t len = src ? kstring::strlen(src) : 0;
    char *dst = static_cast<char *>(malloc(len + 1));
    if (!dst)
        return nullptr;
    if (len != 0)
        kstring::memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

static void utf16le_to_ascii(const uint16_t *src, uint32_t len, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    size_t pos = 0;
    for (uint32_t i = 0; i < len && pos + 1 < out_size; i++) {
        uint16_t cp = src[i];
        if (cp == 0 || cp == 0xFFFF)
            break;
        out[pos++] = (cp >= 32 && cp < 127) ? static_cast<char>(cp) : '_';
    }
    out[pos] = '\0';
}

static bool register_partition(BlockDevice *parent, uint32_t partition_index, uint64_t start_lba, uint64_t block_count,
                               const char *label)
{
    if (!parent || block_count == 0)
        return false;

    auto *part = static_cast<BlockDevice *>(malloc(sizeof(BlockDevice)));
    auto *data = static_cast<PartitionDeviceData *>(malloc(sizeof(PartitionDeviceData)));
    if (!part || !data) {
        free(part);
        free(data);
        return false;
    }

    char name_buf[32] = {};
    kstring::strncpy(name_buf, parent->name ? parent->name : "disk", sizeof(name_buf) - 1);
    kstring::strncat(name_buf, "p", sizeof(name_buf) - 1 - kstring::strlen(name_buf));
    char idx_buf[16];
    kstring::itoa(partition_index, idx_buf, 10);
    kstring::strncat(name_buf, idx_buf, sizeof(name_buf) - 1 - kstring::strlen(name_buf));

    *part = {};
    part->name = dup_string(name_buf);
    kstring::strncpy(part->model, parent->model, sizeof(part->model) - 1);
    if (label && label[0] != '\0') {
        kstring::strncpy(part->display_name, label, sizeof(part->display_name) - 1);
    } else {
        kstring::strncpy(part->display_name, name_buf, sizeof(part->display_name) - 1);
    }
    part->block_size = parent->block_size;
    part->total_blocks = block_count;
    part->is_partition = true;
    part->has_partitions = false;
    part->partition_index = partition_index;
    part->start_lba = start_lba;
    part->parent = parent;
    part->read_blocks = partition_read_blocks;
    part->write_blocks = partition_write_blocks;
    data->parent = parent;
    data->start_lba = start_lba;
    part->private_data = data;

    block_dev_register(part);
    parent->has_partitions = true;
    DEBUG_INFO("partition: registered %s from %s lba=%llu blocks=%llu", part->name ? part->name : "(null)",
               parent->name ? parent->name : "(null)", start_lba, block_count);
    return true;
}

static bool scan_gpt(BlockDevice *dev)
{
    auto *sector = static_cast<uint8_t *>(malloc(static_cast<size_t>(dev->block_size)));
    if (!sector)
        return false;
    if (dev->read_blocks(dev, 1, 1, sector) < 0) {
        free(sector);
        return false;
    }

    PartitionGptInfo info = {};
    if (!partition_parse_gpt_header(sector, (uint32_t)dev->block_size, &info)) {
        free(sector);
        return false;
    }
    free(sector);

    uint32_t entries_per_sector = (uint32_t)dev->block_size / info.entry_size;
    if (entries_per_sector == 0)
        return false;

    uint32_t registered = 0;
    auto *entry_sector = static_cast<uint8_t *>(malloc(static_cast<size_t>(dev->block_size)));
    if (!entry_sector)
        return false;
    for (uint32_t idx = 0; idx < info.entry_count; idx++) {
        if ((idx % entries_per_sector) == 0) {
            uint64_t lba = info.entries_lba + (idx / entries_per_sector);
            if (dev->read_blocks(dev, lba, 1, entry_sector) < 0)
                break;
        }
        PartitionScanEntry parsed = {};
        const uint8_t *entry_ptr = entry_sector + (idx % entries_per_sector) * info.entry_size;
        if (!partition_parse_gpt_entry(entry_ptr, info.entry_size, idx + 1, &parsed))
            continue;
        if (register_partition(dev, parsed.partition_index, parsed.start_lba, parsed.block_count, parsed.label))
            registered++;
    }
    free(entry_sector);
    return registered != 0;
}

static void scan_mbr(BlockDevice *dev)
{
    auto *sector = static_cast<uint8_t *>(malloc(static_cast<size_t>(dev->block_size)));
    if (!sector)
        return;
    if (dev->read_blocks(dev, 0, 1, sector) < 0) {
        free(sector);
        return;
    }
    PartitionScanEntry entries[4];
    int count = partition_parse_mbr_entries(sector, entries, 4);
    for (int i = 0; i < count; i++)
        register_partition(dev, entries[i].partition_index, entries[i].start_lba, entries[i].block_count,
                           entries[i].label);
    free(sector);
}

} // namespace

int partition_parse_mbr_entries(const uint8_t *sector, PartitionScanEntry *out, int max_entries)
{
    if (!sector || !out || max_entries <= 0)
        return 0;
    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return 0;

    auto *entries = reinterpret_cast<const MbrPartitionEntry *>(sector + 446);
    int count = 0;
    for (uint32_t i = 0; i < 4 && count < max_entries; i++) {
        if (entries[i].type == 0 || entries[i].sector_count == 0 || entries[i].type == 0xEE)
            continue;
        out[count] = {};
        out[count].partition_index = i + 1;
        out[count].start_lba = entries[i].first_lba;
        out[count].block_count = entries[i].sector_count;
        out[count].label[0] = '\0';
        count++;
    }
    return count;
}

bool partition_parse_gpt_header(const uint8_t *sector, uint32_t block_size, PartitionGptInfo *out)
{
    if (!sector || !out || block_size < 512)
        return false;
    auto *header = reinterpret_cast<const GptHeader *>(sector);
    static constexpr uint64_t GPT_SIGNATURE = 0x5452415020494645ULL;
    if (header->signature != GPT_SIGNATURE || header->partition_entry_size < sizeof(GptPartitionEntry) ||
        header->partition_entry_count == 0 || header->partition_entry_size > block_size)
        return false;
    out->entries_lba = header->partition_entries_lba;
    out->entry_count = header->partition_entry_count;
    out->entry_size = header->partition_entry_size;
    return true;
}

bool partition_parse_gpt_entry(const uint8_t *entry_bytes, uint32_t entry_size, uint32_t partition_index,
                               PartitionScanEntry *out)
{
    if (!entry_bytes || !out || entry_size < sizeof(GptPartitionEntry) || partition_index == 0)
        return false;
    auto *entry = reinterpret_cast<const GptPartitionEntry *>(entry_bytes);
    if (guid_all_zero(entry->type_guid) || entry->last_lba < entry->first_lba)
        return false;

    *out = {};
    out->partition_index = partition_index;
    out->start_lba = entry->first_lba;
    out->block_count = entry->last_lba - entry->first_lba + 1;
    utf16le_to_ascii(entry->name, 36, out->label, sizeof(out->label));
    return true;
}

void partition_scan_all()
{
    for (BlockDevice *dev = block_dev_first(); dev; dev = dev->next) {
        if (dev->is_partition || dev->has_partitions || !dev->read_blocks || dev->block_size < 512)
            continue;
        if (scan_gpt(dev))
            continue;
        scan_mbr(dev);
    }
}
