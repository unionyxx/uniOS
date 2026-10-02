#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <kernel/syscall.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>
#include <uapi/syscalls.h>
#include <uapi/syscalls_ext.h>

extern "C" int64_t sys_fd_transfer(uint64_t target_pid, int fd);
void signal_send(Process *p, int sig);

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
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
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
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

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
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

static volatile bool g_thread_exit_ran;

static void thread_exit_entry()
{
    g_thread_exit_ran = true;
    sys_thread_exit(7);
}

KTEST(thread_exit_unmaps_recorded_stack)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    // The recorded stack covers the surgery page: exiting must unmap it.
    const int64_t tid = sys_thread_create(thread_exit_entry, nullptr, top, &mock_frame, TEST_VADDR, 4096);
    KTEST_EXPECT(tid > 0);

    // Bounded wait for the thread to die. ktest threads are children of the
    // pid-0 kernel task, so the kernel-zombie reaper (which runs after every
    // schedule pass) usually wins any waitpid race: read the exit status off
    // the parent's exec_exit_status instead, which the exit path stamps
    // before the zombie transition and nobody overwrites in between.
    bool gone = false;
    for (int i = 0; i < 200 && !gone; i++) {
        scheduler_yield();
        Process *t = process_find_by_pid(static_cast<uint64_t>(tid));
        gone = !t || t->state == ProcessState_Zombie;
    }
    KTEST_EXPECT(gone);
    KTEST_EXPECT(g_thread_exit_ran);

    // The recorded range is unmapped from the shared VMA list (the thread's
    // exit owns the VMA node and the frame on success).
    VMA *remaining = vma_find(leader->vmalist->head, TEST_VADDR);
    KTEST_EXPECT(remaining == nullptr);

    KTEST_EXPECT_EQ(leader->exec_exit_status, 7);
    int32_t wait_status = 0;
    (void)process_waitpid(tid, &wait_status, 0);

    free(stack);
    if (remaining) {
        // Failure path: the kernel did not unmap; clean up by hand.
        vmm_unmap_page_in(leader->page_table, TEST_VADDR);
        pmm_free_frame(page);
        free(vma);
    }
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

KTEST(thread_detach_self_reaps)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    const int64_t tid = sys_thread_create(group_futex_waiter_thread, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    // Detach while it is alive: the thread must leave the caller's children
    // list and route to the kernel-zombie auto-reap on exit.
    KTEST_EXPECT_EQ(sys_thread_detach(static_cast<uint64_t>(tid)), 0);
    // A second detach (or a bogus tid) is ESRCH.
    KTEST_EXPECT_EQ(sys_thread_detach(static_cast<uint64_t>(tid)), -10);
    KTEST_EXPECT_EQ(sys_thread_detach(99999), -10);

    // Kill it through the group path (the leader's exit signal) and wait for
    // the auto-reap: the process disappears from the table entirely.
    process_group_kill_siblings(leader);

    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        gone = process_find_by_pid(static_cast<uint64_t>(tid)) == nullptr;
    }
    KTEST_EXPECT(gone);

    free(stack);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
}

KTEST(futex_wait_timeout_expires)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    volatile uint32_t *word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    // Nobody wakes the word: a 50 ms timeout must expire instead of hanging.
    const int64_t r = sys_futex(word, FUTEX_WAIT, 0, 50);
    KTEST_EXPECT_EQ(r, -110);

    // Zero timeout still blocks until a real wake: the value mismatch path
    // stays immediate (EAGAIN, no sleep).
    *word = 1;
    KTEST_EXPECT_EQ(sys_futex(word, FUTEX_WAIT, 0, 0), -11);

    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

// ---- timed futex wait hardening ----

static volatile uint32_t *g_timed_word;

// Wakes the shared futex word at ~5 ms and again at ~45 ms after start:
// the first wake ends a short-deadline wait early (that wait's
// registration must be dropped, not left behind), the second ends the
// re-wait well before its own later deadline.
static void timed_two_stage_waker()
{
    scheduler_sleep_ms(5);
    sys_futex(g_timed_word, FUTEX_WAKE, 1);
    scheduler_sleep_ms(40);
    sys_futex(g_timed_word, FUTEX_WAKE, 1);
    sys_thread_exit(0);
}

static void timed_single_waker()
{
    scheduler_sleep_ms(10);
    sys_futex(g_timed_word, FUTEX_WAKE, 1);
    sys_thread_exit(0);
}

