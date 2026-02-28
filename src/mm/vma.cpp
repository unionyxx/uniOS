#include <kernel/mm/vma.h>
#include <kernel/mm/heap.h>
#include <kernel/debug.h>

[[nodiscard]] VMA* vma_find(VMA* list, uint64_t addr) {
    for (VMA* current = list; current; current = current->next) {
        if (addr >= current->start && addr < current->end) {
            return current;
        }
    }
    return nullptr;
}

[[nodiscard]] VMA* vma_add(VMA** list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type) {
    if (start >= end) return nullptr;
    
    VMA* new_vma = static_cast<VMA*>(malloc(sizeof(VMA)));
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

[[nodiscard]] VMA* vma_clone(VMA* src_list) {
    VMA* new_list = nullptr;
    VMA** last_ptr = &new_list;
    
    for (VMA* current = src_list; current; current = current->next) {
        VMA* new_vma = static_cast<VMA*>(malloc(sizeof(VMA)));
        if (!new_vma) {
            vma_free_all(new_list);
            return nullptr;
        }
        
        *new_vma = *current;
        new_vma->next = nullptr;
        
        *last_ptr = new_vma;
        last_ptr = &(new_vma->next);
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
