#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/sync/spinlock.h>
#include <kernel/terminal.h>
#include <libk/kstring.h>

static Spinlock g_heap_lock = SPINLOCK_INIT;

constexpr size_t MIN_BUCKET_SIZE = 16;
constexpr size_t MAX_BUCKET_SIZE = 4096;
constexpr int NUM_BUCKETS = 9;
constexpr uint64_t HEAP_MAGIC = 0xC0FFEE1234567890ULL;
constexpr uint64_t ALIGNED_MAGIC = 0x12345678C0FFEE00ULL;

struct FreeBlock
{
    FreeBlock *next;
};

struct AllocHeader
{
    size_t size;
    uint64_t magic;
};

static FreeBlock *g_buckets[NUM_BUCKETS];

struct PageSlot
{
    uint64_t page_virt;
    int bucket_idx;
    uint16_t free_count;
    uint16_t total_count;
};

constexpr int MAX_TRACKED_PAGES = 65536;
static PageSlot g_page_slots[MAX_TRACKED_PAGES];

[[nodiscard]] static bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] static bool add_overflow(size_t a, size_t b, size_t *out)
{
    return __builtin_add_overflow(a, b, out);
}

[[nodiscard]] static bool mul_overflow(size_t a, size_t b, size_t *out)
{
    return __builtin_mul_overflow(a, b, out);
}

[[nodiscard]] static size_t heap_observed_alignment(void *ptr)
{
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr == 0)
        return 16;

    size_t alignment = static_cast<size_t>(addr & (~addr + 1));
    if (alignment < 16)
        alignment = 16;
    if (alignment > 4096)
        alignment = 4096;
    return alignment;
}

[[nodiscard]] static AllocHeader *heap_resolve_header(void *user_ptr, void **base_user_ptr_out,
                                                      bool *was_aligned_out = nullptr)
{
    if (base_user_ptr_out)
        *base_user_ptr_out = user_ptr;
    if (was_aligned_out)
        *was_aligned_out = false;
    if (!user_ptr)
        return nullptr;

    AllocHeader *header = reinterpret_cast<AllocHeader *>(user_ptr) - 1;
    if (header->magic != ALIGNED_MAGIC)
        return header;

    uintptr_t raw_addr = static_cast<uintptr_t>(header->size);
    uintptr_t user_addr = reinterpret_cast<uintptr_t>(user_ptr);
    if (raw_addr == 0 || raw_addr >= user_addr)
        return nullptr;

    if (base_user_ptr_out)
        *base_user_ptr_out = reinterpret_cast<void *>(raw_addr);
    if (was_aligned_out)
        *was_aligned_out = true;

    return reinterpret_cast<AllocHeader *>(raw_addr) - 1;
}

static void page_tracker_add(uint64_t page_virt, int bucket_idx, uint16_t total)
{
    for (auto &slot : g_page_slots) {
        if (slot.page_virt == 0) {
            slot = {page_virt, bucket_idx, total, total};
            return;
        }
    }
    DEBUG_WARN("heap: page_tracker full, coalescing disabled for page %llx\n", page_virt);
}

[[nodiscard]] static PageSlot *page_tracker_find(uint64_t page_virt)
{
    for (auto &slot : g_page_slots) {
        if (slot.page_virt == page_virt)
            return &slot;
    }
    return nullptr;
}

static void page_tracker_alloc_block(uint64_t block_virt)
{
    uint64_t page_virt = block_virt & ~(0xFFFULL);
    if (PageSlot *slot = page_tracker_find(page_virt); slot && slot->free_count > 0) {
        slot->free_count--;
    }
}

[[nodiscard]] static bool page_tracker_free_block(uint64_t block_virt, uint64_t &out_page_virt, int &out_bucket_idx)
{
    uint64_t page_virt = block_virt & ~(0xFFFULL);
    PageSlot *slot = page_tracker_find(page_virt);
    if (!slot)
        return false;

    slot->free_count++;
    if (slot->free_count == slot->total_count) {
        out_page_virt = slot->page_virt;
        out_bucket_idx = slot->bucket_idx;
        slot->page_virt = 0;
        return true;
    }
    return false;
}

