#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <kernel/syscall.h>
#include <uapi/syscalls.h>
#include <uapi/syscalls_ext.h>

namespace {

constexpr uint64_t TEST_VADDR = 0x10000000ULL;

} // namespace

// Both siblings block in a futex wait: that is the state where SIGKILL must
// reach them (futex waits are signal-aware). Running kernel-mode tasks are
// not signal-killable by design; user threads get their deaths at the
// syscall-return trampoline (signal_check), which the entry emulates here
// after the futex returns EINTR. Task 6's threadtest covers the real
// user-mode path end to end.
static volatile uint32_t *g_group_futex_addr;

static void group_futex_waiter_thread()
{
    sys_futex(g_group_futex_addr, FUTEX_WAIT, 0);
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            process_exit(0);
        scheduler_yield();
    }
}

KTEST(thread_group_exit_kills_blocked_siblings)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vma_list;
    uint32_t orig_vma_count = leader->vma_count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    g_group_futex_addr = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_group_futex_addr = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vma_list = vma;
    leader->vma_count = 1;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack_a = malloc(4096);
    void *stack_b = malloc(4096);
    KTEST_EXPECT(stack_a != nullptr);
    KTEST_EXPECT(stack_b != nullptr);
    void *top_a = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack_a) + 4096);
    void *top_b = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack_b) + 4096);

    const int64_t tid_a = sys_thread_create(group_futex_waiter_thread, nullptr, top_a, &mock_frame);
    const int64_t tid_b = sys_thread_create(group_futex_waiter_thread, nullptr, top_b, &mock_frame);
    KTEST_EXPECT(tid_a > 0);
    KTEST_EXPECT(tid_b > 0);

    // Give both threads time to reach the futex wait (queue waits put the
    // process into Waiting, not Blocked).
    bool a_blocked = false, b_blocked = false;
    for (int i = 0; i < 200 && !(a_blocked && b_blocked); i++) {
        scheduler_yield();
        Process *pa = process_find_by_pid(static_cast<uint64_t>(tid_a));
        Process *pb = process_find_by_pid(static_cast<uint64_t>(tid_b));
        a_blocked = pa && (pa->state == ProcessState_Blocked || pa->state == ProcessState_Waiting);
        b_blocked = pb && (pb->state == ProcessState_Blocked || pb->state == ProcessState_Waiting);
    }
    KTEST_EXPECT(a_blocked);
    KTEST_EXPECT(b_blocked);

    // The leader exits: group-kill every sibling (what process_exit does).
    process_group_kill_siblings(leader);

    // Bounded wait for both to die; never block on them directly so a lost
    // wake fails the assert instead of hanging the suite.
    bool a_dead = false, b_dead = false;
    for (int i = 0; i < 200 && !(a_dead && b_dead); i++) {
        scheduler_yield();
        Process *pa = process_find_by_pid(static_cast<uint64_t>(tid_a));
        Process *pb = process_find_by_pid(static_cast<uint64_t>(tid_b));
        a_dead = !pa || pa->state == ProcessState_Zombie;
        b_dead = !pb || pb->state == ProcessState_Zombie;
    }
    KTEST_EXPECT(a_dead);
    KTEST_EXPECT(b_dead);

    // Reap what the kernel-zombie path has not already taken.
    int32_t status = 0;
    (void)process_waitpid(tid_a, &status, 0);
    (void)process_waitpid(tid_b, &status, 0);

    free(stack_a);
    free(stack_b);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    // The dying siblings group-signaled this (kernel) task: clear the
    // pending mask so later ktests that sleep in signal-aware waits do not
    // inherit a false fatal signal.
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vma_list = orig_vma_list;
    leader->vma_count = orig_vma_count;
}
