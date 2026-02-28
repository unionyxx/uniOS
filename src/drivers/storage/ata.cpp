#include <drivers/storage/ata.h>
#include <kernel/fs/block_dev.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/debug.h>

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERR          0x1F1
#define ATA_PRIMARY_SECCOUNT     0x1F2
#define ATA_PRIMARY_LBA_LO       0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HI       0x1F5
#define ATA_PRIMARY_DRIVE        0x1F6
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_COMMAND      0x1F7

#define ATA_CMD_READ_PIO         0x20
#define ATA_CMD_WRITE_PIO        0x30
#define ATA_CMD_CACHE_FLUSH      0xE7

#define ATA_STATUS_BSY           0x80
#define ATA_STATUS_DRQ           0x08
#define ATA_STATUS_DF            0x20
#define ATA_STATUS_ERR           0x01

static void ata_wait_bsy() {
    while (inb(ATA_PRIMARY_STATUS) & ATA_STATUS_BSY);
}

static void ata_wait_drq() {
    while (!(inb(ATA_PRIMARY_STATUS) & ATA_STATUS_DRQ));
}

static int64_t ata_read_blocks(BlockDevice* dev, uint64_t lba, uint32_t count, void* buffer) {
    (void)dev;
    uint8_t* out = (uint8_t*)buffer;
    
    for (uint32_t i = 0; i < count; i++) {
        ata_wait_bsy();
        
        uint32_t current_lba = (uint32_t)lba + i;
        outb(ATA_PRIMARY_DRIVE, 0xE0 | ((current_lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_SECCOUNT, 1);
        outb(ATA_PRIMARY_LBA_LO, current_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID, (current_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HI, (current_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_PIO);
        
        ata_wait_bsy();
        ata_wait_drq();
        
        if (inb(ATA_PRIMARY_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            DEBUG_ERROR("ATA: Read error at LBA %d", current_lba);
            return -1;
        }
        
        uint16_t* out16 = (uint16_t*)(out + (i * 512));
        for (int j = 0; j < 256; j++) {
            out16[j] = inw(ATA_PRIMARY_DATA);
        }
    }
    
    return count;
}

static int64_t ata_write_blocks(BlockDevice* dev, uint64_t lba, uint32_t count, const void* buffer) {
    (void)dev;
    const uint8_t* in = (const uint8_t*)buffer;
    
    for (uint32_t i = 0; i < count; i++) {
        ata_wait_bsy();
        
        uint32_t current_lba = (uint32_t)lba + i;
        outb(ATA_PRIMARY_DRIVE, 0xE0 | ((current_lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_SECCOUNT, 1);
        outb(ATA_PRIMARY_LBA_LO, current_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID, (current_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HI, (current_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_PIO);
        
        ata_wait_bsy();
        ata_wait_drq();
        
        DEBUG_INFO("ATA: Writing sector LBA %d", current_lba);
        const uint16_t* in16 = (const uint16_t*)(in + (i * 512));
        for (int j = 0; j < 256; j++) {
            outw(ATA_PRIMARY_DATA, in16[j]);
        }
        
        // Cache flush after each sector for PIO simplicity
        outb(ATA_PRIMARY_COMMAND, ATA_CMD_CACHE_FLUSH);
        ata_wait_bsy();
        
        if (inb(ATA_PRIMARY_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            DEBUG_ERROR("ATA: Write error at LBA %d", current_lba);
            return -1;
        }
    }
    
    return count;
}

static BlockDevice g_ata0_dev;

void ata_init() {
    DEBUG_INFO("ATA: Initializing Primary Master PIO...");
    
    g_ata0_dev.name = "ata0";
    g_ata0_dev.block_size = 512;
    g_ata0_dev.total_blocks = 0; // Unknown for PIO without IDENTIFY, but 64MB image has 131072 blocks
    g_ata0_dev.read_blocks = ata_read_blocks;
    g_ata0_dev.write_blocks = ata_write_blocks;
    g_ata0_dev.private_data = nullptr;
    
    block_dev_register(&g_ata0_dev);
    DEBUG_SUCCESS("ATA: Registered ata0 device");
}
