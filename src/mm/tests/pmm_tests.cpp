#include <kernel/ktest.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>

KTEST(pmm_alloc_free)
{
    void *frame = pmm_alloc_frame();
    KTEST_EXPECT(frame != nullptr);

    // Allocate another one to ensure they are distinct
    void *frame2 = pmm_alloc_frame();
    KTEST_EXPECT(frame2 != nullptr);
    KTEST_EXPECT(frame != frame2);

    pmm_free_frame(frame);
    pmm_free_frame(frame2);

    // Re-allocating should give us one of those back potentially
    void *frame3 = pmm_alloc_frame();
    KTEST_EXPECT(frame3 != nullptr);
    pmm_free_frame(frame3);
}

KTEST(pmm_alloc_is_zeroed)
{
    void *frame = pmm_alloc_frame();
    KTEST_EXPECT(frame != nullptr);

    // Check if it's actually zeroed (pmm_alloc_frame does this by default)
    uint8_t *ptr = (uint8_t *)vmm_phys_to_virt((uint64_t)frame);
    for (int i = 0; i < 4096; i++) {
        KTEST_EXPECT_EQ(ptr[i], 0);
    }

    pmm_free_frame(frame);
}

KTEST(pmm_null_frame_is_invalid)
{
    uint64_t free_before = pmm_get_free_memory();
    pmm_free_frame(nullptr);

    KTEST_EXPECT(!pmm_is_managed(nullptr));
    KTEST_EXPECT_EQ(pmm_get_refcount(nullptr), 0);
    KTEST_EXPECT_EQ(pmm_get_free_memory(), free_before);
}