KTEST(futex_timed_wake_deregisters_on_early_wake)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    g_timed_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_timed_word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    // Wakes at ~5 ms and ~45 ms (deadline of wait 1 is ~30 ms, of wait 2
    // ~300 ms, so both wakes are genuinely early).
    const int64_t tid = sys_thread_create(timed_two_stage_waker, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    // Wait 1: genuinely woken before its deadline — must return 0.
    KTEST_EXPECT_EQ(sys_futex(g_timed_word, FUTEX_WAIT, 0, 30), 0);
    // Wait 2: fresh, far later deadline. A registration left behind by
    // wait 1 would fire the walker at wait 1's old deadline and turn this
    // into a spurious -110 while the thread is parked here.
    KTEST_EXPECT_EQ(sys_futex(g_timed_word, FUTEX_WAIT, 0, 300), 0);

    int32_t status = 0;
    (void)process_waitpid(tid, &status, 0);

    free(stack);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

KTEST(futex_timeout_rounds_up_to_one_tick)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    volatile uint32_t *word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    // A sub-tick timeout must still consume at least one full tick:
    // truncating (timeout_ms * freq) / 1000 to zero arms a deadline that
    // is already expired at registration, so the wait returns without a
    // single tick passing.
    const uint64_t t0 = timer_get_ticks();
    KTEST_EXPECT_EQ(sys_futex(word, FUTEX_WAIT, 0, 5), -110);
    KTEST_EXPECT(timer_get_ticks() > t0);

    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

KTEST(futex_huge_timeout_does_not_expire_early)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    g_timed_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_timed_word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    const int64_t tid = sys_thread_create(timed_single_waker, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    // A saturating timeout_ms must not wrap the deadline into the past:
    // the wait ends by the waker's wake (~10 ms), not by a premature
    // -110 fired at registration time.
    KTEST_EXPECT_EQ(sys_futex(g_timed_word, FUTEX_WAIT, 0, 1ULL << 63), 0);

    int32_t status = 0;
    (void)process_waitpid(tid, &status, 0);

    free(stack);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

// ---- untimed futex waits vs fatal signals ----

static volatile uint32_t *g_eintr_word;
static volatile int64_t g_eintr_retval;

static void futex_eintr_waiter_thread()
{
    g_eintr_retval = sys_futex(g_eintr_word, FUTEX_WAIT, 0);
    sys_thread_exit(0);
}

// The EINTR contract at every signal arrival point a ktest can drive an
// untimed futex wait through: pending before the call (pre-wait check)
// and pending while parked (wake + post-wait check). The third point —
// a fatal signal landing between the pre-wait check and the queue push —
// is SMP-only and not deterministically constructible here (on UP the
// bucket lock keeps IRQs off across that whole span); the queued recheck
// in the untimed path covers it by construction.
KTEST(futex_untimed_wait_returns_eintr_on_fatal_signal)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    volatile uint32_t *word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    // Signal already pending: the wait must refuse to sleep instead of
    // parking with a fatal signal queued.
    leader->signals.pending = 1ULL << SIGKILL;
    KTEST_EXPECT_EQ(sys_futex(word, FUTEX_WAIT, 0, 0), -4);
    leader->signals.pending = 0;

    // Signal arriving while parked: the waiter must come back with -EINTR
    // rather than sleeping until an unrelated futex wake.
    g_eintr_word = word;
    g_eintr_retval = 0;
    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    const int64_t tid = sys_thread_create(futex_eintr_waiter_thread, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    bool parked = false;
    for (int i = 0; i < 200 && !parked; i++) {
        scheduler_yield();
        Process *waiter = process_find_by_pid(static_cast<uint64_t>(tid));
        parked = waiter && (waiter->state == ProcessState_Waiting || waiter->state == ProcessState_Blocked);
    }
    KTEST_EXPECT(parked);

    signal_send(process_find_by_pid(static_cast<uint64_t>(tid)), SIGKILL);

    bool done = false;
    for (int i = 0; i < 500 && !done; i++) {
        scheduler_yield();
        done = g_eintr_retval != 0 || process_find_by_pid(static_cast<uint64_t>(tid)) == nullptr;
    }
    KTEST_EXPECT(done);
    KTEST_EXPECT_EQ(g_eintr_retval, -4);

    int32_t status = 0;
    (void)process_waitpid(tid, &status, 0);

    free(stack);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

// ---- SYS_THREAD_EXIT must not group-kill ----

static volatile bool g_exit9_ran;
static volatile uint64_t g_spinner_steps;
static volatile bool g_spinner_run;

static void exit_with_9_thread()
{
    g_exit9_ran = true;
    sys_thread_exit(9);
}

static void spinner_sibling_thread()
{
    while (g_spinner_run) {
        g_spinner_steps++;
        scheduler_yield();
    }
    sys_thread_exit(0);
}

KTEST(thread_exit_leaves_leader_alive)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *exit_stack = malloc(4096);
    void *spin_stack = malloc(4096);
    KTEST_EXPECT(exit_stack != nullptr);
    KTEST_EXPECT(spin_stack != nullptr);
    void *exit_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(exit_stack) + 4096);
    void *spin_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(spin_stack) + 4096);

    g_exit9_ran = false;
    g_spinner_run = true;
    g_spinner_steps = 0;

    // The exiting member records the surgery page as its stack; the
    // sibling just spins until released.
    const int64_t exit_tid = sys_thread_create(exit_with_9_thread, nullptr, exit_top, &mock_frame, TEST_VADDR, 4096);
    const int64_t spin_tid = sys_thread_create(spinner_sibling_thread, nullptr, spin_top, &mock_frame);
    KTEST_EXPECT(exit_tid > 0);
    KTEST_EXPECT(spin_tid > 0);

    // Bounded wait for the member to die and be auto-reaped (ktest
    // threads are children of the pid-0 kernel task).
    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        gone = process_find_by_pid(static_cast<uint64_t>(exit_tid)) == nullptr;
    }
    KTEST_EXPECT(gone);
    KTEST_EXPECT(g_exit9_ran);

    // The leader is still the running task, untouched by the member exit.
    KTEST_EXPECT(process_get_current() == leader);
    KTEST_EXPECT(leader->state == ProcessState_Running);
    // The whole point of SYS_THREAD_EXIT vs SYS_EXIT: no group-kill. A
    // regression to the group path would leave a pending SIGKILL on the
    // leader — this kernel task survives signals, but the mask shows it.
    KTEST_EXPECT((leader->signals.pending & (1ULL << SIGKILL)) == 0);

    // The live sibling is still schedulable and making progress.
    Process *spin = process_find_by_pid(static_cast<uint64_t>(spin_tid));
    KTEST_EXPECT(spin != nullptr);
    KTEST_EXPECT(spin->state != ProcessState_Zombie);
    const uint64_t steps_before = g_spinner_steps;
    for (int i = 0; i < 100 && g_spinner_steps == steps_before; i++)
        scheduler_yield();
    KTEST_EXPECT(g_spinner_steps > steps_before);

    // The member's shared references were released on its exit: the fd
    // table is back to leader + sibling, the recorded stack VMA is gone,
    // and the exit status reached the parent stamp.
    KTEST_EXPECT_EQ(leader->fdtab->refs, 2u);
    KTEST_EXPECT(vma_find(leader->vmalist->head, TEST_VADDR) == nullptr);
    KTEST_EXPECT_EQ(leader->exec_exit_status, 9);
    // Residual gap: the ktest leader is the pid-0 kernel task, immune to
    // signal death, so "the leader survives a real fatal group signal"
    // cannot be asserted in ktest context; Task 6's userspace threadtest
    // exercises the user-mode leader end to end.

    g_spinner_run = false;
    bool spin_gone = false;
    for (int i = 0; i < 500 && !spin_gone; i++) {
        scheduler_yield();
        spin_gone = process_find_by_pid(static_cast<uint64_t>(spin_tid)) == nullptr;
    }
    KTEST_EXPECT(spin_gone);

    int32_t status = 0;
    (void)process_waitpid(exit_tid, &status, 0);
    (void)process_waitpid(spin_tid, &status, 0);

    free(exit_stack);
    free(spin_stack);
    if (vma_find(leader->vmalist->head, TEST_VADDR)) {
        // Failure path: the kernel did not unmap; clean up by hand.
        vmm_unmap_page_in(leader->page_table, TEST_VADDR);
        pmm_free_frame(page);
        free(vma);
    }
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

