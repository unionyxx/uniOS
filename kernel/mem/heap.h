#pragma once
#include <stddef.h>
#include <stdint.h>

void heap_init(void* start, size_t size);
void* malloc(size_t size);
void free(void* ptr);

// C++ operators
void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* ptr);
void operator delete[](void* ptr);
void operator delete(void* ptr, size_t size);
void operator delete[](void* ptr, size_t size);
