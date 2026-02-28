#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/debug.h>
#include <kernel/sync/spinlock.h>
#include <kernel/terminal.h>

static Spinlock g_heap_lock = SPINLOCK_INIT;

constexpr size_t   MIN_BUCKET_SIZE = 16;
constexpr size_t   MAX_BUCKET_SIZE = 4096;
constexpr int      NUM_BUCKETS     = 9;
constexpr uint64_t HEAP_MAGIC      = 0xC0FFEE1234567890ULL;

struct FreeBlock {
    FreeBlock* next;
};

struct AllocHeader {
    size_t   size;
    uint64_t magic;
};

static FreeBlock* g_buckets[NUM_BUCKETS];

struct PageSlot {
    uint64_t page_virt;
    int      bucket_idx;
    uint16_t free_count;
    uint16_t total_count;
};

constexpr int MAX_TRACKED_PAGES = 512;
static PageSlot g_page_slots[MAX_TRACKED_PAGES];

static void page_tracker_add(uint64_t page_virt, int bucket_idx, uint16_t total) {
    for (auto& slot : g_page_slots) {
        if (slot.page_virt == 0) {
            slot = { page_virt, bucket_idx, total, total };
            return;
        }
    }
    DEBUG_WARN("heap: page_tracker full, coalescing disabled for page %llx\n", page_virt);
}

[[nodiscard]] static PageSlot* page_tracker_find(uint64_t page_virt) {
    for (auto& slot : g_page_slots) {
        if (slot.page_virt == page_virt) return &slot;
    }
    return nullptr;
}

static void page_tracker_alloc_block(uint64_t block_virt) {
    uint64_t page_virt = block_virt & ~(0xFFFULL);
    if (PageSlot* slot = page_tracker_find(page_virt); slot && slot->free_count > 0) {
        slot->free_count--;
    }
}

[[nodiscard]] static bool page_tracker_free_block(uint64_t block_virt, uint64_t& out_page_virt, int& out_bucket_idx) {
    uint64_t page_virt = block_virt & ~(0xFFFULL);
    PageSlot* slot = page_tracker_find(page_virt);
    if (!slot) return false;

    slot->free_count++;
    if (slot->free_count == slot->total_count) {
        out_page_virt  = slot->page_virt;
        out_bucket_idx = slot->bucket_idx;
        slot->page_virt = 0;
        return true;
    }
    return false;
}

static void bucket_remove_page_blocks(int bucket_idx, uint64_t page_virt) {
    FreeBlock* new_head = nullptr;
    FreeBlock* new_tail = nullptr;
    FreeBlock* cur = g_buckets[bucket_idx];

    while (cur) {
        FreeBlock* nxt = cur->next;
        uint64_t addr = reinterpret_cast<uint64_t>(cur);

        if (addr < page_virt || addr >= page_virt + 4096) {
            cur->next = nullptr;
            if (!new_head) {
                new_head = new_tail = cur;
            } else {
                new_tail->next = cur;
                new_tail = cur;
            }
        }
        cur = nxt;
    }
    g_buckets[bucket_idx] = new_head;
}

[[nodiscard]] static int get_bucket_index(size_t size) {
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

[[nodiscard]] static constexpr size_t get_bucket_size(int index) {
    return static_cast<size_t>(16) << index;
}

void heap_init(void* start, size_t size) {
    (void)start;
    (void)size;
    for (auto& b : g_buckets) b = nullptr;
    for (auto& p : g_page_slots) p = {};
}

// Uses vmm_phys_to_virt as physical RAM is mapped into HHDM.
[[nodiscard]] static void* heap_alloc_large(size_t size) {
    size_t pages = (size + sizeof(AllocHeader) + 4095) / 4096;
    void* ptr = pmm_alloc_frames(pages);
    if (!ptr) [[unlikely]] return nullptr;

    uint64_t virt = vmm_phys_to_virt(reinterpret_cast<uint64_t>(ptr));
    AllocHeader* header = reinterpret_cast<AllocHeader*>(virt);
    header->size  = pages * 4096;
    header->magic = HEAP_MAGIC;
    return reinterpret_cast<void*>(header + 1);
}

[[nodiscard]] static void* malloc_unlocked(size_t size) {
    if (size == 0) [[unlikely]] return nullptr;

    size_t total_size = size + sizeof(AllocHeader);
    if (total_size > MAX_BUCKET_SIZE) [[unlikely]] {
        return heap_alloc_large(size);
    }

    int bucket_idx = get_bucket_index(total_size);
    size_t bucket_size = get_bucket_size(bucket_idx);

    if (g_buckets[bucket_idx]) [[likely]] {
        FreeBlock* block = g_buckets[bucket_idx];
        g_buckets[bucket_idx] = block->next;

        page_tracker_alloc_block(reinterpret_cast<uint64_t>(block));

        AllocHeader* header = reinterpret_cast<AllocHeader*>(block);
        header->size  = bucket_size;
        header->magic = HEAP_MAGIC;
        return reinterpret_cast<void*>(header + 1);
    }

    void* page_phys = pmm_alloc_frame();
    if (!page_phys) [[unlikely]] return nullptr;

    uint64_t page_virt = vmm_phys_to_virt(reinterpret_cast<uint64_t>(page_phys));
    size_t num_blocks = 4096 / bucket_size;

    for (size_t i = 0; i < num_blocks; i++) {
        FreeBlock* block = reinterpret_cast<FreeBlock*>(page_virt + i * bucket_size);
        block->next = g_buckets[bucket_idx];
        g_buckets[bucket_idx] = block;
    }

    page_tracker_add(page_virt, bucket_idx, static_cast<uint16_t>(num_blocks));
    return malloc_unlocked(size);
}

[[nodiscard]] void* malloc(size_t size) {
    spinlock_acquire(&g_heap_lock);
    void* result = malloc_unlocked(size);
    spinlock_release(&g_heap_lock);
    return result;
}

[[nodiscard]] void* aligned_alloc(size_t alignment, size_t size) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);

    size_t total = size + alignment + sizeof(void*);
    void* raw = malloc(total);
    if (!raw) [[unlikely]] return nullptr;

    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
    uintptr_t aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);

    reinterpret_cast<void**>(aligned_addr)[-1] = raw;
    return reinterpret_cast<void*>(aligned_addr);
}

