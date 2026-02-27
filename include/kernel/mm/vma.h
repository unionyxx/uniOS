#pragma once
#include <stdint.h>
#include <stddef.h>

enum VMAType {
    VMA_GENERIC,
    VMA_TEXT,     // Code (Read-Execute)
    VMA_DATA,     // Data (Read-Write)
    VMA_STACK,    // Stack (Read-Write, grows down)
    VMA_HEAP,     // Heap (Read-Write, grows up)
    VMA_MMIO,     // Memory Mapped I/O
    VMA_ANONYMOUS // Shared memory or other anonymous mappings
};

struct VMA {
    uint64_t start;
    uint64_t end;     // Exclusive
    uint64_t flags;   // Page table flags (PTE_PRESENT, PTE_WRITABLE, etc.)
    VMAType  type;
    bool     is_cow;  // True if this VMA is subject to Copy-on-Write
    
    VMA* next;
};

/**
 * @brief Find a VMA that contains the given virtual address.
 * @param list  Head of the VMA list
 * @param addr  Virtual address to search for
 * @return Pointer to VMA or nullptr if not found
 */
VMA* vma_find(VMA* list, uint64_t addr);

/**
 * @brief Add a new VMA to the list, merging with neighbors if possible.
 * @param list_ptr Pointer to the head pointer of the VMA list
 * @param start    Start virtual address
 * @param end      End virtual address (exclusive)
 * @param flags    Page table flags
 * @param type     Type of memory region
 * @return Pointer to the new or merged VMA, or nullptr on failure
 */
VMA* vma_add(VMA** list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type);

/**
 * @brief Remove a VMA or part of a VMA from the list.
 * @param list_ptr Pointer to the head pointer of the VMA list
 * @param start    Start virtual address
 * @param end      End virtual address (exclusive)
 */
void vma_remove(VMA** list_ptr, uint64_t start, uint64_t end);

/**
 * @brief Clone a VMA list (used during fork).
 * @param src_list Head of the source VMA list
 * @return Head of the new cloned VMA list
 */
VMA* vma_clone(VMA* src_list);

/**
 * @brief Free all VMAs in a list.
 * @param list Head of the VMA list
 */
void vma_free_all(VMA* list);
