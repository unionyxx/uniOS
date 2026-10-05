#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * @brief A physical memory zone: a frame-range classification over the whole
 *        PMM bitmap (multiple boot USABLE regions can each contribute parts of
 *        both zones). Membership is the O(1) test
 *        `base_frame <= frame < base_frame + frame_count`.
 */
struct PmmZone
{
    uint64_t base_frame;  // first frame index (phys / 4096) of the zone
    uint64_t frame_count; // frames in [base_frame, base_frame + frame_count); 0 = empty zone
    const char *name;
};

/** @brief Frames below 4 GiB: the only memory 32-bit-only DMA devices can reach. */
extern PmmZone g_pmm_zone_dma32;
/** @brief Frames at or above 4 GiB. Empty on machines with no RAM above 4 GiB. */
extern PmmZone g_pmm_zone_normal;

/** @brief Initializes the Physical Memory Manager. */
void pmm_init();

/**
 * @brief Allocates a single physical frame (4096 bytes).
 * @return Physical address of the allocated frame, or nullptr on failure.
 * @note Prefers ZONE_NORMAL; falls back to ZONE_DMA32 only when NORMAL is
 *       exhausted (order-0 allocations may borrow from the DMA32 zone).
 * @note The returned frame is zeroed by default.
 */
[[nodiscard]] void *pmm_alloc_frame();

/**
 * @brief Allocates multiple contiguous physical frames.
 * @return Physical address of the first frame, or nullptr on failure.
 * @note Scans ZONE_NORMAL top-down. While NORMAL exists (RAM above 4 GiB) it
 *       never falls back to ZONE_DMA32 — contiguous DMA32 runs are the scarce
 *       resource 32-bit devices depend on — and failure is logged and returns
 *       null. When the machine has no RAM above 4 GiB, DMA32 is the only zone
 *       and is scanned instead. Order-0 single-frame allocation (pmm_alloc_frame)
 *       DOES fall back to DMA32 under NORMAL exhaustion: only the contiguous
 *       paths protect the zone; sustained order-0 pressure can still consume
 *       it (the full reservation policy arrives with per-zone locks/magazines).
 */
[[nodiscard]] void *pmm_alloc_frames(size_t count);

/**
 * @brief Allocates a single physical frame strictly from ZONE_DMA32.
 * @return Physical address (below 4 GiB) or nullptr when the zone is exhausted.
 * @note Never falls back to high memory.
 */
[[nodiscard]] void *pmm_alloc_frame_dma32();

/**
 * @brief Allocates contiguous physical frames strictly from ZONE_DMA32.
 * @return Physical address of the first frame (whole run below 4 GiB), or
 *         nullptr when the zone has no run of that size.
 * @note Never falls back to high memory.
 */
[[nodiscard]] void *pmm_alloc_frames_dma32(size_t count);

/**
 * @brief Marks a specific physical range as permanently used (SMP trampoline).
 * @param phys  Frame-aligned physical address of the first frame.
 * @param pages Number of frames to reserve.
 * @return true if every frame was free and is now reserved.
 */
[[nodiscard]] bool pmm_reserve_range(uint64_t phys, size_t pages);

/** @brief Frees a previously allocated physical frame. */
void pmm_free_frame(void *frame);

/** @brief Increments the reference count of a physical frame. */
void pmm_refcount_inc(const void *frame);

/** @brief Decrements the reference count of a physical frame. */
void pmm_refcount_dec(void *frame);

/** @brief Gets the current reference count of a physical frame. */
[[nodiscard]] uint16_t pmm_get_refcount(const void *frame);

/** @brief Returns total free physical memory in bytes. */
[[nodiscard]] uint64_t pmm_get_free_memory();

/** @brief Returns total physical memory managed by the PMM in bytes. */
[[nodiscard]] uint64_t pmm_get_total_memory();

/** @brief Checks if a physical address is within the managed RAM range. */
[[nodiscard]] bool pmm_is_managed(const void *frame);
