#include "pmm.h"
#include "limine.h"

// Limine memory map request
__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// Bitmap for 512MB of RAM (4KB pages)
// 512MB / 4KB = 131072 frames
// 131072 / 8 = 16384 bytes
#define BITMAP_SIZE 16384
static uint8_t bitmap[BITMAP_SIZE];
static uint64_t total_memory = 0;
static uint64_t free_memory = 0;
static uint64_t highest_page = 0;

static void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_unset(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static bool bitmap_test(uint64_t bit) {
    return bitmap[bit / 8] & (1 << (bit % 8));
}

void pmm_init() {
    if (memmap_request.response == NULL) {
        return;
    }

    struct limine_memmap_response* response = memmap_request.response;

    // 1. Mark everything as used initially
    for (int i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF;
    }

    // 2. Iterate through memory map and free usable regions
    for (uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry* entry = response->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            // Align base to 4KB
            uint64_t base = (entry->base + 4095) & ~4095;
            uint64_t length = entry->length;
            
            // If base wasn't aligned, reduce length
            if (base > entry->base) {
                length -= (base - entry->base);
            }
            
            // Align length to 4KB
            length &= ~4095;

            for (uint64_t j = 0; j < length; j += 4096) {
                uint64_t addr = base + j;
                uint64_t frame_idx = addr / 4096;
                
                if (frame_idx < (BITMAP_SIZE * 8)) {
                    bitmap_unset(frame_idx);
                    free_memory += 4096;
                    total_memory += 4096;
                    if (frame_idx > highest_page) highest_page = frame_idx;
                }
            }
        }
    }
}

void* pmm_alloc_frame() {
    for (uint64_t i = 0; i <= highest_page; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_memory -= 4096;
            return (void*)(i * 4096);
        }
    }
    return NULL; // Out of memory
}

void pmm_free_frame(void* frame) {
    uint64_t addr = (uint64_t)frame;
    uint64_t frame_idx = addr / 4096;
    
    if (frame_idx < (BITMAP_SIZE * 8)) {
        if (bitmap_test(frame_idx)) {
            bitmap_unset(frame_idx);
            free_memory += 4096;
        }
    }
}

uint64_t pmm_get_free_memory() {
    return free_memory;
}

uint64_t pmm_get_total_memory() {
    return total_memory;
}
