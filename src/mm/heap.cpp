#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/sync/spinlock.h>
#include <kernel/terminal.h>
#include <libk/kstring.h>

static Spinlock g_heap_lock = SPINLOCK_INIT;

constexpr size_t MIN_BUCKET_SIZE = 32;
constexpr size_t MAX_BUCKET_SIZE = 4096;
constexpr int NUM_BUCKETS = 8;
constexpr uint64_t HEAP_MAGIC = 0xC0FFEE1234567890ULL;
constexpr uint64_t ALIGNED_MAGIC = 0x12345678C0FFEE00ULL;
// Scrubbed over HEAP_MAGIC at free time. While a block sits in a free list its
// magic is this sentinel, so a stale aligned header whose round-trip resolves
// to it fails the live-base check in heap_resolve_header.
constexpr uint64_t HEAP_FREED_MAGIC = 0xDEADC0DEDEADBEEFULL;

struct FreeBlock
{
    FreeBlock *next;
};

struct AllocHeader
{
    size_t size;
    uint64_t magic;
};

// Inner header of an aligned allocation, planted in the malloc over-allocation
// padding directly below the returned pointer p, occupying
// [p - sizeof(AlignedHeader), p). base and magic deliberately alias the
// AllocHeader layout (base at p - 16, magic at p - 8) so heap_resolve_header
// can tell plain and aligned blocks apart from the single probe at p - 8.
struct AlignedHeader
{
    uint64_t aligned_offset; // p - base: the exact round-trip partner
    uint64_t base;           // user pointer of the underlying malloc block
    uint64_t magic;          // ALIGNED_MAGIC
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
constexpr uint64_t SLOT_EMPTY = 0;
constexpr uint64_t SLOT_TOMBSTONE = 1;

static PageSlot g_page_slots[MAX_TRACKED_PAGES];

[[nodiscard]] static constexpr bool is_power_of_two(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] static inline bool add_overflow(size_t a, size_t b, size_t *out)
{
    return __builtin_add_overflow(a, b, out);
}

[[nodiscard]] static inline bool mul_overflow(size_t a, size_t b, size_t *out)
{
    return __builtin_mul_overflow(a, b, out);
}

[[nodiscard]] static size_t heap_observed_alignment(const void *ptr)
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

#ifdef DEBUG
// Ktest fault injection: when set, metadata-corruption aborts are reported here
// instead of panicking, so tests can drive the refuse-never-misroute path. Runs
// with IRQs disabled but g_heap_lock RELEASED (callers abort after unlocking) —
// the hook must not touch heap state; record only.
static void (*g_heap_corruption_hook)(void *ptr, const char *reason);
#endif

// Corrupt allocation metadata is a kernel bug: panic in debug builds, log and
// refuse in release (leaking a block is always safer than a misdirected free).
static void heap_corruption_abort(void *ptr, const char *reason)
{
#ifdef DEBUG
    if (g_heap_corruption_hook) {
        g_heap_corruption_hook(ptr, reason);
        return;
    }
    DEBUG_ERROR("heap: corrupt allocation metadata at %p: %s", ptr, reason);
    panic("heap: corrupt allocation metadata (details in log)");
#else
    DEBUG_ERROR("heap: corrupt allocation metadata at %p: %s", ptr, reason);
#endif
}

#ifdef DEBUG
void heap_debug_set_corruption_hook(void (*hook)(void *, const char *))
{
    g_heap_corruption_hook = hook;
}
#endif

// Resolve the AllocHeader owning `user_ptr`, following the aligned-allocation
// indirection when present. Plain blocks return their own header directly.
// Aligned blocks must pass the full validation chain — ANY failure is corrupt
// metadata and aborts; the caller must refuse, never misroute:
//   1. base is sane: non-null, 16-aligned (every heap user pointer is), below p
//   2. aligned_offset covers the inner header itself (>= sizeof(AlignedHeader))
//   3. exact round-trip: base + aligned_offset == p, overflow-free
//   4. the base block is a live allocation: its AllocHeader carries HEAP_MAGIC
//      and its span covers p (freed bases carry HEAP_FREED_MAGIC and fail here)
[[nodiscard]] static AllocHeader *heap_resolve_header(void *user_ptr, void **base_user_ptr_out,
                                                      bool *was_aligned_out = nullptr)
{
    if (base_user_ptr_out)
        *base_user_ptr_out = user_ptr;
    if (was_aligned_out)
        *was_aligned_out = false;
    if (!user_ptr)
        return nullptr;

    AllocHeader *probe = reinterpret_cast<AllocHeader *>(user_ptr) - 1;
    if (probe->magic != ALIGNED_MAGIC)
        return probe;

    uintptr_t user_addr = reinterpret_cast<uintptr_t>(user_ptr);
    AlignedHeader *inner = reinterpret_cast<AlignedHeader *>(user_addr - sizeof(AlignedHeader));
    uintptr_t base_addr = inner->base;
    uintptr_t offset = inner->aligned_offset;

    if (base_addr == 0 || (base_addr & (sizeof(AllocHeader) - 1)) != 0 || base_addr >= user_addr ||
        offset < sizeof(AlignedHeader) || offset != user_addr - base_addr) {
        heap_corruption_abort(user_ptr, "aligned header fails base/offset validation");
        return nullptr;
    }

    AllocHeader *base_header = reinterpret_cast<AllocHeader *>(base_addr) - 1;
    if (base_header->magic != HEAP_MAGIC || base_header->size < sizeof(AllocHeader) ||
        base_header->size - sizeof(AllocHeader) < offset) {
        heap_corruption_abort(user_ptr, "aligned base is not a live allocation");
        return nullptr;
    }

    if (base_user_ptr_out)
        *base_user_ptr_out = reinterpret_cast<void *>(base_addr);
    if (was_aligned_out)
        *was_aligned_out = true;
    return base_header;
}

static inline uint32_t hash_page(uint64_t virt)
{
    virt ^= virt >> 33;
    virt *= 0xff51afd7ed558ccdULL;
    virt ^= virt >> 33;
    return static_cast<uint32_t>(virt & (MAX_TRACKED_PAGES - 1));
}

[[nodiscard]] static bool page_tracker_add(uint64_t page_virt, int bucket_idx, uint16_t total)
{
    uint32_t idx = hash_page(page_virt);
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) {
        uint32_t probe = (idx + i) & (MAX_TRACKED_PAGES - 1);
        if (g_page_slots[probe].page_virt == SLOT_EMPTY || g_page_slots[probe].page_virt == SLOT_TOMBSTONE) {
            g_page_slots[probe] = {page_virt, bucket_idx, total, total};
            return true;
        }
    }
    DEBUG_WARN("heap: page_tracker full, coalescing disabled for page %llx", (unsigned long long)page_virt);
    return false;
}

