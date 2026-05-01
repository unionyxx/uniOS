#pragma once
#include <stddef.h>
#include <stdint.h>

/** @brief Initializes the Physical Memory Manager. */
void pmm_init();

/**
 * @brief Allocates a single physical frame (4096 bytes).
 * @return Physical address of the allocated frame, or nullptr on failure.
 * @note The returned frame is zeroed by default.
 */
[[nodiscard]] void *pmm_alloc_frame();

/**
 * @brief Allocates multiple contiguous physical frames.
 * @param count Number of frames to allocate.
 * @return Physical address of the first frame, or nullptr on failure.
 */
[[nodiscard]] void *pmm_alloc_frames(size_t count);

/** @brief Frees a previously allocated physical frame. */
void pmm_free_frame(void *frame);

/** @brief Increments the reference count of a physical frame. */
void pmm_refcount_inc(void *frame);

/** @brief Decrements the reference count of a physical frame. */
void pmm_refcount_dec(void *frame);

/** @brief Gets the current reference count of a physical frame. */
[[nodiscard]] uint16_t pmm_get_refcount(void *frame);

/** @brief Returns total free physical memory in bytes. */
[[nodiscard]] uint64_t pmm_get_free_memory();

/** @brief Returns total physical memory managed by the PMM in bytes. */
[[nodiscard]] uint64_t pmm_get_total_memory();

/** @brief Checks if a physical address is within the managed RAM range. */
[[nodiscard]] bool pmm_is_managed(void *frame);
