#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/net/net.h>
#include <kernel/net/tcp.h>
#include <kernel/process.h>
#include <kernel/syscall.h>
#include <kernel/user_ptr.h>
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
    KSTAC();
    kstring::zero_memory(out, sizeof(NetStatus));
    KCLAC();
    int64_t res = sys_net_status(out);
    KTEST_EXPECT_EQ(res, 0);
    NetStatus status;
    KSTAC();
    status = *out;
    KCLAC();
    // ktest context: deferred services (net_init) have not run yet.
    KTEST_EXPECT_EQ(status.nic, static_cast<uint8_t>(NET_NIC_NONE));
    KTEST_EXPECT_EQ(status.link_up, 0);
    KTEST_EXPECT_EQ(status.configured, 0);
    KTEST_EXPECT_EQ(status.ip, 0u);
    KTEST_EXPECT_EQ(status.gateway, 0u);

    vmm_unmap_page_in(current->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);

    current->page_table = orig_page_table;
    current->vma_list = orig_vma_list;
    current->vma_count = orig_vma_count;
}

KTEST(net_syscall_socket_state_rejects_bad_handles)
{
    // High bits set but neither a UDP nor a TCP kind with a valid index.
    KTEST_EXPECT_EQ(sys_socket_state(0xFFFF), static_cast<int64_t>(-9)); // -EBADF
    // TCP kind (2) with an index beyond TCP_MAX_SOCKETS (32).
    KTEST_EXPECT_EQ(sys_socket_state((2u << 12) | 32u), static_cast<int64_t>(-9));
    // UDP kind (1) with an index beyond UDP_MAX_SOCKETS.
    KTEST_EXPECT_EQ(sys_socket_state((1u << 12) | 16u), static_cast<int64_t>(-9));
}

KTEST(net_syscall_socket_state_reports_closed_tcp)
{
    const int sock = tcp_socket();
    KTEST_EXPECT(sock >= 0);
    if (sock < 0)
        return;

    // SOCKET_KIND_TCP is 2; a fresh slot sits in TCP_CLOSED.
    const uint64_t handle = (2u << 12) | static_cast<uint64_t>(sock);
    KTEST_EXPECT_EQ(sys_socket_state(handle), static_cast<int64_t>(NET_TCP_CLOSED));

    tcp_close(sock);
    // After close the slot is reset; the handle decodes to an unused slot.
    KTEST_EXPECT_EQ(sys_socket_state(handle), static_cast<int64_t>(-9));
}

KTEST(net_syscall_renew_gated_before_init)
{
    // ktest context: deferred services (net_init) have not run yet.
    KTEST_EXPECT_EQ(sys_net_renew(), static_cast<int64_t>(-11)); // -EAGAIN
}

KTEST(net_renew_guard_rejects_double_entry)
{
    KTEST_EXPECT(net_renew_begin());
    KTEST_EXPECT(!net_renew_begin()); // second entry while in flight
    net_renew_end();
    KTEST_EXPECT(net_renew_begin()); // guard released, entry works again
    net_renew_end();
}

KTEST(net_renew_guard_visible_to_dhcp_tick)
{
    // dhcp_tick() (called from net_poll()) must be able to see the renew
    // guard so it does not drive its own T1 renewal while a manual renew
    // exchange is mid-flight. The gate itself is only live with a bound
    // lease, which ktest context never has; this pins the observable half.
    KTEST_EXPECT(!net_renew_in_progress());
    KTEST_EXPECT(net_renew_begin());
    KTEST_EXPECT(net_renew_in_progress());
    net_renew_end();
    KTEST_EXPECT(!net_renew_in_progress());
}
