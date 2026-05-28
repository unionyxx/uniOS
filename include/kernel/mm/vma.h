#pragma once
#include <stddef.h>
#include <stdint.h>

enum class VMAType
{
    Generic,
    Text,
    Data,
    Stack,
    Heap,
    Mmio,
    Anonymous,
    Shared
};

struct VMA
{
    uint64_t start;
    uint64_t end;
    uint64_t flags;
    VMAType type;
    bool is_cow;
    VMA *next;
};

[[nodiscard]] VMA *vma_find(VMA *list, uint64_t addr);
[[nodiscard]] VMA *vma_add(VMA **list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type);

void vma_remove(VMA **list_ptr, uint64_t start, uint64_t end);
[[nodiscard]] bool vma_unmap(VMA **list_ptr, uint64_t start, uint64_t end);
[[nodiscard]] VMA *vma_clone(const VMA *src_list);
void vma_free_all(VMA *list);
void vma_dump_list(VMA *list);