[[nodiscard]] static PageSlot *page_tracker_find(uint64_t page_virt)
{
    uint32_t idx = hash_page(page_virt);
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) {
        uint32_t probe = (idx + i) & (MAX_TRACKED_PAGES - 1);
        if (g_page_slots[probe].page_virt == page_virt)
            return &g_page_slots[probe];
        if (g_page_slots[probe].page_virt == SLOT_EMPTY)
            return nullptr;
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
        slot->page_virt = SLOT_TOMBSTONE;
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

[[nodiscard]] static inline int get_bucket_index(size_t size)
{
    if (size <= 32)
        return 0;
    if (size > 4096)
        return -1;
    return 64 - __builtin_clzll(size - 1) - 5;
}

[[nodiscard]] static constexpr size_t get_bucket_size(int index)
{
    return static_cast<size_t>(32) << index;
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
    if (total_size > MAX_BUCKET_SIZE) [[unlikely]]
        return heap_alloc_large(total_size);

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

    if (!page_tracker_add(page_virt, bucket_idx, static_cast<uint16_t>(num_blocks))) {
        pmm_free_frame(page_phys);
        return nullptr;
    }

    for (size_t i = 0; i < num_blocks; i++) {
        FreeBlock *block = reinterpret_cast<FreeBlock *>(page_virt + i * bucket_size);
        block->next = g_buckets[bucket_idx];
        g_buckets[bucket_idx] = block;
    }

    return malloc_unlocked(size);
}

[[nodiscard]] void *malloc(size_t size)
{
    uint64_t flags = spinlock_acquire_irqsave(&g_heap_lock);
    void *result = malloc_unlocked(size);
    spinlock_release_irqrestore(&g_heap_lock, flags);
    return result;
}

[[nodiscard]] void *aligned_alloc(size_t alignment, size_t size)
{
    if (!is_power_of_two(alignment))
        return nullptr;
    if (alignment <= 16)
        return malloc(size);

    // Over-allocate for the base AllocHeader, the inner AlignedHeader below the
    // aligned address, and the alignment slack.
    size_t total = 0;
    if (add_overflow(size, alignment - 1, &total) || add_overflow(total, sizeof(AllocHeader), &total) ||
        add_overflow(total, sizeof(AlignedHeader), &total))
        return nullptr;

    const void *raw = malloc(total);
    if (!raw) [[unlikely]]
        return nullptr;

    uintptr_t raw_addr = reinterpret_cast<uintptr_t>(raw);
    // Rounding up past sizeof(AlignedHeader) guarantees the inner header at
    // [aligned_addr - sizeof(AlignedHeader), aligned_addr) stays inside the
    // base block's user area.
    uintptr_t aligned_addr = (raw_addr + sizeof(AlignedHeader) + alignment - 1) & ~(alignment - 1);

    AlignedHeader *inner = reinterpret_cast<AlignedHeader *>(aligned_addr - sizeof(AlignedHeader));
    inner->aligned_offset = aligned_addr - raw_addr;
    inner->base = raw_addr;
    inner->magic = ALIGNED_MAGIC;

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

    uint64_t flags = spinlock_acquire_irqsave(&g_heap_lock);

    bool was_aligned = false;
    AllocHeader *header = heap_resolve_header(ptr, nullptr, &was_aligned);
    if (!header) [[unlikely]] {
        // heap_resolve_header already aborted loudly on the failed chain.
        spinlock_release_irqrestore(&g_heap_lock, flags);
        return;
    }
    if (header->magic != HEAP_MAGIC) [[unlikely]] {
        spinlock_release_irqrestore(&g_heap_lock, flags);
        heap_corruption_abort(ptr, "block header does not carry HEAP_MAGIC");
        return;
    }

    if (was_aligned) {
        // Load-bearing scrub: the inner header dies with the free. A recycled
        // block must never carry a live-looking aligned header, or a stale
        // interior pointer (base + aligned_offset) would validate fully and
        // misdirect the free into whatever lives at the base now.
        AlignedHeader *inner =
            reinterpret_cast<AlignedHeader *>(reinterpret_cast<uintptr_t>(ptr) - sizeof(AlignedHeader));
        kstring::zero_memory(inner, sizeof(AlignedHeader));
    }

    size_t size = header->size;
    header->magic = HEAP_FREED_MAGIC;

    if (size > MAX_BUCKET_SIZE) {
        size_t pages = size / 4096;
        uint64_t phys = vmm_virt_to_phys(reinterpret_cast<uint64_t>(header));
        spinlock_release_irqrestore(&g_heap_lock, flags);
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
        spinlock_release_irqrestore(&g_heap_lock, flags);
        pmm_free_frame(reinterpret_cast<void *>(vmm_virt_to_phys(page_virt_out)));
        return;
    }

    FreeBlock *block = reinterpret_cast<FreeBlock *>(header);
    block->next = g_buckets[bucket_idx];
    g_buckets[bucket_idx] = block;

    spinlock_release_irqrestore(&g_heap_lock, flags);
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

    // Resolve and validate the header under the heap lock: a concurrent free
    // of `ptr` (or of its aligned base) would otherwise turn the metadata read
    // into a race on freed/reused memory. The caller still guarantees `ptr`
    // stays valid for the duration of realloc.
    void *base_user_ptr = ptr;
    bool was_aligned = false;
    size_t old_size = 0;

    {
        uint64_t flags = spinlock_acquire_irqsave(&g_heap_lock);
        const AllocHeader *header = heap_resolve_header(ptr, &base_user_ptr, &was_aligned);

        if (!header) [[unlikely]] {
            // heap_resolve_header already aborted loudly on the failed chain.
            spinlock_release_irqrestore(&g_heap_lock, flags);
            return nullptr;
        }
        if (header->magic != HEAP_MAGIC || header->size < sizeof(AllocHeader)) [[unlikely]] {
            spinlock_release_irqrestore(&g_heap_lock, flags);
            heap_corruption_abort(ptr, "realloc header failed validation");
            return nullptr;
        }

        size_t total_usable = header->size - sizeof(AllocHeader);
        uintptr_t user_addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base_addr = reinterpret_cast<uintptr_t>(base_user_ptr);

        if (user_addr < base_addr) {
            spinlock_release_irqrestore(&g_heap_lock, flags);
            return nullptr;
        }

        size_t alignment_slack = static_cast<size_t>(user_addr - base_addr);
        if (alignment_slack > total_usable) {
            spinlock_release_irqrestore(&g_heap_lock, flags);
            return nullptr;
        }

        old_size = total_usable - alignment_slack;
        spinlock_release_irqrestore(&g_heap_lock, flags);
    }

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

static void print_uint(uint64_t val)
{
    if (val == 0) {
        g_terminal.write("0");
        return;
    }
    char buf[32];
    int i = 0;
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) {
        char c[2] = {buf[--i], '\0'};
        g_terminal.write(c);
    }
}

extern "C" void heap_dump_stats()
{
    g_terminal.write_line("Kernel Heap Stats:");
    g_terminal.write_line("  Bucket Sizes: 32, 64, 128, 256, 512, 1024, 2048, 4096");
    g_terminal.write("  Free Blocks:  ");

    uint64_t flags = spinlock_acquire_irqsave(&g_heap_lock);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        int count = 0;
        for (const FreeBlock *cur = g_buckets[i]; cur; cur = cur->next)
            count++;
        print_uint(static_cast<uint64_t>(count));
        if (i < NUM_BUCKETS - 1)
            g_terminal.write(", ");
    }
    g_terminal.write("\n");

    int tracked_pages = 0;
    for (const auto &slot : g_page_slots) {
        if (slot.page_virt != SLOT_EMPTY && slot.page_virt != SLOT_TOMBSTONE)
            tracked_pages++;
    }

    g_terminal.write("  Tracked Pages: ");
    print_uint(static_cast<uint64_t>(tracked_pages));
    g_terminal.write(" / ");
    print_uint(static_cast<uint64_t>(MAX_TRACKED_PAGES));
    g_terminal.write("\n");

    spinlock_release_irqrestore(&g_heap_lock, flags);
}