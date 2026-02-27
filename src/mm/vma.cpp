#include <kernel/mm/vma.h>
#include <kernel/mm/heap.h>
#include <kernel/debug.h>

VMA* vma_find(VMA* list, uint64_t addr) {
    VMA* current = list;
    while (current) {
        if (addr >= current->start && addr < current->end) {
            return current;
        }
        current = current->next;
    }
    return nullptr;
}

VMA* vma_add(VMA** list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type) {
    if (start >= end) return nullptr;
    
    // Simple implementation: Add to the beginning of the list, no merging for now.
    // In a production OS, we'd use an AVL tree or red-black tree and merge adjacent VMAs.
    VMA* new_vma = (VMA*)malloc(sizeof(VMA));
    if (!new_vma) return nullptr;
    
    new_vma->start = start;
    new_vma->end   = end;
    new_vma->flags = flags;
    new_vma->type  = type;
    new_vma->is_cow = false;
    new_vma->next  = *list_ptr;
    
    *list_ptr = new_vma;
    return new_vma;
}

void vma_remove(VMA** list_ptr, uint64_t start, uint64_t end) {
    // Basic implementation: Only support removing entire VMAs that match the range exactly for now.
    VMA** current = list_ptr;
    while (*current) {
        VMA* v = *current;
        if (v->start == start && v->end == end) {
            *current = v->next;
            free(v);
            return;
        }
        current = &((*current)->next);
    }
}

VMA* vma_clone(VMA* src_list) {
    VMA* new_list = nullptr;
    VMA** last_ptr = &new_list;
    
    VMA* current = src_list;
    while (current) {
        VMA* new_vma = (VMA*)malloc(sizeof(VMA));
        if (!new_vma) {
            vma_free_all(new_list);
            return nullptr;
        }
        
        *new_vma = *current; // Copy data
        new_vma->next = nullptr;
        
        *last_ptr = new_vma;
        last_ptr = &(new_vma->next);
        
        current = current->next;
    }
    
    return new_list;
}

void vma_free_all(VMA* list) {
    VMA* current = list;
    while (current) {
        VMA* next = current->next;
        free(current);
        current = next;
    }
}
