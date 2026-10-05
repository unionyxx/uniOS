#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <libk/kstring.h>

KTEST(heap_malloc_free)
{
    void *p1 = malloc(16);
    KTEST_EXPECT(p1 != nullptr);

    void *p2 = malloc(1024);
    KTEST_EXPECT(p2 != nullptr);
    KTEST_EXPECT(p1 != p2);

    free(p1);
    free(p2);
}

KTEST(heap_realloc)
{
    void *p1 = malloc(32);
    KTEST_EXPECT(p1 != nullptr);
    kstring::memset(p1, 0xAA, 32);

    void *p1_new = realloc(p1, 64);
    KTEST_EXPECT(p1_new != nullptr);

    // Check if data was preserved
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(p1_new);
    for (int i = 0; i < 32; i++) {
        KTEST_EXPECT_EQ(ptr[i], 0xAA);
    }

    free(p1_new);
}

KTEST(heap_calloc)
{
    void *p = calloc(10, 64);
    KTEST_EXPECT(p != nullptr);
    if (!p)
        return;

    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(p);
    for (int i = 0; i < 640; i++) {
        KTEST_EXPECT_EQ(ptr[i], 0);
    }

    free(p);
}

KTEST(heap_calloc_overflow)
{
    void *p = calloc(static_cast<size_t>(-1), 64);
    KTEST_EXPECT(p == nullptr);
}

KTEST(heap_aligned_alloc_free)
{
    constexpr size_t alignments[] = {32, 64, 512, 4096};
    for (size_t alignment : alignments) {
        void *p = aligned_alloc(alignment, 100);
        KTEST_EXPECT(p != nullptr);
        KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(p) & (alignment - 1), static_cast<uintptr_t>(0));

        kstring::memset(p, 0x5A, 100);
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(p);
        for (int i = 0; i < 100; i++)
            KTEST_EXPECT_EQ(bytes[i], 0x5A);

        aligned_free(p);
    }

    // alignment <= 16 must take the plain malloc path (16 is malloc's guarantee)
    void *plain = aligned_alloc(16, 64);
    KTEST_EXPECT(plain != nullptr);
    free(plain);

    KTEST_EXPECT(aligned_alloc(0, 64) == nullptr);
    KTEST_EXPECT(aligned_alloc(48, 64) == nullptr);
}

KTEST(heap_aligned_realloc)
{
    void *p = aligned_alloc(64, 48);
    KTEST_EXPECT(p != nullptr);
    kstring::memset(p, 0xC3, 48);

    // Growing an aligned block must preserve the alignment contract and data.
    void *grown = realloc(p, 256);
    KTEST_EXPECT(grown != nullptr);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(grown) & 63, static_cast<uintptr_t>(0));

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(grown);
    for (int i = 0; i < 48; i++)
        KTEST_EXPECT_EQ(bytes[i], 0xC3);

    free(grown);
}

#ifdef DEBUG
// White-box mirrors of the heap's private AlignedHeader (src/mm/heap.cpp):
// 24 bytes ending at p — {aligned_offset, base, magic} with base/magic aliasing
// the AllocHeader slots. Keep in sync with the heap implementation.
constexpr size_t HEAP_TEST_INNER_SIZE = 24;
constexpr size_t HEAP_TEST_INNER_OFF_OFFSET = 24;
constexpr size_t HEAP_TEST_INNER_OFF_BASE = 16;
constexpr size_t HEAP_TEST_INNER_OFF_MAGIC = 8;
constexpr uint64_t HEAP_TEST_ALIGNED_MAGIC = 0x12345678C0FFEE00ULL;

extern void heap_debug_set_corruption_hook(void (*hook)(void *ptr, const char *reason));

static int heap_test_corruption_events = 0;
static void *heap_test_corruption_ptr = nullptr;

static void heap_test_corruption_hook(void *ptr, const char *reason)
{
    (void)reason;
    heap_test_corruption_events++;
    heap_test_corruption_ptr = ptr;
}

