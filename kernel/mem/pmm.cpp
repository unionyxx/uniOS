#include "pmm.h"
#include "limine.h"
#include "bitmap.h"
#include "debug.h"
#include "spinlock.h"

// PMM lock for thread safety
static Spinlock pmm_lock = SPINLOCK_INIT;

// Limine memory map request
__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// Bitmap for physical memory management
// Support up to 16GB of RAM (4KB pages)
// 16GB / 4KB = 4194304 frames
// 4194304 / 8 = 524288 bytes = 512KB
#define BITMAP_SIZE 524288
static uint8_t pmm_bitmap_buffer[BITMAP_SIZE];
static Bitmap pmm_bitmap;
static size_t bitmap_bits = 0;  // Actual number of bits in use

static uint64_t total_memory = 0;
static uint64_t free_memory = 0;
static uint64_t highest_page = 0;

void pmm_init() {
    if (memmap_request.response == nullptr) {
        return;
    }

    struct limine_memmap_response* response = memmap_request.response;

    // Initialize bitmap - supports up to 16GB of RAM
    bitmap_bits = BITMAP_SIZE * 8;
    pmm_bitmap.init(pmm_bitmap_buffer, bitmap_bits);
    
    // 1. Mark everything as used initially
    pmm_bitmap.set_range(0, bitmap_bits, true);

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
                
                if (frame_idx < bitmap_bits) {
                    pmm_bitmap.set(frame_idx, false); // Mark as free
                    free_memory += 4096;
                    total_memory += 4096;
                    if (frame_idx > highest_page) highest_page = frame_idx;
                }
            }
        }
    }
    
    DEBUG_INFO("PMM: Total: %lu MB, Free: %lu MB (max addressable: %lu MB)", 
               total_memory / 1024 / 1024, free_memory / 1024 / 1024,
               (bitmap_bits * 4096ULL) / 1024 / 1024);
}

void* pmm_alloc_frame() {
    spinlock_acquire(&pmm_lock);
    
    size_t frame_idx = pmm_bitmap.find_first_free();
    
    if (frame_idx != (size_t)-1 && frame_idx <= highest_page) {
        pmm_bitmap.set(frame_idx, true);
        free_memory -= 4096;
        spinlock_release(&pmm_lock);
        return (void*)(frame_idx * 4096);
    }
    
    spinlock_release(&pmm_lock);
    return nullptr; // Out of memory
}

void* pmm_alloc_frames(size_t count) {
    spinlock_acquire(&pmm_lock);
    
    size_t frame_idx = pmm_bitmap.find_first_free_sequence(count);
    
    if (frame_idx != (size_t)-1 && (frame_idx + count - 1) <= highest_page) {
        pmm_bitmap.set_range(frame_idx, count, true);
        free_memory -= (4096 * count);
        spinlock_release(&pmm_lock);
        return (void*)(frame_idx * 4096);
    }
    
    spinlock_release(&pmm_lock);
    return nullptr; // Out of memory
}

void pmm_free_frame(void* frame) {
    spinlock_acquire(&pmm_lock);
    
    uint64_t addr = (uint64_t)frame;
    uint64_t frame_idx = addr / 4096;
    
    if (frame_idx < bitmap_bits) {
        if (pmm_bitmap[frame_idx]) {
            pmm_bitmap.set(frame_idx, false);
            pmm_bitmap.update_hint(frame_idx);  // Update hint for faster reallocation
            free_memory += 4096;
        }
    }
    
    spinlock_release(&pmm_lock);
}

uint64_t pmm_get_free_memory() {
    return free_memory;
}

uint64_t pmm_get_total_memory() {
    return total_memory;
}
