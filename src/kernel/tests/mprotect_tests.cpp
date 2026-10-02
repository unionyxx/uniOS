#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/user_ptr.h>
#include <uapi/syscalls.h>

extern "C" int64_t sys_mprotect(void *addr, size_t len, int prot);

namespace {

constexpr uint64_t TEST_VADDR = 0x10000000ULL;
constexpr size_t TEST_PAGES = 4;
constexpr uint64_t TEST_END = TEST_VADDR + TEST_PAGES * 4096;

} // namespace

// Sub-range mprotect semantics. pthread_create maps one RW region per
// thread and mprotects its first page to PROT_NONE as a guard; the guard
// call must not touch the protection of the remaining pages. The old
// implementation wrote the new flags to every overlapping VMA, so the
// guard call stripped PTE_WRITABLE from the whole mapping's metadata and
// every write-validated syscall into the thread stack failed.
KTEST(mprotect_subrange_guard_page)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *frames[TEST_PAGES] = {};
    for (size_t i = 0; i < TEST_PAGES; i++) {
        frames[i] = pmm_alloc_frame();
        KTEST_EXPECT(frames[i] != nullptr);
        Result<void> map =
            vmm_replace_page_in(leader->page_table, TEST_VADDR + i * 4096, reinterpret_cast<uint64_t>(frames[i]),
                                PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        KTEST_EXPECT(map.ok());
    }

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_END;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->is_cow = false;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    // The pthread guard geometry: protect the first page of the mapping.
    KTEST_EXPECT_EQ(sys_mprotect(reinterpret_cast<void *>(TEST_VADDR), 4096, PROT_NONE), 0);

    // The mapping must split at the range end: a one-page guard VMA
    // without PTE_WRITABLE, and one intact VMA for the rest.
    VMA *guard = vma_find(leader->vmalist->head, TEST_VADDR);
    VMA *rest = vma_find(leader->vmalist->head, TEST_VADDR + 4096);
    KTEST_EXPECT(guard != nullptr);
    KTEST_EXPECT(rest != nullptr);
    KTEST_EXPECT_EQ(guard->start, TEST_VADDR);
    KTEST_EXPECT_EQ(guard->end, TEST_VADDR + 4096);
    KTEST_EXPECT_EQ(rest->start, TEST_VADDR + 4096);
    KTEST_EXPECT_EQ(rest->end, TEST_END);
    KTEST_EXPECT((guard->flags & PTE_WRITABLE) == 0);
    KTEST_EXPECT((rest->flags & PTE_WRITABLE) != 0);
    // The untouched remainder is a single VMA, not fragmented further.
    KTEST_EXPECT(vma_find(leader->vmalist->head, TEST_END - 4096) == rest);

    // The user-visible consequence: a write into the thread's own stack
    // (the untouched part) still validates; the guard page does not.
    KTEST_EXPECT(validate_user_ptr(reinterpret_cast<void *>(TEST_VADDR + 4096), (TEST_PAGES - 1) * 4096, true));
    KTEST_EXPECT(!validate_user_ptr(reinterpret_cast<void *>(TEST_VADDR), 4096, true));

    // Teardown: unmap the whole surgery range (nodes, PTEs, frames —
    // including the guard's), then restore the leader's list.
    KTEST_EXPECT(munmap_process_range(leader, TEST_VADDR, TEST_PAGES * 4096));

    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

// A sub-range strictly inside a mapping exercises both range boundaries:
// the split runs at start and end, the flag update covers exactly the
// covered nodes, and a later sub-range unmap cuts the mprotect-created
// nodes without disturbing the neighbors.
KTEST(mprotect_subrange_split_then_unmap)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *frames[TEST_PAGES] = {};
    for (size_t i = 0; i < TEST_PAGES; i++) {
        frames[i] = pmm_alloc_frame();
        KTEST_EXPECT(frames[i] != nullptr);
        Result<void> map =
            vmm_replace_page_in(leader->page_table, TEST_VADDR + i * 4096, reinterpret_cast<uint64_t>(frames[i]),
                                PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        KTEST_EXPECT(map.ok());
    }

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_END;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->is_cow = false;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    // Protect the two middle pages: [base+1p, base+3p).
    const uint64_t mid_lo = TEST_VADDR + 4096;
    const uint64_t mid_hi = TEST_VADDR + 3 * 4096;
    KTEST_EXPECT_EQ(sys_mprotect(reinterpret_cast<void *>(mid_lo), 2 * 4096, PROT_NONE), 0);

    // Three VMAs: RW head, non-writable middle, RW tail.
    VMA *head = vma_find(leader->vmalist->head, TEST_VADDR);
    VMA *mid = vma_find(leader->vmalist->head, mid_lo);
    VMA *tail = vma_find(leader->vmalist->head, mid_hi);
    KTEST_EXPECT(head != nullptr);
    KTEST_EXPECT(mid != nullptr);
    KTEST_EXPECT(tail != nullptr);
    KTEST_EXPECT(head != mid && mid != tail && head != tail);
    KTEST_EXPECT_EQ(head->start, TEST_VADDR);
    KTEST_EXPECT_EQ(head->end, mid_lo);
    KTEST_EXPECT_EQ(mid->start, mid_lo);
    KTEST_EXPECT_EQ(mid->end, mid_hi);
    KTEST_EXPECT_EQ(tail->start, mid_hi);
    KTEST_EXPECT_EQ(tail->end, TEST_END);
    KTEST_EXPECT((head->flags & PTE_WRITABLE) != 0);
    KTEST_EXPECT((mid->flags & PTE_WRITABLE) == 0);
    KTEST_EXPECT((tail->flags & PTE_WRITABLE) != 0);

    // Split-then-unmap: remove the first of the two protected pages. The
    // unmap cuts strictly inside the mprotect-created middle VMA; the
    // remaining protected page and both RW neighbors must survive.
    KTEST_EXPECT(munmap_process_range(leader, mid_lo, 4096));
    KTEST_EXPECT(vma_find(leader->vmalist->head, mid_lo) == nullptr);
    VMA *head2 = vma_find(leader->vmalist->head, TEST_VADDR);
    VMA *mid2 = vma_find(leader->vmalist->head, TEST_VADDR + 2 * 4096);
    VMA *tail2 = vma_find(leader->vmalist->head, mid_hi);
    KTEST_EXPECT(head2 != nullptr);
    KTEST_EXPECT(mid2 != nullptr);
    KTEST_EXPECT(tail2 != nullptr);
    KTEST_EXPECT_EQ(head2->start, TEST_VADDR);
    KTEST_EXPECT_EQ(head2->end, mid_lo);
    KTEST_EXPECT_EQ(mid2->start, TEST_VADDR + 2 * 4096);
    KTEST_EXPECT_EQ(mid2->end, mid_hi);
    KTEST_EXPECT_EQ(tail2->start, mid_hi);
    KTEST_EXPECT_EQ(tail2->end, TEST_END);
    KTEST_EXPECT((head2->flags & PTE_WRITABLE) != 0);
    KTEST_EXPECT((mid2->flags & PTE_WRITABLE) == 0);
    KTEST_EXPECT((tail2->flags & PTE_WRITABLE) != 0);
    KTEST_EXPECT(validate_user_ptr(reinterpret_cast<void *>(mid_hi), 4096, true));

    // Teardown: unmap the rest of the surgery range.
    KTEST_EXPECT(munmap_process_range(leader, TEST_VADDR, TEST_PAGES * 4096));

    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}