static void heap_test_reset_corruption()
{
    heap_test_corruption_events = 0;
    heap_test_corruption_ptr = nullptr;
}

static void heap_test_write_inner(uintptr_t p, uint64_t base, uint64_t offset)
{
    *reinterpret_cast<uint64_t *>(p - HEAP_TEST_INNER_OFF_OFFSET) = offset;
    *reinterpret_cast<uint64_t *>(p - HEAP_TEST_INNER_OFF_BASE) = base;
    *reinterpret_cast<uint64_t *>(p - HEAP_TEST_INNER_OFF_MAGIC) = HEAP_TEST_ALIGNED_MAGIC;
}

KTEST(heap_aligned_stale_inner_free_refused)
{
    heap_debug_set_corruption_hook(heap_test_corruption_hook);
    heap_test_reset_corruption();

    // 1. Aligned allocation; capture the inner header before the free.
    void *aligned = aligned_alloc(64, 128);
    KTEST_EXPECT(aligned != nullptr);
    uintptr_t aligned_addr = reinterpret_cast<uintptr_t>(aligned);

    KTEST_EXPECT_EQ(*reinterpret_cast<uint64_t *>(aligned_addr - HEAP_TEST_INNER_OFF_MAGIC), HEAP_TEST_ALIGNED_MAGIC);
    uint64_t stale_base = *reinterpret_cast<uint64_t *>(aligned_addr - HEAP_TEST_INNER_OFF_BASE);
    uint64_t stale_offset = *reinterpret_cast<uint64_t *>(aligned_addr - HEAP_TEST_INNER_OFF_OFFSET);
    KTEST_EXPECT(stale_base != 0);
    KTEST_EXPECT(stale_offset >= HEAP_TEST_INNER_SIZE);
    KTEST_EXPECT_EQ(aligned_addr - stale_base, stale_offset);

    // 2. Keepalive from the same bucket (256), allocated after the aligned
    //    block: it keeps the base's page from returning to the PMM when the
    //    base frees, so the slot recycles through the bucket LIFO list —
    //    otherwise the same-slot repro below depends on PMM frame reuse.
    //    (aligned_alloc(64, 128) requests 231 bytes -> malloc adds 16 ->
    //    bucket 256; malloc(240) -> 240 + 16 = 256 -> same bucket.)
    void *keep = malloc(240);
    KTEST_EXPECT(keep != nullptr);

    // 3. Free: the inner header must be scrubbed so it can never validate later.
    free(aligned);
    KTEST_EXPECT_EQ(*reinterpret_cast<uint64_t *>(aligned_addr - HEAP_TEST_INNER_OFF_MAGIC), 0ULL);
    KTEST_EXPECT_EQ(*reinterpret_cast<uint64_t *>(aligned_addr - HEAP_TEST_INNER_OFF_BASE), 0ULL);
    KTEST_EXPECT_EQ(*reinterpret_cast<uint64_t *>(aligned_addr - HEAP_TEST_INNER_OFF_OFFSET), 0ULL);

    // 4. Plain allocation landing on the same slot (free list is LIFO).
    void *plain = malloc(240);
    KTEST_EXPECT(plain != nullptr);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(plain), stale_base);

    kstring::memset(plain, 0x77, 240);
    heap_test_reset_corruption();

    // 5. The stale free the old bug allowed: base + stale offset == the old
    //    aligned pointer, pointing into the live plain allocation. Pre-fix, the
    //    stale inner header validated and misdirected the free into `plain`.
    void *stale_ptr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(plain) + stale_offset);
    free(stale_ptr);

    // 6. Refused loudly, exactly once — never misdirected.
    KTEST_EXPECT_EQ(heap_test_corruption_events, 1);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(heap_test_corruption_ptr), reinterpret_cast<uintptr_t>(stale_ptr));

    // 7. `plain` is still live and the heap keeps cycling the slot cleanly: a
    //    misroute would have freed `plain` here (double free -> corruption event).
    heap_test_reset_corruption();
    free(plain);
    KTEST_EXPECT_EQ(heap_test_corruption_events, 0);

    void *again = malloc(240);
    KTEST_EXPECT(again != nullptr);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(again), stale_base);
    free(again);

    free(keep);
    heap_debug_set_corruption_hook(nullptr);
}

