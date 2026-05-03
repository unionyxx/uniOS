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