void aligned_free(void* ptr) {
    if (!ptr) [[unlikely]] return;
    free(reinterpret_cast<void**>(ptr)[-1]);
}

void free(void* ptr) {
    if (!ptr) [[unlikely]] return;

    spinlock_acquire(&g_heap_lock);
    AllocHeader* header = reinterpret_cast<AllocHeader*>(ptr) - 1;
    
    if (header->magic != HEAP_MAGIC) [[unlikely]] {
        spinlock_release(&g_heap_lock);
        DEBUG_ERROR("Heap corruption detected at %p (magic: %llx)", ptr, header->magic);
        return;
    }

    size_t size = header->size;
    if (size > MAX_BUCKET_SIZE) {
        size_t pages = size / 4096;
        uint64_t phys = vmm_virt_to_phys(reinterpret_cast<uint64_t>(header));
        for (size_t i = 0; i < pages; i++) {
            pmm_free_frame(reinterpret_cast<void*>(phys + i * 4096));
        }
        spinlock_release(&g_heap_lock);
        return;
    }

    int bucket_idx = get_bucket_index(size);
    uint64_t page_virt_out = 0;
    int page_bucket = 0;
    
    if (page_tracker_free_block(reinterpret_cast<uint64_t>(header), page_virt_out, page_bucket)) {
        bucket_remove_page_blocks(page_bucket, page_virt_out);
        pmm_free_frame(reinterpret_cast<void*>(vmm_virt_to_phys(page_virt_out)));
        spinlock_release(&g_heap_lock);
        return;
    }

    FreeBlock* block = reinterpret_cast<FreeBlock*>(header);
    block->next = g_buckets[bucket_idx];
    g_buckets[bucket_idx] = block;

    spinlock_release(&g_heap_lock);
}

[[nodiscard]] void* operator new(size_t size) { return malloc(size); }
[[nodiscard]] void* operator new[](size_t size) { return malloc(size); }
void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }

extern "C" void heap_dump_stats() {
    g_terminal.write_line("Kernel Heap Stats:");
    g_terminal.write_line("  Bucket Sizes: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096");
    g_terminal.write("  Free Blocks:  ");
    
    spinlock_acquire(&g_heap_lock);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        int count = 0;
        for (FreeBlock* cur = g_buckets[i]; cur; cur = cur->next) count++;
        
        char buf[16];
        int bi = 0;
        if (count == 0) {
            buf[bi++] = '0';
        } else {
            char tmp[16];
            int ti = 0, n = count;
            while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
            while (ti > 0) buf[bi++] = tmp[--ti];
        }
        buf[bi] = '\0';
        g_terminal.write(buf);
        if (i < NUM_BUCKETS - 1) g_terminal.write(", ");
    }
    g_terminal.write("\n");

    int tracked_pages = 0;
    for (const auto& slot : g_page_slots) {
        if (slot.page_virt != 0) tracked_pages++;
    }
    
    g_terminal.write("  Tracked Pages: ");
    char buf[16];
    int bi = 0;
    if (tracked_pages == 0) {
        buf[bi++] = '0';
    } else {
        char tmp[16]; 
        int ti = 0, n = tracked_pages;
        while (n > 0) { tmp[ti++] = '0' + (n % 10); n /= 10; }
        while (ti > 0) buf[bi++] = tmp[--ti];
    }
    buf[bi] = '\0';
    g_terminal.write(buf);
    g_terminal.write(" / 512\n");
    
    spinlock_release(&g_heap_lock);
}
