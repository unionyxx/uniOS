#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>

// Heap lock for thread safety
static Spinlock heap_lock = SPINLOCK_INIT;

// Bucket allocator implementation
// Buckets for sizes: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
// FIXED: replaced #define macros with typed constexpr values
constexpr size_t   MIN_BUCKET_SIZE = 16;
constexpr size_t   MAX_BUCKET_SIZE = 4096;
constexpr int      NUM_BUCKETS     = 9;
constexpr uint64_t HEAP_MAGIC      = 0xC0FFEE1234567890ULL;

struct FreeBlock {
    FreeBlock* next;
};

struct AllocHeader {
    size_t   size;   // Rounded up to bucket size, includes this header
    uint64_t magic;
};

static FreeBlock* buckets[NUM_BUCKETS];

// ============================================================================
// Page Coalescing Tracker
//
// Tracks per-page free-block counts so fully-freed pages can be returned to
// the PMM.  Without this, freed small blocks strand entire physical pages in
// the bucket lists forever and cause long-term memory fragmentation.
// ============================================================================
struct PageSlot {
    uint64_t page_virt;    // HHDM virtual base of the tracked page (0 = unused)
    int      bucket_idx;   // which bucket this page feeds
    uint16_t free_count;   // blocks currently in the free list for this page
    uint16_t total_count;  // total blocks the page was originally split into
};

constexpr int MAX_TRACKED_PAGES = 512;
static PageSlot page_slots[MAX_TRACKED_PAGES];

// Register a freshly-split page.
static void page_tracker_add(uint64_t page_virt, int bucket_idx, uint16_t total) {
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) {
        if (page_slots[i].page_virt == 0) {
            page_slots[i] = { page_virt, bucket_idx, total, total };
            return;
        }
    }
    // Tracker full – page will work correctly, just won't be coalesced.
    DEBUG_WARN("heap: page_tracker full, coalescing disabled for page %llx\n", page_virt);
}

// Find the slot for a page-aligned virtual address.
static PageSlot* page_tracker_find(uint64_t page_virt) {
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) {
        if (page_slots[i].page_virt == page_virt) return &page_slots[i];
    }
    return nullptr;
}

// Called when a block is popped from the free list (allocation path).
static void page_tracker_alloc_block(uint64_t block_virt) {
    uint64_t page_virt = block_virt & ~(uint64_t)0xFFF;
    PageSlot* slot = page_tracker_find(page_virt);
    if (slot && slot->free_count > 0) slot->free_count--;
}

// Called when a block is returned by the user (free path).
// Returns true and fills *out_page_virt / *out_bucket_idx when the entire
// backing page is now free and should be returned to the PMM.
static bool page_tracker_free_block(uint64_t block_virt,
                                     uint64_t* out_page_virt,
                                     int*      out_bucket_idx) {
    uint64_t  page_virt = block_virt & ~(uint64_t)0xFFF;
    PageSlot* slot      = page_tracker_find(page_virt);
    if (!slot) return false;

    slot->free_count++;
    if (slot->free_count == slot->total_count) {
        *out_page_virt  = slot->page_virt;
        *out_bucket_idx = slot->bucket_idx;
        slot->page_virt = 0;  // release the tracking slot
        return true;
    }
    return false;
}

// Sweep a bucket list and drop every block that belongs to page_virt.
// Called just before the page is handed back to the PMM.
// The block currently being freed is NOT yet in the bucket, so it is
// naturally skipped here.
static void bucket_remove_page_blocks(int bucket_idx, uint64_t page_virt) {
    FreeBlock* new_head = nullptr;
    FreeBlock* new_tail = nullptr;
    FreeBlock* cur      = buckets[bucket_idx];

    while (cur) {
        FreeBlock* nxt  = cur->next;
        uint64_t   addr = (uint64_t)cur;

        if (addr >= page_virt && addr < page_virt + 4096) {
            // Block belongs to the page being reclaimed – drop it
        } else {
            cur->next = nullptr;
            if (!new_head) { new_head = new_tail = cur; }
            else           { new_tail->next = cur; new_tail = cur; }
        }
        cur = nxt;
    }
    buckets[bucket_idx] = new_head;
}

// ============================================================================

static int get_bucket_index(size_t size) {
    if (size <=   16) return 0;
    if (size <=   32) return 1;
    if (size <=   64) return 2;
    if (size <=  128) return 3;
    if (size <=  256) return 4;
    if (size <=  512) return 5;
    if (size <= 1024) return 6;
    if (size <= 2048) return 7;
    if (size <= 4096) return 8;
    return -1;
}

static size_t get_bucket_size(int index) {
    return static_cast<size_t>(16) << index;
}

void heap_init(void* start, size_t size) {
    for (int i = 0; i < NUM_BUCKETS; i++)      buckets[i]     = nullptr;
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) page_slots[i] = {};
    (void)start;
    (void)size;
}

// NOTE: Large allocations use vmm_phys_to_virt() (the HHDM identity mapping)
// to produce a kernel virtual address.  This is always valid under the Limine
// boot protocol which maps all physical RAM in the HHDM.  If KASLR or memory
// hot-plug is ever introduced, replace this with an explicit vmm_map_pages()
// call so the mapping is guaranteed rather than assumed.
[[nodiscard]] static void* heap_alloc_large(size_t size) {
    size_t pages = (size + sizeof(AllocHeader) + 4095) / 4096;
    void*  ptr   = pmm_alloc_frames(pages);
    if (!ptr) [[unlikely]] return nullptr;

    uint64_t     virt   = vmm_phys_to_virt((uint64_t)ptr);
    AllocHeader* header = (AllocHeader*)virt;
    header->size  = pages * 4096;
    header->magic = HEAP_MAGIC;
    return (void*)(header + 1);
}

