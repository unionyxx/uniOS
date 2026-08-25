#include <drivers/storage/ata.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>
#include <kernel/fs/block_dev.h>
#include <kernel/fs/storage_guard.h>
#include <libk/kstring.h>

#define ATA_PRIMARY_DATA 0x1F0
#define ATA_PRIMARY_ERR 0x1F1
#define ATA_PRIMARY_SECCOUNT 0x1F2
#define ATA_PRIMARY_LBA_LO 0x1F3
#define ATA_PRIMARY_LBA_MID 0x1F4
#define ATA_PRIMARY_LBA_HI 0x1F5
#define ATA_PRIMARY_DRIVE 0x1F6
#define ATA_PRIMARY_STATUS 0x1F7
#define ATA_PRIMARY_COMMAND 0x1F7

#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA

#define ATA_STATUS_BSY 0x80
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_DF 0x20
#define ATA_STATUS_ERR 0x01

static void ata_extract_model(const uint16_t *identify, char *out, size_t out_size)
{
    if (!identify || !out || out_size == 0)
        return;
    size_t pos = 0;
    for (int i = 27; i <= 46 && pos + 1 < out_size; i++) {
        uint16_t word = identify[i];
        char hi = (char)(word >> 8);
        char lo = (char)(word & 0xFF);
        if (hi >= 32 && hi < 127)
            out[pos++] = hi;
        if (lo >= 32 && lo < 127 && pos + 1 < out_size)
            out[pos++] = lo;
    }
    while (pos > 0 && out[pos - 1] == ' ')
        pos--;
    out[pos] = '\0';
}

#include <kernel/sync/spinlock.h>

// Serializes register access, PIO transfers and the sector cache: the IDE
// task-file is a single global resource and concurrent callers would corrupt
// each other's commands mid-flight.
static Spinlock g_ata_lock = SPINLOCK_INIT;

static bool ata_wait_bsy()
{
    uint32_t timeout = 1000000;
    while (timeout > 0) {
        if (!(inb(ATA_PRIMARY_STATUS) & ATA_STATUS_BSY))
            return true;
        timeout--;
    }
    return false;
}

static bool ata_wait_drq()
{
    uint32_t timeout = 1000000;
    while (timeout > 0) {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if ((status & ATA_STATUS_ERR) || (status & ATA_STATUS_DF))
            return false;
        if (status & ATA_STATUS_DRQ)
            return true;
        timeout--;
    }
    return false;
}

static bool ata_identify(uint16_t identify[256])
{
    if (!identify)
        return false;
    if (!ata_wait_bsy())
        return false;

    outb(ATA_PRIMARY_DRIVE, 0xA0);
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MID, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0 || status == 0xFF)
        return false;
    if (inb(ATA_PRIMARY_LBA_MID) != 0 || inb(ATA_PRIMARY_LBA_HI) != 0)
        return false;

    uint32_t timeout = 100000;
    while (timeout-- > 0) {
        status = inb(ATA_PRIMARY_STATUS);
        if ((status & ATA_STATUS_ERR) != 0)
            return false;
        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ) != 0)
            break;
    }
    if (timeout == 0)
        return false;

    for (int i = 0; i < 256; i++)
        identify[i] = inw(ATA_PRIMARY_DATA);
    return true;
}

// 48-bit LBA PIO: the old 28-bit commands silently wrapped LBAs above
// 2^28, reading/writing the wrong sectors on any disk larger than 128 GB.
// Callers must hold g_ata_lock.
static int64_t ata_pio_read_sector(uint64_t lba, uint8_t *out)
{
    if (lba >= (1ULL << 48))
        return -1;
    if (!ata_wait_bsy())
        return -1;

    outb(ATA_PRIMARY_DRIVE, 0xE0); // LBA mode, drive 0; 48-bit commands take the full LBA
    outb(ATA_PRIMARY_SECCOUNT, 0); // count high: 0 in 48-bit means 65536; write low after
    outb(ATA_PRIMARY_LBA_LO, (lba >> 24) & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 32) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 40) & 0xFF);
    outb(ATA_PRIMARY_SECCOUNT, 1);
    outb(ATA_PRIMARY_LBA_LO, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_PIO_EXT);

    if (!ata_wait_bsy())
        return -1;
    if (!ata_wait_drq())
        return -1;

    if (inb(ATA_PRIMARY_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        DEBUG_ERROR("ATA: read error at LBA %llu", lba);
        return -1;
    }

    uint16_t *out16 = (uint16_t *)out;
    for (int j = 0; j < 256; j++) {
        out16[j] = inw(ATA_PRIMARY_DATA);
    }
    return 1;
}

