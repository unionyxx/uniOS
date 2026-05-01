#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vma.h>
#include <kernel/process.h>

[[nodiscard]] VMA *vma_find(VMA *list, uint64_t addr)
{
    for (VMA *current = list; current; current = current->next) {
        if (addr >= current->start && addr < current->end) {
            return current;
        }
        // Safety: catch potential cycles
        if (current == current->next) {
            DEBUG_ERROR("vma: cycle detected in list!");
            break;
        }
    }

    // Debug: Log the list if we missed a likely range
    // if (addr >= 0x100000000ULL && addr < 0x300000000ULL) {
    //     DEBUG_ERROR("vma_find: missed heap addr 0x%llx. List dump:", addr);
    //     vma_dump_list(list);
    // }
    return nullptr;
}

[[nodiscard]] VMA *vma_add(VMA **list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type)
{
    if (start >= end)
        return nullptr;

    VMA **link = list_ptr;
    while (*link && (*link)->start < start) {
        if (start < (*link)->end && end > (*link)->start) {
            DEBUG_ERROR("vma_add: detected overlap! NEW:[0x%llx, 0x%llx) EXIST:[0x%llx, 0x%llx)", start, end,
                        (*link)->start, (*link)->end);
            return nullptr;
        }
        link = &(*link)->next;
    }

    if (*link && start < (*link)->end && end > (*link)->start) {
        DEBUG_ERROR("vma_add: detected overlap! NEW:[0x%llx, 0x%llx) EXIST:[0x%llx, 0x%llx)", start, end,
                    (*link)->start, (*link)->end);
        return nullptr;
    }

    VMA *new_vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    if (!new_vma)
        return nullptr;

    new_vma->start = start;
    new_vma->end = end;
    new_vma->flags = flags;
    new_vma->type = type;
    new_vma->is_cow = false;
    new_vma->next = *link;

    *link = new_vma;
    return new_vma;
}

void vma_remove(VMA **list_ptr, uint64_t start, uint64_t end)
{
    VMA **current = list_ptr;
    while (*current) {
        VMA *v = *current;
        if (v->start == start && v->end == end) {
            *current = v->next;
            free(v);
            return;
        }
        current = &((*current)->next);
    }
}

[[nodiscard]] VMA *vma_clone(VMA *src_list)
{
    VMA *new_list = nullptr;
    VMA **last_ptr = &new_list;

    for (VMA *current = src_list; current; current = current->next) {
        VMA *new_vma = static_cast<VMA *>(malloc(sizeof(VMA)));
        if (!new_vma) {
            vma_free_all(new_list);
            return nullptr;
        }

        *new_vma = *current;
        new_vma->next = nullptr;

        *last_ptr = new_vma;
        last_ptr = &(new_vma->next);

        // DEBUG_INFO("vma_clone: cloned [0x%llx, 0x%llx) type=%d", current->start, current->end, (int)current->type);

        if (current == current->next) {
            DEBUG_ERROR("vma_clone: cycle detected!");
            break;
        }
    }

    return new_list;
}

#include <kernel/cpu.h>
#define STAC()                                                                                                         \
    if (g_cpu_features.has_smap)                                                                                       \
    asm volatile("stac" ::: "memory")
#define CLAC()                                                                                                         \
    if (g_cpu_features.has_smap)                                                                                       \
    asm volatile("clac" ::: "memory")

void vma_dump_list(VMA *list)
{
    if (!list) {
        DEBUG_INFO("  VMA list is NULL");
        return;
    }

    STAC();
    int count = 0;
    VMA *curr = list;
    while (curr) {
        DEBUG_INFO("  VMA #%d: [0x%llx, 0x%llx) flags=0x%llx type=%d cow=%d", count++, curr->start, curr->end,
                   curr->flags, (int)curr->type, (int)curr->is_cow);

        curr = curr->next;
        if (count > 100) {
            DEBUG_ERROR("  VMA dump aborted: possible cycle or list too long");
            break;
        }
    }
    CLAC();
}

void vma_free_all(VMA *list)
{
    VMA *current = list;
    while (current) {
        VMA *next = current->next;
        free(current);
        current = next;
    }
}
