#include <kernel/mm/pmm.h>
#include <boot/limine.h>
#include <kernel/mm/bitmap.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>

static Spinlock pmm_lock = SPINLOCK_INIT;

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
    .response = nullptr
};

static uint8_t* pmm_bitmap_buffer = nullptr;
static Bitmap pmm_bitmap;
static uint16_t* pmm_refcounts = nullptr;
static size_t bitmap_bits = 0;

static uint64_t total_memory = 0;
static uint64_t free_memory = 0;
static uint64_t highest_page = 0;

void pmm_init() {
    if (memmap_request.response == nullptr) {
        return;
    }

    struct limine_memmap_response* response = memmap_request.response;

    uint64_t highest_phys_addr = 0;
    for (uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry* entry = response->entries[i];
        if (entry->type != LIMINE_MEMMAP_RESERVED && entry->type != LIMINE_MEMMAP_BAD_MEMORY) {
            uint64_t end = entry->base + entry->length;
            if (end > highest_phys_addr) {
                highest_phys_addr = end;
            }
        }
    }

    uint64_t max_frames = highest_phys_addr / 4096;
    bitmap_bits = max_frames;
    uint64_t bitmap_size_bytes = (max_frames + 7) / 8;
    bitmap_size_bytes = (bitmap_size_bytes + 4095) & ~4095;
    
    uint64_t refcounts_size_bytes = (max_frames * sizeof(uint16_t) + 4095) & ~4095;

    uint64_t bitmap_phys_addr = 0;
    uint64_t refcounts_phys_addr = 0;
    
    for (uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry* entry = response->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t base = (entry->base + 4095) & ~4095;
            uint64_t length = entry->length;
            if (base > entry->base) {
                length -= (base - entry->base);
            }
            
            if (bitmap_phys_addr == 0 && length >= bitmap_size_bytes) {
                bitmap_phys_addr = base;
                if (length >= bitmap_size_bytes + refcounts_size_bytes) {
                    refcounts_phys_addr = base + bitmap_size_bytes;
                }
            } else if (refcounts_phys_addr == 0 && length >= refcounts_size_bytes) {
                refcounts_phys_addr = base;
            }
            
            if (bitmap_phys_addr != 0 && refcounts_phys_addr != 0) break;
        }
    }

    if (bitmap_phys_addr == 0 || refcounts_phys_addr == 0) {
        panic("PMM: Failed to find memory for bitmap or refcounts");
    }

    pmm_bitmap_buffer = (uint8_t*)(bitmap_phys_addr + vmm_get_hhdm_offset());
    pmm_bitmap.init(pmm_bitmap_buffer, bitmap_bits);
    
    pmm_refcounts = (uint16_t*)(refcounts_phys_addr + vmm_get_hhdm_offset());
    for (size_t i = 0; i < bitmap_bits; i++) pmm_refcounts[i] = 0;
    
    pmm_bitmap.set_range(0, bitmap_bits, true);

    uint64_t usable_memory = 0;
    uint64_t reserved_memory = 0;
    
    for (uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry* entry = response->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_memory += entry->length;
            uint64_t base = (entry->base + 4095) & ~4095;
            uint64_t length = entry->length;
            
            if (base > entry->base) {
                length -= (base - entry->base);
            }
            
            length &= ~4095;

            for (uint64_t j = 0; j < length; j += 4096) {
                uint64_t addr = base + j;
                uint64_t frame_idx = addr / 4096;
                
                if (frame_idx < bitmap_bits) {
                    pmm_bitmap.set(frame_idx, false);
                    free_memory += 4096;
                    total_memory += 4096;
                    if (frame_idx > highest_page) highest_page = frame_idx;
                }
            }
        } else {
            reserved_memory += entry->length;
        }
    }
    
    DEBUG_INFO("Memory Map: %d entries, Usable: %lu MB, Reserved: %lu MB", 
               response->entry_count, usable_memory / 1024 / 1024, reserved_memory / 1024 / 1024);

    uint64_t bitmap_start_frame = bitmap_phys_addr / 4096;
    uint64_t bitmap_frame_count = bitmap_size_bytes / 4096;
    pmm_bitmap.set_range(bitmap_start_frame, bitmap_frame_count, true);
    for (size_t i = 0; i < bitmap_frame_count; i++) pmm_refcounts[bitmap_start_frame + i] = 1;
    free_memory -= bitmap_size_bytes;

    uint64_t refcounts_start_frame = refcounts_phys_addr / 4096;
    uint64_t refcounts_frame_count = refcounts_size_bytes / 4096;
    pmm_bitmap.set_range(refcounts_start_frame, refcounts_frame_count, true);
    for (size_t i = 0; i < refcounts_frame_count; i++) pmm_refcounts[refcounts_start_frame + i] = 1;
    free_memory -= refcounts_size_bytes;
    
    DEBUG_SUCCESS("Physical Memory Manager Initialized");
}

void* pmm_alloc_frame() {
    spinlock_acquire(&pmm_lock);
    
    size_t frame_idx = pmm_bitmap.find_first_free();
    
    if (frame_idx != (size_t)-1 && frame_idx <= highest_page) {
        pmm_bitmap.set(frame_idx, true);
        pmm_refcounts[frame_idx] = 1;
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
        for (size_t i = 0; i < count; i++) pmm_refcounts[frame_idx + i] = 1;
        free_memory -= (4096 * count);
        spinlock_release(&pmm_lock);
        return (void*)(frame_idx * 4096);
    }
    
    spinlock_release(&pmm_lock);
    return nullptr; // Out of memory
}

void pmm_refcount_inc(void* frame) {
    spinlock_acquire(&pmm_lock);
    uint64_t addr = (uint64_t)frame;
    uint64_t frame_idx = addr / 4096;
    if (frame_idx < bitmap_bits) {
        pmm_refcounts[frame_idx]++;
    }
    spinlock_release(&pmm_lock);
}

void pmm_refcount_dec(void* frame) {
    spinlock_acquire(&pmm_lock);
    uint64_t addr = (uint64_t)frame;
    uint64_t frame_idx = addr / 4096;
    if (frame_idx < bitmap_bits) {
        if (pmm_refcounts[frame_idx] > 0) {
            pmm_refcounts[frame_idx]--;
            if (pmm_refcounts[frame_idx] == 0) {
                pmm_bitmap.set(frame_idx, false);
                pmm_bitmap.update_hint(frame_idx);
                free_memory += 4096;
            }
        }
    }
    spinlock_release(&pmm_lock);
}

uint16_t pmm_get_refcount(void* frame) {
    spinlock_acquire(&pmm_lock);
    uint64_t addr = (uint64_t)frame;
    uint64_t frame_idx = addr / 4096;
    uint16_t count = (frame_idx < bitmap_bits) ? pmm_refcounts[frame_idx] : 0;
    spinlock_release(&pmm_lock);
    return count;
}

void pmm_free_frame(void* frame) {
    pmm_refcount_dec(frame);
}

uint64_t pmm_get_free_memory() {
    return free_memory;
}

uint64_t pmm_get_total_memory() {
    return total_memory;
}
