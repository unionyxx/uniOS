#include "pmm.h"
#include "limine.h"
#include "bitmap.h"
#include "debug.h"

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
static uint8_t pmm_bitmap_buffer[BITMAP_SIZE];
static Bitmap pmm_bitmap;

static uint64_t total_memory = 0;
static uint64_t free_memory = 0;
static uint64_t highest_page = 0;

void pmm_init() {
    if (memmap_request.response == NULL) {
        return;
    }

    struct limine_memmap_response* response = memmap_request.response;

    // Initialize bitmap
    pmm_bitmap.init(pmm_bitmap_buffer, BITMAP_SIZE * 8);
    
    // 1. Mark everything as used initially
    pmm_bitmap.set_range(0, BITMAP_SIZE * 8, true);

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
                    pmm_bitmap.set(frame_idx, false); // Mark as free
                    free_memory += 4096;
                    total_memory += 4096;
                    if (frame_idx > highest_page) highest_page = frame_idx;
                }
            }
        }
    }
    
    DEBUG_INFO("PMM: Total Memory: %lu MB, Free Memory: %lu MB", total_memory / 1024 / 1024, free_memory / 1024 / 1024);
}

void* pmm_alloc_frame() {
    size_t frame_idx = pmm_bitmap.find_first_free();
    
    if (frame_idx != (size_t)-1 && frame_idx <= highest_page) {
        pmm_bitmap.set(frame_idx, true);
        free_memory -= 4096;
        return (void*)(frame_idx * 4096);
    }
    
    return NULL; // Out of memory
}

void* pmm_alloc_frames(size_t count) {
    size_t frame_idx = pmm_bitmap.find_first_free_sequence(count);
    
    if (frame_idx != (size_t)-1 && (frame_idx + count - 1) <= highest_page) {
        pmm_bitmap.set_range(frame_idx, count, true);
        free_memory -= (4096 * count);
        return (void*)(frame_idx * 4096);
    }
    
    return NULL; // Out of memory
}

void pmm_free_frame(void* frame) {
    uint64_t addr = (uint64_t)frame;
    uint64_t frame_idx = addr / 4096;
    
    if (frame_idx < (BITMAP_SIZE * 8)) {
        if (pmm_bitmap[frame_idx]) {
            pmm_bitmap.set(frame_idx, false);
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
