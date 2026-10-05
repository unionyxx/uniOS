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
    uint8_t *ptr = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(frame)));
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

KTEST(pmm_dma32_allocations_below_4g)
{
    constexpr uint64_t k_dma32_limit = 0x100000000ULL; // 4 GiB

    for (int i = 0; i < 4; i++) {
        void *frame = pmm_alloc_frame_dma32();
        KTEST_EXPECT(frame != nullptr);
        KTEST_EXPECT(reinterpret_cast<uint64_t>(frame) < k_dma32_limit);
        pmm_free_frame(frame);
    }

    void *run = pmm_alloc_frames_dma32(2);
    KTEST_EXPECT(run != nullptr);
    KTEST_EXPECT(reinterpret_cast<uint64_t>(run) + 2 * 4096 <= k_dma32_limit);
    pmm_free_frame(run);
    pmm_free_frame(reinterpret_cast<void *>(reinterpret_cast<uint64_t>(run) + 4096));
}

#ifdef DEBUG
// Zone-bound overrides are debug-only test hooks: shrink a zone descriptor to
// fake exhaustion without draining gigabytes of real memory.
KTEST(pmm_dma32_exhaustion_returns_null)
{
    const uint64_t saved_base = g_pmm_zone_dma32.base_frame;
    const uint64_t saved_count = g_pmm_zone_dma32.frame_count;

    void *f = pmm_alloc_frame_dma32();
    KTEST_EXPECT(f != nullptr);
    const uint64_t frame = reinterpret_cast<uint64_t>(f) / 4096;

    // ZONE_DMA32 = exactly {f}, and f is allocated: the zone is exhausted.
    g_pmm_zone_dma32.base_frame = frame;
    g_pmm_zone_dma32.frame_count = 1;

    const bool single_null = pmm_alloc_frame_dma32() == nullptr;
    const bool run1_null = pmm_alloc_frames_dma32(1) == nullptr;
    const bool run2_null = pmm_alloc_frames_dma32(2) == nullptr;

    // Freeing the only zone frame must make the zone allocatable again - the
    // exhaustion is null-with-no-fallback, not a wedged allocator.
    pmm_free_frame(f);
    void *again = pmm_alloc_frame_dma32();
    const bool recovered = (again != nullptr) && (reinterpret_cast<uint64_t>(again) / 4096 == frame) &&
                           (reinterpret_cast<uint64_t>(again) < 0x100000000ULL);
    pmm_free_frame(again);

    g_pmm_zone_dma32.base_frame = saved_base;
    g_pmm_zone_dma32.frame_count = saved_count;

    KTEST_EXPECT(single_null);
    KTEST_EXPECT(run1_null);
    KTEST_EXPECT(run2_null);
    KTEST_EXPECT(recovered);
}

KTEST(pmm_alloc_frame_falls_back_to_dma32)
{
    const uint64_t saved_base = g_pmm_zone_normal.base_frame;
    const uint64_t saved_count = g_pmm_zone_normal.frame_count;

    void *probe = pmm_alloc_frame();
    KTEST_EXPECT(probe != nullptr);

    // Fake "NORMAL exists but is exhausted": point the zone at one frame that
    // is already allocated.
    g_pmm_zone_normal.base_frame = reinterpret_cast<uint64_t>(probe) / 4096;
    g_pmm_zone_normal.frame_count = 1;

    // Order-0: must fall back to ZONE_DMA32 (below 4 GiB), not fail.
    void *f = pmm_alloc_frame();
    const bool fallback_low = (f != nullptr) && (reinterpret_cast<uint64_t>(f) < 0x100000000ULL);

    // Contiguous: may NOT fall back to DMA32 while NORMAL exists.
    void *run = pmm_alloc_frames(2);
    const bool contiguous_refused = (run == nullptr);

    pmm_free_frame(f);
    pmm_free_frame(probe);
    g_pmm_zone_normal.base_frame = saved_base;
    g_pmm_zone_normal.frame_count = saved_count;

    KTEST_EXPECT(fallback_low);
    KTEST_EXPECT(contiguous_refused);
}
#endif // DEBUG
