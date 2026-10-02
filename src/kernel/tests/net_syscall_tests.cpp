#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/net/net.h>
#include <kernel/process.h>
#include <libk/kstring.h>
#include <uapi/syscalls_ext.h>

namespace {

constexpr uint64_t TEST_VADDR = 0x10000000ULL;

} // namespace

KTEST(net_syscall_status_rejects_kernel_pointer)
{
    NetStatus local = {};
    int64_t res = sys_net_status(&local);
    KTEST_EXPECT_EQ(res, -14); // -EFAULT: kernel-space pointer rejected
}

KTEST(net_syscall_status_fills_struct)
{
    Process *current = process_get_current();
    KTEST_EXPECT(current != nullptr);

    uint64_t *orig_page_table = current->page_table;
    VMA *orig_vma_list = current->vma_list;
    uint32_t orig_vma_count = current->vma_count;

    if (!current->page_table)
        current->page_table = vmm_get_kernel_pml4();

    // sys_net_status validates the out pointer as a user address: map a
    // real user page and install its VMA (futex-test pattern).
    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(current->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    current->vma_list = vma;
    current->vma_count = 1;

    NetStatus *out = reinterpret_cast<NetStatus *>(TEST_VADDR);
    kstring::zero_memory(out, sizeof(NetStatus));
    int64_t res = sys_net_status(out);
    KTEST_EXPECT_EQ(res, 0);
    // ktest context: deferred services (net_init) have not run yet.
    KTEST_EXPECT_EQ(out->nic, static_cast<uint8_t>(NET_NIC_NONE));
    KTEST_EXPECT_EQ(out->link_up, 0);
    KTEST_EXPECT_EQ(out->configured, 0);
    KTEST_EXPECT_EQ(out->ip, 0u);
    KTEST_EXPECT_EQ(out->gateway, 0u);

    vmm_unmap_page_in(current->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);

    current->page_table = orig_page_table;
    current->vma_list = orig_vma_list;
    current->vma_count = orig_vma_count;
}