KTEST(thread_detach_rejects_forked_children)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    // A stand-in for a forked child: its own group leader (leader_pid ==
    // pid), linked as a child but never on the process list or any queue,
    // so only the detach path's children-list walk can observe it.
    auto *forked = static_cast<Process *>(aligned_alloc(64, sizeof(Process)));
    KTEST_EXPECT(forked != nullptr);
    kstring::zero_memory(forked, sizeof(Process));
    forked->pid = 60000;
    forked->leader_pid = 60000;
    forked->parent_pid = leader->pid;
    forked->state = ProcessState_Ready;
    forked->sibling_next = leader->children_list;
    leader->children_list = forked;

    // Detach must refuse it (-ESRCH): orphaning a forked child would send
    // its exit status into the kernel-zombie auto-reap where the parent
    // can never collect it.
    KTEST_EXPECT_EQ(sys_thread_detach(60000), -10);

    // Teardown: unlink the stand-in if the call left it in place.
    Process *prev = nullptr;
    Process *c = leader->children_list;
    while (c) {
        if (c == forked) {
            if (prev)
                prev->sibling_next = c->sibling_next;
            else
                leader->children_list = c->sibling_next;
            break;
        }
        prev = c;
        c = c->sibling_next;
    }
    aligned_free(forked);
}