static void bucket_remove_page_blocks(int bucket_idx, uint64_t page_virt)
{
    FreeBlock *new_head = nullptr;
    FreeBlock *new_tail = nullptr;
    FreeBlock *cur = g_buckets[bucket_idx];

    while (cur) {
        FreeBlock *nxt = cur->next;
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

[[nodiscard]] static int get_bucket_index(size_t size)
{
    if (size <= 16)
        return 0;
    if (size <= 32)
        return 1;
    if (size <= 64)
        return 2;
    if (size <= 128)
        return 3;
    if (size <= 256)
        return 4;
    if (size <= 512)
        return 5;
    if (size <= 1024)
        return 6;
    if (size <= 2048)
        return 7;
    if (size <= 4096)
        return 8;
    return -1;
}

[[nodiscard]] static constexpr size_t get_bucket_size(int index)
{
    return static_cast<size_t>(16) << index;
}

void heap_init(void *start, size_t size)
{
    (void)start;
    (void)size;
    for (auto &b : g_buckets)
        b = nullptr;
    for (auto &p : g_page_slots)
        p = {};
}

[[nodiscard]] static void *heap_alloc_large(size_t total_size)
{
    size_t pages = (total_size + 4095) / 4096;
    void *ptr = pmm_alloc_frames(pages);
    if (!ptr) [[unlikely]]
        return nullptr;

    uint64_t virt = vmm_phys_to_virt(reinterpret_cast<uint64_t>(ptr));
    AllocHeader *header = reinterpret_cast<AllocHeader *>(virt);
    header->size = pages * 4096;
    header->magic = HEAP_MAGIC;
    return reinterpret_cast<void *>(header + 1);
}

[[nodiscard]] static void *malloc_unlocked(size_t size)
{
    if (size == 0) [[unlikely]]
        return nullptr;

    size_t total_size = 0;
    if (add_overflow(size, sizeof(AllocHeader), &total_size)) [[unlikely]]
        return nullptr;
    if (total_size > MAX_BUCKET_SIZE) [[unlikely]] {
        return heap_alloc_large(total_size);
    }

    int bucket_idx = get_bucket_index(total_size);
    size_t bucket_size = get_bucket_size(bucket_idx);

    if (g_buckets[bucket_idx]) [[likely]] {
        FreeBlock *block = g_buckets[bucket_idx];
        g_buckets[bucket_idx] = block->next;

        page_tracker_alloc_block(reinterpret_cast<uint64_t>(block));

        AllocHeader *header = reinterpret_cast<AllocHeader *>(block);
        header->size = bucket_size;
        header->magic = HEAP_MAGIC;
        return reinterpret_cast<void *>(header + 1);
    }

    void *page_phys = pmm_alloc_frame();
    if (!page_phys) [[unlikely]]
        return nullptr;

    uint64_t page_virt = vmm_phys_to_virt(reinterpret_cast<uint64_t>(page_phys));
    size_t num_blocks = 4096 / bucket_size;

    for (size_t i = 0; i < num_blocks; i++) {
        FreeBlock *block = reinterpret_cast<FreeBlock *>(page_virt + i * bucket_size);
        block->next = g_buckets[bucket_idx];
        g_buckets[bucket_idx] = block;
    }

    page_tracker_add(page_virt, bucket_idx, static_cast<uint16_t>(num_blocks));
    return malloc_unlocked(size);
}

[[nodiscard]] void *malloc(size_t size)
{
    spinlock_acquire(&g_heap_lock);
    void *result = malloc_unlocked(size);
    spinlock_release(&g_heap_lock);
    return result;
}

[[nodiscard]] void *aligned_alloc(size_t alignment, size_t size)
{
    if (!is_power_of_two(alignment))
        return nullptr;
    if (alignment <= 16)
        return malloc(size);

    size_t total = 0;
    if (add_overflow(size, alignment - 1, &total))
        return nullptr;
    if (add_overflow(total, sizeof(AllocHeader), &total))
        return nullptr;
    void *raw = malloc(total);
    if (!raw) [[unlikely]]
        return nullptr;

    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
    uintptr_t aligned_addr = (raw_addr + sizeof(AllocHeader) + alignment - 1) & ~(alignment - 1);

    AllocHeader *align_header = reinterpret_cast<AllocHeader *>(aligned_addr) - 1;
    align_header->size = raw_addr;
    align_header->magic = ALIGNED_MAGIC;

    return reinterpret_cast<void *>(aligned_addr);
}

void aligned_free(void *ptr)
{
    free(ptr);
}

void free(void *ptr)
{
    if (!ptr) [[unlikely]]
        return;

    spinlock_acquire(&g_heap_lock);

    void *base_user_ptr = ptr;
    AllocHeader *header = heap_resolve_header(ptr, &base_user_ptr);
    if (!header || header->magic != HEAP_MAGIC) [[unlikely]] {
        spinlock_release(&g_heap_lock);
        DEBUG_ERROR("Heap corruption detected at %p", ptr);
        return;
    }

    size_t size = header->size;
    header->magic = 0;

    if (size > MAX_BUCKET_SIZE) {
        size_t pages = size / 4096;
        uint64_t phys = vmm_virt_to_phys(reinterpret_cast<uint64_t>(header));
        spinlock_release(&g_heap_lock);
        for (size_t i = 0; i < pages; i++) {
            pmm_free_frame(reinterpret_cast<void *>(phys + i * 4096));
        }
        return;
    }

    int bucket_idx = get_bucket_index(size);
    uint64_t page_virt_out = 0;
    int page_bucket = 0;

    if (page_tracker_free_block(reinterpret_cast<uint64_t>(header), page_virt_out, page_bucket)) {
        bucket_remove_page_blocks(page_bucket, page_virt_out);
        spinlock_release(&g_heap_lock);
        pmm_free_frame(reinterpret_cast<void *>(vmm_virt_to_phys(page_virt_out)));
        return;
    }

    FreeBlock *block = reinterpret_cast<FreeBlock *>(header);
    block->next = g_buckets[bucket_idx];
    g_buckets[bucket_idx] = block;

    spinlock_release(&g_heap_lock);
}

[[nodiscard]] void *calloc(size_t nmemb, size_t size)
{
    size_t total = 0;
    if (mul_overflow(nmemb, size, &total))
        return nullptr;
    void *ptr = malloc(total);
    if (ptr)
        kstring::zero_memory(ptr, total);
    return ptr;
}

[[nodiscard]] void *realloc(void *ptr, size_t size)
{
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return nullptr;
    }

    void *base_user_ptr = ptr;
    bool was_aligned = false;
    AllocHeader *header = heap_resolve_header(ptr, &base_user_ptr, &was_aligned);
    if (!header || header->magic != HEAP_MAGIC)
        return nullptr;
    if (header->size < sizeof(AllocHeader))
        return nullptr;

    size_t total_usable = header->size - sizeof(AllocHeader);
    uintptr_t user_addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(base_user_ptr);
    if (user_addr < base_addr)
        return nullptr;

    size_t alignment_slack = static_cast<size_t>(user_addr - base_addr);
    if (alignment_slack > total_usable)
        return nullptr;

    size_t old_size = total_usable - alignment_slack;
    if (size <= old_size)
        return ptr;

    void *new_ptr = was_aligned ? aligned_alloc(heap_observed_alignment(ptr), size) : malloc(size);
    if (!new_ptr)
        return nullptr;

    kstring::memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

[[nodiscard]] void *operator new(size_t size)
{
    return malloc(size);
}
[[nodiscard]] void *operator new[](size_t size)
{
    return malloc(size);
}
void operator delete(void *ptr) noexcept
{
    free(ptr);
}
void operator delete[](void *ptr) noexcept
{
    free(ptr);
}
void operator delete(void *ptr, size_t) noexcept
{
    free(ptr);
}
void operator delete[](void *ptr, size_t) noexcept
{
    free(ptr);
}

extern "C" void heap_dump_stats()
{
    g_terminal.write_line("Kernel Heap Stats:");
    g_terminal.write_line("  Bucket Sizes: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096");
    g_terminal.write("  Free Blocks:  ");

    spinlock_acquire(&g_heap_lock);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        int count = 0;
        for (FreeBlock *cur = g_buckets[i]; cur; cur = cur->next)
            count++;

        char buf[16];
        int bi = 0;
        if (count == 0) {
            buf[bi++] = '0';
        } else {
            char tmp[16];
            int ti = 0, n = count;
            while (n > 0) {
                tmp[ti++] = '0' + (n % 10);
                n /= 10;
            }
            while (ti > 0)
                buf[bi++] = tmp[--ti];
        }
        buf[bi] = '\0';
        g_terminal.write(buf);
        if (i < NUM_BUCKETS - 1)
            g_terminal.write(", ");
    }
    g_terminal.write("\n");

    int tracked_pages = 0;
    for (const auto &slot : g_page_slots) {
        if (slot.page_virt != 0)
            tracked_pages++;
    }

    g_terminal.write("  Tracked Pages: ");
    char buf[16];
    int bi = 0;
    if (tracked_pages == 0) {
        buf[bi++] = '0';
    } else {
        char tmp[16];
        int ti = 0, n = tracked_pages;
        while (n > 0) {
            tmp[ti++] = '0' + (n % 10);
            n /= 10;
        }
        while (ti > 0)
            buf[bi++] = tmp[--ti];
    }
    buf[bi] = '\0';
    g_terminal.write(buf);
    g_terminal.write(" / ");

    char cap_buf[16];
    bi = 0;
    int cap = MAX_TRACKED_PAGES;
    if (cap == 0) {
        cap_buf[bi++] = '0';
    } else {
        char tmp[16];
        int ti = 0;
        while (cap > 0) {
            tmp[ti++] = '0' + (cap % 10);
            cap /= 10;
        }
        while (ti > 0)
            cap_buf[bi++] = tmp[--ti];
    }
    cap_buf[bi] = '\0';
    g_terminal.write(cap_buf);
    g_terminal.write("\n");

    spinlock_release(&g_heap_lock);
}
