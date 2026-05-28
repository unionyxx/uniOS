#include <kernel/cpu.h>
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
    }
    return nullptr;
}

[[nodiscard]] VMA *vma_add(VMA **list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type)
{
    if (start >= end || !list_ptr)
        return nullptr;

    if (!*list_ptr) {
        VMA *sentinel = static_cast<VMA *>(malloc(sizeof(VMA)));
        if (!sentinel)
            return nullptr;
        sentinel->start = 0;
        sentinel->end = 0;
        sentinel->flags = 0;
        sentinel->type = VMAType::Generic;
        sentinel->is_cow = false;
        sentinel->next = nullptr;
        *list_ptr = sentinel;
    }

    VMA **link = list_ptr;

    while (*link && (*link)->start < start) {
        if (start < (*link)->end)
            return nullptr;
        link = &(*link)->next;
    }

    if (*link && end > (*link)->start)
        return nullptr;

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
    if (!list_ptr)
        return;

    for (VMA **link = list_ptr; *link; link = &(*link)->next) {
        if ((*link)->start == start && (*link)->end == end) {
            VMA *target = *link;
            *link = target->next;
            free(target);
            return;
        }
    }
}

[[nodiscard]] bool vma_unmap(VMA **list_ptr, uint64_t start, uint64_t end)
{
    if (!list_ptr || start >= end)
        return false;

    VMA **link = list_ptr;
    while (*link) {
        VMA *curr = *link;
        if (end <= curr->start || start >= curr->end) {
            // Case 1: No overlap
            link = &curr->next;
        } else if (start <= curr->start && end >= curr->end) {
            // Case 2: Complete overlap -> delete
            *link = curr->next;
            free(curr);
        } else if (start <= curr->start && end < curr->end) {
            // Case 3: Trim left
            curr->start = end;
            link = &curr->next;
        } else if (start > curr->start && end >= curr->end) {
            // Case 4: Trim right
            curr->end = start;
            link = &curr->next;
        } else {
            // Case 5: Split inside
            VMA *new_vma = static_cast<VMA *>(malloc(sizeof(VMA)));
            if (!new_vma) {
                // Out of memory during VMA split
                return false;
            }
            new_vma->start = end;
            new_vma->end = curr->end;
            new_vma->flags = curr->flags;
            new_vma->type = curr->type;
            new_vma->is_cow = curr->is_cow;
            new_vma->next = curr->next;

            curr->end = start;
            curr->next = new_vma;

            link = &new_vma->next;
        }
    }
    return true;
}

[[nodiscard]] VMA *vma_clone(const VMA *src_list)
{
    VMA *new_list = nullptr;
    VMA **link = &new_list;

    for (const VMA *current = src_list; current; current = current->next) {
        VMA *new_vma = static_cast<VMA *>(malloc(sizeof(VMA)));
        if (!new_vma) {
            vma_free_all(new_list);
            return nullptr;
        }

        *new_vma = *current;
        new_vma->next = nullptr;

        *link = new_vma;
        link = &new_vma->next;
    }

    return new_list;
}

#define STAC()                                                                                                         \
    do {                                                                                                               \
        if (g_cpu_features.has_smap)                                                                                   \
            asm volatile("stac" ::: "memory");                                                                         \
    } while (0)
#define CLAC()                                                                                                         \
    do {                                                                                                               \
        if (g_cpu_features.has_smap)                                                                                   \
            asm volatile("clac" ::: "memory");                                                                         \
    } while (0)

void vma_dump_list(VMA *list)
{
    if (!list)
        return;

    STAC();
    int count = 0;
    for (VMA *curr = list; curr && count < 100; curr = curr->next, ++count) {
        DEBUG_INFO("  VMA #%d: [0x%llx, 0x%llx) flags=0x%llx type=%d cow=%d", count, curr->start, curr->end,
                   curr->flags, static_cast<int>(curr->type), static_cast<int>(curr->is_cow));
    }
    CLAC();
}

void vma_free_all(VMA *list)
{
    while (list) {
        VMA *next = list->next;
        free(list);
        list = next;
    }
}