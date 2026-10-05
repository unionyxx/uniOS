#include <kernel/ktest.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <uapi/syscalls.h>

// The SYS_MMAP body, extracted from the dispatcher (syscall.cpp) so ktests
// drive the real install/rollback path instead of a re-implementation.
extern "C" uint64_t sys_mmap_impl(uint64_t length, uint64_t prot, uint64_t map_flags, int fd, uint64_t offset);

namespace {

constexpr uint64_t k_test_pages = 8;
constexpr uint64_t k_test_len = k_test_pages * 4096;
constexpr uint64_t k_fail_at = 3; // the Nth frame allocation in the map path fails

} // namespace

// Plain map/unmap round trip through the extracted mmap body: the mapping
// lands, translates, and tears down cleanly - VMA metadata gone, every PTE
// cleared, PMM free count back to the post-prime baseline.
KTEST(mmap_unmap_round_trip)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    // Prime: one discarded cycle so the first-touch page-table levels and
    // heap buckets for this range exist before the baseline is taken.
    uint64_t prime = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);
    KTEST_EXPECT(prime != static_cast<uint64_t>(-1));
    KTEST_EXPECT(munmap_process_range(leader, prime, k_test_len));

    const uint64_t baseline = pmm_get_free_memory();

    uint64_t base = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);
    KTEST_EXPECT(base != static_cast<uint64_t>(-1));
    KTEST_EXPECT_EQ(base, prime); // same free gap after the teardown
    KTEST_EXPECT(vma_find(leader->vmalist->head, base) != nullptr);
    for (uint64_t i = 0; i < k_test_pages; i++)
        KTEST_EXPECT(vmm_virt_to_phys_in(leader->page_table, base + i * 4096) != 0);

    KTEST_EXPECT(munmap_process_range(leader, base, k_test_len));
    KTEST_EXPECT(vma_find(leader->vmalist->head, base) == nullptr);
    for (uint64_t i = 0; i < k_test_pages; i++)
        KTEST_EXPECT_EQ(vmm_virt_to_phys_in(leader->page_table, base + i * 4096), 0u);
    KTEST_EXPECT_EQ(pmm_get_free_memory(), baseline);

    leader->page_table = orig_page_table;
}

#ifdef DEBUG
// Debug fault-injection + observability hooks (syscall.cpp).
extern uint64_t g_mmap_debug_notify_frames;
extern uint64_t g_mmap_debug_rollback_seq;
extern uint64_t g_munmap_debug_oom;
extern "C" void sys_mmap_debug_fail_after(uint64_t n);

// Fault-injected mmap OOM: the 3rd frame allocation fails with 2 pages
// already installed. The rollback must return the system to the exact
// pre-call state without ever shooting down under the vma lock: VMA list
// restored byte-identically, PMM free count back to baseline, the futex
// notify reached on the freed frames, and all four ordered phases (clear
// under the lock, invalidate, notify, free) completed.
KTEST(mmap_oom_rollback_releases_frames_and_notifies)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    uint64_t prime = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);
    KTEST_EXPECT(prime != static_cast<uint64_t>(-1));
    KTEST_EXPECT(munmap_process_range(leader, prime, k_test_len));

    VMA *post_prime_head = leader->vmalist->head;
    const uint64_t baseline = pmm_get_free_memory();
    g_mmap_debug_notify_frames = 0;
    g_mmap_debug_rollback_seq = 0;
    sys_mmap_debug_fail_after(k_fail_at);

    const uint64_t ret = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);

    sys_mmap_debug_fail_after(0);
    KTEST_EXPECT_EQ(ret, static_cast<uint64_t>(-1));
    // The rollback removed exactly the VMA it added; nothing else moved.
    KTEST_EXPECT_EQ(leader->vmalist->head, post_prime_head);
    KTEST_EXPECT(vma_find(leader->vmalist->head, prime) == nullptr);
    // The k_fail_at - 1 pages that were installed are gone from the tables.
    for (uint64_t i = 0; i < k_fail_at - 1; i++)
        KTEST_EXPECT_EQ(vmm_virt_to_phys_in(leader->page_table, prime + i * 4096), 0u);
    // Every frame (data + journal chunk) returned to the PMM.
    KTEST_EXPECT_EQ(pmm_get_free_memory(), baseline);
    // The futex notify ran on the freed pages...
    KTEST_EXPECT_EQ(g_mmap_debug_notify_frames, k_fail_at - 1);
    // ...and the ordered phases all completed (4 = frames released).
    KTEST_EXPECT_EQ(g_mmap_debug_rollback_seq, 4u);

    // The rolled-back range is immediately reusable: map it again and tear
    // it down - a leaked or mis-refcounted frame would fail the count.
    uint64_t again = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);
    KTEST_EXPECT(again != static_cast<uint64_t>(-1));
    KTEST_EXPECT_EQ(again, prime);
    KTEST_EXPECT(munmap_process_range(leader, again, k_test_len));
    KTEST_EXPECT_EQ(pmm_get_free_memory(), baseline);

    leader->page_table = orig_page_table;
}

// Fault-injected munmap heap-OOM: the journal allocation is forced to fail
// so the zero-allocation fallback runs. Same invariants: frames freed,
// futex waiters notified, ordered phases completed, metadata and PTEs gone.
KTEST(munmap_oom_fallback_notifies_and_frees)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    uint64_t prime = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);
    KTEST_EXPECT(prime != static_cast<uint64_t>(-1));
    KTEST_EXPECT(munmap_process_range(leader, prime, k_test_len));

    const uint64_t baseline = pmm_get_free_memory();
    g_mmap_debug_notify_frames = 0;
    g_mmap_debug_rollback_seq = 0;

    uint64_t base = sys_mmap_impl(k_test_len, PROT_READ | PROT_WRITE, 0, -1, 0);
    KTEST_EXPECT(base != static_cast<uint64_t>(-1));
    KTEST_EXPECT_EQ(base, prime);

    g_munmap_debug_oom = 1;
    const bool unmapped = munmap_process_range(leader, base, k_test_len);
    g_munmap_debug_oom = 0;

    KTEST_EXPECT(unmapped);
    KTEST_EXPECT(vma_find(leader->vmalist->head, base) == nullptr);
    for (uint64_t i = 0; i < k_test_pages; i++)
        KTEST_EXPECT_EQ(vmm_virt_to_phys_in(leader->page_table, base + i * 4096), 0u);
    KTEST_EXPECT_EQ(pmm_get_free_memory(), baseline);
    KTEST_EXPECT_EQ(g_mmap_debug_notify_frames, k_test_pages);
    KTEST_EXPECT_EQ(g_mmap_debug_rollback_seq, 4u);

    leader->page_table = orig_page_table;
}
#endif // DEBUG