static void zombie_surgery_thread_entry()
{
    while (true)
        scheduler_yield();
}

KTEST(fd_transfer_rejects_zombie_without_fd_table)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    // A thread that never runs, zombified by hand with its fd-table
    // reference already dropped — exactly the state of an
    // exited-but-unreaped joinable zombie (process_release_private_fds
    // nulls fdtab at exit).
    const int64_t tid = sys_thread_create(zombie_surgery_thread_entry, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);
    Process *thread = process_find_by_pid(static_cast<uint64_t>(tid));
    KTEST_EXPECT(thread != nullptr);
    if (!thread) {
        free(stack);
        leader->page_table = orig_page_table;
        return;
    }

    scheduler_remove_from_ready_queue(thread);
    fd_table_release(thread->fdtab);
    thread->fdtab = nullptr;
    thread->state = ProcessState_Zombie;
    // Sever the shared objects so the reaper fully frees this thread
    // instead of deferring while the leader lives.
    thread->vmalist = nullptr;
    thread->page_table = nullptr;

    // Transferring into a zombie with no fd table must fail cleanly
    // (-ESRCH), not dereference the null table.
    KTEST_EXPECT_EQ(sys_fd_transfer(static_cast<uint64_t>(tid), 0), -3);

    int32_t status = 0;
    KTEST_EXPECT_EQ(process_waitpid(tid, &status, 0), tid);

    free(stack);
    leader->page_table = orig_page_table;
}

KTEST(futex_timed_wait_table_full_returns_enospc)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    uint32_t orig_vma_count = leader->vmalist->count;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    volatile uint32_t *word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;
    leader->vmalist->count = 1;

    // Fill every timed-wait slot with far-future registrations on fake
    // stand-ins: the walker only compares pointers, and a far-future
    // deadline means it never touches them during the test.
    Process *dummies[64];
    size_t placed = 0;
    const uint64_t far = timer_get_ticks() + 1000000000ULL;
    for (; placed < 64; placed++) {
        dummies[placed] = reinterpret_cast<Process *>(0x10000ULL + placed * 64);
        if (!scheduler_note_wake_deadline(dummies[placed], far))
            break;
    }
    KTEST_EXPECT(placed >= 1);

    // The next timed waiter must get an honest -ENOSPC instead of a
    // silently dropped timeout (an unbounded hang).
    KTEST_EXPECT_EQ(sys_futex(word, FUTEX_WAIT, 0, 50), -28);

    for (size_t i = 0; i <= placed && i < 64; i++)
        scheduler_clear_wake_deadline(dummies[i]);

    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
    leader->vmalist->count = orig_vma_count;
}

KTEST(thread_create_detached_flag_self_reaps)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    // THREAD_DETACHED at create time: the thread is never linked into any
    // children list, so it is not waitable, and its exit routes to the
    // kernel-zombie auto-reap.
    const int64_t tid = sys_thread_create(group_futex_waiter_thread, nullptr, top, &mock_frame, 0, 0, THREAD_DETACHED);
    KTEST_EXPECT(tid > 0);

    int32_t status = 0;
    KTEST_EXPECT_EQ(process_waitpid(static_cast<int64_t>(tid), &status, WNOHANG), -1);

    // Kill it through the group path (the leader's exit signal) and wait
    // for the auto-reap: the process disappears without any waitpid.
    process_group_kill_siblings(leader);

    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        gone = process_find_by_pid(static_cast<uint64_t>(tid)) == nullptr;
    }
    KTEST_EXPECT(gone);

    free(stack);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
}