// Callers must hold g_ata_lock. Does NOT flush the device cache; batch
// flushes with ata_pio_flush() instead of paying one flush per sector.
static int64_t ata_pio_write_sector(uint64_t lba, const uint8_t *in)
{
    if (lba >= (1ULL << 48))
        return -1;
    if (!ata_wait_bsy())
        return -1;

    outb(ATA_PRIMARY_DRIVE, 0xE0);
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, (lba >> 24) & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 32) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 40) & 0xFF);
    outb(ATA_PRIMARY_SECCOUNT, 1);
    outb(ATA_PRIMARY_LBA_LO, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_PIO_EXT);

    if (!ata_wait_bsy())
        return -1;
    if (!ata_wait_drq())
        return -1;

    const uint16_t *in16 = (const uint16_t *)in;
    for (int j = 0; j < 256; j++) {
        outw(ATA_PRIMARY_DATA, in16[j]);
    }

    if (!ata_wait_bsy())
        return -1;
    if (inb(ATA_PRIMARY_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        DEBUG_ERROR("ATA: write error at LBA %llu", lba);
        return -1;
    }
    return 1;
}

static bool ata_pio_flush()
{
    if (!ata_wait_bsy())
        return false;
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_CACHE_FLUSH_EXT);
    return ata_wait_bsy();
}

// Block Cache Implementation
#define CACHE_ENTRIES 256
#define HASH_BUCKETS 64

struct CacheEntry
{
    uint64_t lba;
    uint8_t data[512];
    bool valid;
    bool dirty;
    CacheEntry *lru_prev;
    CacheEntry *lru_next;
    CacheEntry *hash_next;
};

static CacheEntry g_cache[CACHE_ENTRIES];
static CacheEntry *g_hash_table[HASH_BUCKETS] = {nullptr};
static CacheEntry *g_lru_head = nullptr;
static CacheEntry *g_lru_tail = nullptr;

static void lru_remove(CacheEntry *entry)
{
    if (entry->lru_prev)
        entry->lru_prev->lru_next = entry->lru_next;
    else
        g_lru_head = entry->lru_next;
    if (entry->lru_next)
        entry->lru_next->lru_prev = entry->lru_prev;
    else
        g_lru_tail = entry->lru_prev;
}

static void lru_push_front(CacheEntry *entry)
{
    entry->lru_next = g_lru_head;
    entry->lru_prev = nullptr;
    if (g_lru_head)
        g_lru_head->lru_prev = entry;
    g_lru_head = entry;
    if (!g_lru_tail)
        g_lru_tail = entry;
}

static CacheEntry *cache_lookup(uint64_t lba)
{
    uint32_t bucket = static_cast<uint32_t>(lba % HASH_BUCKETS);
    CacheEntry *cur = g_hash_table[bucket];
    while (cur) {
        if (cur->valid && cur->lba == lba) {
            lru_remove(cur);
            lru_push_front(cur);
            return cur;
        }
        cur = cur->hash_next;
    }
    return nullptr;
}

static CacheEntry *cache_evict()
{
    CacheEntry *entry = g_lru_tail; // Evict LRU
    if (!entry)
        return nullptr;

    if (entry->valid) {
        if (entry->dirty) {
            ata_pio_write_sector(entry->lba, entry->data);
            entry->dirty = false;
        }
        // Remove from hash table
        uint32_t bucket = static_cast<uint32_t>(entry->lba % HASH_BUCKETS);
        CacheEntry **ptr = &g_hash_table[bucket];
        while (*ptr) {
            if (*ptr == entry) {
                *ptr = entry->hash_next;
                break;
            }
            ptr = &(*ptr)->hash_next;
        }
    }

    lru_remove(entry);
    return entry;
}

static CacheEntry *cache_insert(uint64_t lba, const uint8_t *data, bool dirty)
{
    CacheEntry *entry = cache_lookup(lba);
    if (!entry) {
        entry = cache_evict();
        if (!entry)
            return nullptr;

        entry->valid = true;
        entry->lba = lba;
        uint32_t bucket = static_cast<uint32_t>(lba % HASH_BUCKETS);
        entry->hash_next = g_hash_table[bucket];
        g_hash_table[bucket] = entry;
    }

    if (data)
        kstring::memcpy(entry->data, data, 512);
    entry->dirty = dirty;
    lru_push_front(entry);
    return entry;
}

void ata_cache_init()
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        g_cache[i].valid = false;
        g_cache[i].dirty = false;
        lru_push_front(&g_cache[i]);
    }
}

void ata_cache_flush_all()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_ata_lock);
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (g_cache[i].valid && g_cache[i].dirty) {
            ata_pio_write_sector(g_cache[i].lba, g_cache[i].data);
            g_cache[i].dirty = false;
        }
    }
    ata_pio_flush();
    spinlock_release_irqrestore(&g_ata_lock, flags);
}

