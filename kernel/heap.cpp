#include "heap.h"

struct HeapBlock {
    size_t size;
    bool is_free;
    HeapBlock* next;
};

static HeapBlock* heap_start = nullptr;

void heap_init(void* start, size_t size) {
    heap_start = (HeapBlock*)start;
    heap_start->size = size - sizeof(HeapBlock);
    heap_start->is_free = true;
    heap_start->next = nullptr;
}

void* malloc(size_t size) {
    HeapBlock* current = heap_start;
    
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    while (current) {
        if (current->is_free && current->size >= size) {
            // Found a block
            // Check if we can split it
            if (current->size > size + sizeof(HeapBlock) + 8) {
                HeapBlock* new_block = (HeapBlock*)((uint8_t*)current + sizeof(HeapBlock) + size);
                new_block->size = current->size - size - sizeof(HeapBlock);
                new_block->is_free = true;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->is_free = false;
            return (void*)((uint8_t*)current + sizeof(HeapBlock));
        }
        current = current->next;
    }
    
    return nullptr; // Out of memory
}

void free(void* ptr) {
    if (!ptr) return;
    
    HeapBlock* block = (HeapBlock*)((uint8_t*)ptr - sizeof(HeapBlock));
    block->is_free = true;
    
    // Merge with next block if free
    if (block->next && block->next->is_free) {
        block->size += sizeof(HeapBlock) + block->next->size;
        block->next = block->next->next;
    }
    
    // Ideally we should also merge with previous block, but this is a simple singly-linked list implementation
}

void* operator new(size_t size) {
    return malloc(size);
}

void* operator new[](size_t size) {
    return malloc(size);
}

void operator delete(void* ptr) {
    free(ptr);
}

void operator delete[](void* ptr) {
    free(ptr);
}

void operator delete(void* ptr, size_t size) {
    (void)size;
    free(ptr);
}

void operator delete[](void* ptr, size_t size) {
    (void)size;
    free(ptr);
}
