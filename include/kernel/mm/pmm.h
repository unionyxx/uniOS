#pragma once
#include <stdint.h>
#include <stddef.h>

void pmm_init();

[[nodiscard]] void* pmm_alloc_frame();
[[nodiscard]] void* pmm_alloc_frames(size_t count);

void pmm_free_frame(void* frame);
void pmm_refcount_inc(void* frame);
void pmm_refcount_dec(void* frame);

[[nodiscard]] uint16_t pmm_get_refcount(void* frame);
[[nodiscard]] uint64_t pmm_get_free_memory();
[[nodiscard]] uint64_t pmm_get_total_memory();
