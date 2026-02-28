#pragma once
#include <stdint.h>
#include <stddef.h>

enum class VMAType {
    Generic,
    Text,
    Data,
    Stack,
    Heap,
    Mmio,
    Anonymous
};

struct VMA {
    uint64_t start;
    uint64_t end;
    uint64_t flags;
    VMAType  type;
    bool     is_cow;
    VMA*     next;
};

[[nodiscard]] VMA* vma_find(VMA* list, uint64_t addr);
[[nodiscard]] VMA* vma_add(VMA** list_ptr, uint64_t start, uint64_t end, uint64_t flags, VMAType type);

void vma_remove(VMA** list_ptr, uint64_t start, uint64_t end);
[[nodiscard]] VMA* vma_clone(VMA* src_list);
void vma_free_all(VMA* list);