KTEST(heap_aligned_validation_chain_refused)
{
    heap_debug_set_corruption_hook(heap_test_corruption_hook);

    // Crafted inner headers inside a live plain block's user data: the chain
    // must refuse each one before trusting any base pointer.
    uint8_t *buf = static_cast<uint8_t *>(malloc(512));
    KTEST_EXPECT(buf != nullptr);
    uintptr_t q = reinterpret_cast<uintptr_t>(buf) + 128;

    // (1) null base
    heap_test_reset_corruption();
    heap_test_write_inner(q, 0, 64);
    free(reinterpret_cast<void *>(q));
    KTEST_EXPECT_EQ(heap_test_corruption_events, 1);

    // (2) broken round-trip: base + offset != p
    heap_test_reset_corruption();
    heap_test_write_inner(q, q - 64, 999);
    free(reinterpret_cast<void *>(q));
    KTEST_EXPECT_EQ(heap_test_corruption_events, 1);

    // (3) offset too small to hold the inner header itself (round-trip is exact)
    heap_test_reset_corruption();
    heap_test_write_inner(q, q - 16, 16);
    free(reinterpret_cast<void *>(q));
    KTEST_EXPECT_EQ(heap_test_corruption_events, 1);

    // (4) exact round-trip onto a freed base: the freed sentinel must reject it
    void *freed = malloc(240);
    KTEST_EXPECT(freed != nullptr);
    uintptr_t freed_addr = reinterpret_cast<uintptr_t>(freed);
    free(freed);
    heap_test_reset_corruption();
    heap_test_write_inner(freed_addr + 128, freed_addr, 128);
    free(reinterpret_cast<void *>(freed_addr + 128));
    KTEST_EXPECT_EQ(heap_test_corruption_events, 1);

    // The crafting lives in user data; the host blocks must still free cleanly.
    heap_test_reset_corruption();
    free(buf);
    KTEST_EXPECT_EQ(heap_test_corruption_events, 0);

    heap_debug_set_corruption_hook(nullptr);
}
#endif

extern "C" void heap_dump_stats();

KTEST(heap_stats_after_aligned_sequence)
{
    // Mixed aligned/plain/large traffic, all freed back.
    void *a = aligned_alloc(64, 100);    // bucket 256 base
    void *b = malloc(256);               // bucket 512
    void *keep = malloc(256);            // keeps b's page from returning to the PMM
    void *c = aligned_alloc(4096, 2000); // large (2 pages) base
    KTEST_EXPECT(a != nullptr);
    KTEST_EXPECT(b != nullptr);
    KTEST_EXPECT(keep != nullptr);
    KTEST_EXPECT(c != nullptr);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(a) & 63, static_cast<uintptr_t>(0));
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(c) & 4095, static_cast<uintptr_t>(0));

    kstring::memset(a, 0x11, 100);
    kstring::memset(b, 0x22, 256);
    kstring::memset(c, 0x33, 2000);

    free(a);
    free(b);
    free(c);

    // Walks the free lists and tracked-page table under the heap lock: must
    // not crash or deadlock on the post-sequence state.
    heap_dump_stats();

    // The heap still serves and recycles: b's slot is the 512-bucket head.
    void *d = malloc(256);
    KTEST_EXPECT(d != nullptr);
    KTEST_EXPECT_EQ(reinterpret_cast<uintptr_t>(d), reinterpret_cast<uintptr_t>(b));
    free(d);

    free(keep);
}