static int64_t ata_read_blocks(BlockDevice *dev, uint64_t lba, uint32_t count, void *buffer)
{
    if (!dev || count == 0)
        return -1;
    // Range-check the full request: an out-of-bounds LBA must fail loudly, not
    // wrap around into unrelated sectors.
    if (lba >= dev->total_blocks || static_cast<uint64_t>(count) > dev->total_blocks - lba)
        return -1;
    uint8_t *out = (uint8_t *)buffer;

    uint64_t flags = spinlock_acquire_irqsave(&g_ata_lock);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t current_lba = lba + i;
        CacheEntry *entry = cache_lookup(current_lba);

        if (entry) {
            kstring::memcpy(out + (i * 512), entry->data, 512);
        } else {
            if (ata_pio_read_sector(current_lba, out + (i * 512)) < 0) {
                spinlock_release_irqrestore(&g_ata_lock, flags);
                return i != 0 ? static_cast<int64_t>(i) : -1;
            }
            cache_insert(current_lba, out + (i * 512), false);

            // Read-ahead one sector synchronously (bounded to the device).
            if (i == count - 1 && current_lba + 1 < dev->total_blocks) {
                CacheEntry *next_entry = cache_lookup(current_lba + 1);
                if (!next_entry) {
                    uint8_t temp[512];
                    if (ata_pio_read_sector(current_lba + 1, temp) > 0) {
                        cache_insert(current_lba + 1, temp, false);
                    }
                }
            }
        }
    }
    spinlock_release_irqrestore(&g_ata_lock, flags);
    return count;
}

static int64_t ata_write_blocks(BlockDevice *dev, uint64_t lba, uint32_t count, const void *buffer)
{
    if (!dev || count == 0)
        return -1;
    if (!storage_writes_allowed())
        return -1;
    if (lba >= dev->total_blocks || static_cast<uint64_t>(count) > dev->total_blocks - lba)
        return -1;
    const uint8_t *in = (const uint8_t *)buffer;

    uint64_t flags = spinlock_acquire_irqsave(&g_ata_lock);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t current_lba = lba + i;
        if (ata_pio_write_sector(current_lba, in + (i * 512)) < 0) {
            spinlock_release_irqrestore(&g_ata_lock, flags);
            return i != 0 ? static_cast<int64_t>(i) : -1;
        }
        cache_insert(current_lba, in + (i * 512), false);
    }
    // One device-cache flush per request instead of per sector: the old
    // per-sector flush serialized every 512 bytes behind a full drive flush.
    ata_pio_flush();
    spinlock_release_irqrestore(&g_ata_lock, flags);
    return count;
}

static BlockDevice g_ata0_dev;

void ata_init()
{
    // Guard against floating bus on missing hardware (0xFF returns all bits set, including BSY)
    if (inb(ATA_PRIMARY_STATUS) == 0xFF) {
        DEBUG_INFO("ATA: no IDE controller found at 0x1F0 (floating bus detected)");
        return;
    }

    uint16_t identify[256] = {};
    if (!ata_identify(identify)) {
        DEBUG_INFO("ATA: no primary master ATA device detected");
        return;
    }

    DEBUG_INFO("ATA: initializing Primary Master PIO...");

    ata_cache_init();

    g_ata0_dev.name = "ata0";
    ata_extract_model(identify, g_ata0_dev.model, sizeof(g_ata0_dev.model));
    if (g_ata0_dev.model[0] != '\0')
        kstring::strncpy(g_ata0_dev.display_name, g_ata0_dev.model, sizeof(g_ata0_dev.display_name) - 1);
    else
        kstring::strncpy(g_ata0_dev.display_name, "ata0", sizeof(g_ata0_dev.display_name) - 1);
    g_ata0_dev.block_size = 512;
    g_ata0_dev.total_blocks = ((uint64_t)identify[103] << 48) | ((uint64_t)identify[102] << 32) |
                              ((uint64_t)identify[101] << 16) | identify[100];
    if (g_ata0_dev.total_blocks == 0)
        g_ata0_dev.total_blocks = ((uint64_t)identify[61] << 16) | identify[60];
    g_ata0_dev.is_partition = false;
    g_ata0_dev.has_partitions = false;
    g_ata0_dev.partition_index = 0;
    g_ata0_dev.start_lba = 0;
    g_ata0_dev.parent = nullptr;
    g_ata0_dev.read_blocks = ata_read_blocks;
    g_ata0_dev.write_blocks = ata_write_blocks;
    g_ata0_dev.private_data = nullptr;

    block_dev_register(&g_ata0_dev);
    DEBUG_SUCCESS("ATA: registered ata0 (%s, %llu sectors)", g_ata0_dev.model[0] ? g_ata0_dev.model : "unknown",
                  g_ata0_dev.total_blocks);
}