// Internal malloc without lock (called from within an already-locked context)
[[nodiscard]] static void* malloc_unlocked(size_t size) {
    if (size == 0) [[unlikely]] return nullptr;

    size_t total_size = size + sizeof(AllocHeader);

    if (total_size > MAX_BUCKET_SIZE) [[unlikely]] {
        return heap_alloc_large(size);
    }

    int    bucket_idx  = get_bucket_index(total_size);
    size_t bucket_size = get_bucket_size(bucket_idx);

    if (buckets[bucket_idx]) [[likely]] {
        FreeBlock* block    = buckets[bucket_idx];
        buckets[bucket_idx] = block->next;

        // Notify tracker: one block is no longer in the free list
        page_tracker_alloc_block((uint64_t)block);

        AllocHeader* header = (AllocHeader*)block;
        header->size  = bucket_size;
        header->magic = HEAP_MAGIC;
        return (void*)(header + 1);
    }

    // Bucket empty – allocate a fresh physical page and split it into blocks
    void* page_phys = pmm_alloc_frame();
    if (!page_phys) [[unlikely]] return nullptr;

    uint64_t page_virt  = vmm_phys_to_virt((uint64_t)page_phys);
    size_t   num_blocks = 4096 / bucket_size;

    for (size_t i = 0; i < num_blocks; i++) {
        FreeBlock* block    = (FreeBlock*)(page_virt + i * bucket_size);
        block->next         = buckets[bucket_idx];
        buckets[bucket_idx] = block;
    }

    // Register the page so free() can coalesce it back to the PMM later
    page_tracker_add(page_virt, bucket_idx, (uint16_t)num_blocks);

    return malloc_unlocked(size);  // recursive call now finds a block
}

[[nodiscard]] void* malloc(size_t size) {
    spinlock_acquire(&heap_lock);
    void* result = malloc_unlocked(size);
    spinlock_release(&heap_lock);
    return result;
}

// Allocate memory with specified alignment.
// alignment must be a power of 2 and >= sizeof(void*).
[[nodiscard]] void* aligned_alloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);

    size_t total = size + alignment + sizeof(void*);
    void*  raw   = malloc(total);
    if (!raw) [[unlikely]] return nullptr;

    uintptr_t raw_addr     = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);

    // Store original pointer just before the aligned address for aligned_free()
    ((void**)aligned_addr)[-1] = raw;
    return (void*)aligned_addr;
}

void aligned_free(void* ptr) {
    if (!ptr) [[unlikely]] return;
    void* raw = ((void**)ptr)[-1];
    free(raw);
}

void free(void* ptr) {
    if (!ptr) [[unlikely]] return;

    spinlock_acquire(&heap_lock);

    AllocHeader* header = (AllocHeader*)ptr - 1;
    if (header->magic != HEAP_MAGIC) [[unlikely]] {
        spinlock_release(&heap_lock);
        DEBUG_ERROR("Heap corruption detected at %p (magic: %llx)", ptr, header->magic);
        return;
    }

    size_t size = header->size;

    if (size > MAX_BUCKET_SIZE) {
        // Large allocation – free all backing physical pages
        size_t   pages = size / 4096;
        uint64_t virt  = (uint64_t)header;
        uint64_t phys  = vmm_virt_to_phys(virt);
        for (size_t i = 0; i < pages; i++) {
            pmm_free_frame((void*)(phys + i * 4096));
        }
        spinlock_release(&heap_lock);
        return;
    }

    int bucket_idx = get_bucket_index(size);

    // === Coalescing: is this page now entirely free? ===
    uint64_t page_virt_out = 0;
    int      page_bucket   = 0;
    bool     page_free     = page_tracker_free_block((uint64_t)header,
                                                      &page_virt_out,
                                                      &page_bucket);
    if (page_free) {
        // Remove the other (total-1) free blocks of this page from the bucket.
        // The block being freed right now is NOT in the bucket yet, so it is
        // correctly skipped by bucket_remove_page_blocks.
        bucket_remove_page_blocks(page_bucket, page_virt_out);
        // Return the physical page to the PMM
        uint64_t phys = vmm_virt_to_phys(page_virt_out);
        pmm_free_frame((void*)phys);
        spinlock_release(&heap_lock);
        return;
    }

    // Page still has live allocations – return block to its bucket
    FreeBlock* block = (FreeBlock*)header;
    block->next          = buckets[bucket_idx];
    buckets[bucket_idx]  = block;

    spinlock_release(&heap_lock);
}

// C++ operator overloads
void* operator new  (size_t size)                   { return malloc(size); }
void* operator new[](size_t size)                   { return malloc(size); }
void  operator delete  (void* ptr) noexcept         { free(ptr); }
void  operator delete[](void* ptr) noexcept         { free(ptr); }
void  operator delete  (void* ptr, size_t) noexcept { free(ptr); }
void  operator delete[](void* ptr, size_t) noexcept { free(ptr); }
