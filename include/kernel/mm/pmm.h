#pragma once
#include <stdint.h>
#include <stddef.h>

void pmm_init();
void* pmm_alloc_frame();
void* pmm_alloc_frames(size_t count);
void  pmm_free_frame(void* frame);
void  pmm_refcount_inc(void* frame);
void  pmm_refcount_dec(void* frame);
uint16_t pmm_get_refcount(void* frame);

uint64_t pmm_get_free_memory();
uint64_t pmm_get_total_memory();
