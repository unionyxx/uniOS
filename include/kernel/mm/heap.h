#pragma once
#include <stddef.h>
#include <stdint.h>

void heap_init(void* start, size_t size);

[[nodiscard]] void* malloc(size_t size);
void free(void* ptr);

[[nodiscard]] void* aligned_alloc(size_t alignment, size_t size);
void aligned_free(void* ptr);

[[nodiscard]] void* operator new(size_t size);
[[nodiscard]] void* operator new[](size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t size) noexcept;
void operator delete[](void* ptr, size_t size) noexcept;
