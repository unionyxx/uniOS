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
// Second mapping of a shared frame: distinct virtual page, same physical
// word when the same frame is mapped at both addresses.
constexpr uint64_t TEST_VADDR_ALIAS = 0x10010000ULL;

} // namespace

// Threads this file abandons (signal death, hand-zombification) never
// leave through SYS_THREAD_EXIT, so their TLS mappings stay in the shared
// list; the head restore in each cleanup would drop the VMA nodes while
// their PTEs stay mapped, poisoning every later first-fit
// (vmm_map_page_in refuses present PTEs). Drain them first.
static void drain_abandoned_tls(Process *leader)
{
    uint64_t lo[8];
    uint64_t len[8];
    int found = 0;
    for (VMA *v = leader->vmalist->head; v && found < 8; v = v->next) {
        if (v->type == VMAType::Data) {
            lo[found] = v->start;
            len[found] = v->end - v->start;
            found++;
        }
    }
    for (int i = 0; i < found; i++)
        (void)munmap_process_range(leader, lo[i], len[i]);
}

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

    // The siblings died by the group signal: drain their TLS mappings
    // before the head restore drops the nodes.
    drain_abandoned_tls(leader);

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

    // Signal death leaves the thread's TLS mapping in the shared list.
    drain_abandoned_tls(leader);

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
}

KTEST(futex_timeout_rounds_up_to_one_tick)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
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
}

KTEST(futex_huge_timeout_does_not_expire_early)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
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
}

// ---- futex wake word matching ----

// Waiter threads park on one 32-bit futex word each and record their
// sys_futex return; 999 marks "still parked" (every real return is 0 or a
// negative errno).
static volatile uint32_t *g_word_wait_a;
static volatile uint32_t *g_word_wait_b;
static volatile int64_t g_word_ret_a;
static volatile int64_t g_word_ret_b;

static void futex_word_waiter_a()
{
    g_word_ret_a = sys_futex(g_word_wait_a, FUTEX_WAIT, 0);
    sys_thread_exit(0);
}

static void futex_word_waiter_b()
{
    g_word_ret_b = sys_futex(g_word_wait_b, FUTEX_WAIT, 0);
    sys_thread_exit(0);
}

// True once the thread reached its futex park (queue waits park in
// Waiting, not Blocked).
static bool word_waiter_parked(int64_t tid)
{
    Process *t = process_find_by_pid(static_cast<uint64_t>(tid));
    return t && (t->state == ProcessState_Waiting || t->state == ProcessState_Blocked);
}

// B parks on a different word of the SAME page as A, and parks FIRST (it
// owns the bucket queue head, exactly the interleaving where a page-granular
// wake(A) steals B's queue slot). A word-matched wake must reach A only.
KTEST(futex_wake_matches_only_the_addressed_word)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    g_word_wait_a = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    g_word_wait_b = reinterpret_cast<volatile uint32_t *>(TEST_VADDR + 0x800);
    *g_word_wait_a = 0;
    *g_word_wait_b = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;

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
    g_word_ret_a = 999;
    g_word_ret_b = 999;

    // Park B first so it owns the queue head, then A.
    const int64_t tid_b = sys_thread_create(futex_word_waiter_b, nullptr, top_b, &mock_frame);
    KTEST_EXPECT(tid_b > 0);
    bool b_parked = false;
    for (int i = 0; i < 200 && !b_parked; i++) {
        scheduler_yield();
        b_parked = word_waiter_parked(tid_b);
    }
    KTEST_EXPECT(b_parked);

    const int64_t tid_a = sys_thread_create(futex_word_waiter_a, nullptr, top_a, &mock_frame);
    KTEST_EXPECT(tid_a > 0);
    bool a_parked = false;
    for (int i = 0; i < 200 && !a_parked; i++) {
        scheduler_yield();
        a_parked = word_waiter_parked(tid_a);
    }
    KTEST_EXPECT(a_parked);

    // Waking word A must spend its single wake on A's waiter: B shares the
    // bucket (same page) but not the word.
    KTEST_EXPECT_EQ(sys_futex(g_word_wait_a, FUTEX_WAKE, 1), 1);
    bool a_done = false;
    for (int i = 0; i < 200 && !a_done; i++) {
        scheduler_yield();
        a_done = g_word_ret_a != 999;
    }
    KTEST_EXPECT(a_done);
    KTEST_EXPECT_EQ(g_word_ret_a, 0);

    // Give B every chance to run: it must still be parked on word B.
    for (int i = 0; i < 200; i++)
        scheduler_yield();
    KTEST_EXPECT_EQ(g_word_ret_b, 999);
    KTEST_EXPECT(word_waiter_parked(tid_b));

    // No waiter is left on word A: a further wake must report zero even
    // though B still sleeps in the same bucket.
    KTEST_EXPECT_EQ(sys_futex(g_word_wait_a, FUTEX_WAKE, 1), 0);

    // Drain B through its own word.
    KTEST_EXPECT_EQ(sys_futex(g_word_wait_b, FUTEX_WAKE, 1), 1);
    bool b_done = false;
    for (int i = 0; i < 200 && !b_done; i++) {
        scheduler_yield();
        b_done = g_word_ret_b != 999;
    }
    KTEST_EXPECT(b_done);
    KTEST_EXPECT_EQ(g_word_ret_b, 0);

    int32_t status = 0;
    (void)process_waitpid(tid_a, &status, 0);
    (void)process_waitpid(tid_b, &status, 0);

    free(stack_a);
    free(stack_b);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
}

// The wake count must be spent per word: wake(1) reaches one of the
// waiters parked on that word, never more, and the word keeps its queue.
static volatile uint32_t *g_count_word;
static volatile int64_t g_count_rets[2];
static volatile int g_count_next_slot;

static void futex_count_waiter()
{
    const int slot = __sync_fetch_and_add(&g_count_next_slot, 1);
    g_count_rets[slot] = sys_futex(g_count_word, FUTEX_WAIT, 0);
    sys_thread_exit(0);
}

KTEST(futex_wake_count_spends_one_wake_per_word)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    g_count_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_count_word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack_one = malloc(4096);
    void *stack_two = malloc(4096);
    KTEST_EXPECT(stack_one != nullptr);
    KTEST_EXPECT(stack_two != nullptr);
    void *top_one = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack_one) + 4096);
    void *top_two = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack_two) + 4096);
    g_count_rets[0] = 999;
    g_count_rets[1] = 999;
    g_count_next_slot = 0;

    const int64_t tid_one = sys_thread_create(futex_count_waiter, nullptr, top_one, &mock_frame);
    const int64_t tid_two = sys_thread_create(futex_count_waiter, nullptr, top_two, &mock_frame);
    KTEST_EXPECT(tid_one > 0);
    KTEST_EXPECT(tid_two > 0);

    bool both_parked = false;
    for (int i = 0; i < 200 && !both_parked; i++) {
        scheduler_yield();
        both_parked = word_waiter_parked(tid_one) && word_waiter_parked(tid_two);
    }
    KTEST_EXPECT(both_parked);

    KTEST_EXPECT_EQ(sys_futex(g_count_word, FUTEX_WAKE, 1), 1);

    bool one_done = false;
    for (int i = 0; i < 200 && !one_done; i++) {
        scheduler_yield();
        one_done = g_count_rets[0] != 999 || g_count_rets[1] != 999;
    }
    KTEST_EXPECT(one_done);

    const int first = (g_count_rets[0] != 999) ? 0 : 1;
    const int second = 1 - first;
    KTEST_EXPECT_EQ(g_count_rets[first], 0);
    KTEST_EXPECT_EQ(g_count_rets[second], 999);

    // The second waiter still owns the word: the next wake reaches it.
    KTEST_EXPECT_EQ(sys_futex(g_count_word, FUTEX_WAKE, 1), 1);
    bool all_done = false;
    for (int i = 0; i < 200 && !all_done; i++) {
        scheduler_yield();
        all_done = g_count_rets[0] != 999 && g_count_rets[1] != 999;
    }
    KTEST_EXPECT(all_done);
    KTEST_EXPECT_EQ(g_count_rets[0], 0);
    KTEST_EXPECT_EQ(g_count_rets[1], 0);

    // Fully drained: a further wake reports zero.
    KTEST_EXPECT_EQ(sys_futex(g_count_word, FUTEX_WAKE, 1), 0);

    int32_t status = 0;
    (void)process_waitpid(tid_one, &status, 0);
    (void)process_waitpid(tid_two, &status, 0);

    free(stack_one);
    free(stack_two);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
}

// The futex key is the PHYSICAL word: one frame mapped at two virtual
// addresses (the in-ktest stand-in for two processes sharing a memfd page)
// must match through either mapping, while a different word of the same
// frame sharing the bucket must not.
KTEST(futex_wake_matches_across_virtual_aliases)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    Result<void> map_alias = vmm_replace_page_in(leader->page_table, TEST_VADDR_ALIAS, reinterpret_cast<uint64_t>(page),
                                                 PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map_alias.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    VMA *vma_alias = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    KTEST_EXPECT(vma_alias != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = vma_alias;
    vma_alias->start = TEST_VADDR_ALIAS;
    vma_alias->end = TEST_VADDR_ALIAS + 4096;
    vma_alias->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma_alias->type = VMAType::Anonymous;
    vma_alias->next = nullptr;
    leader->vmalist->head = vma;

    // A parks on word 0 of the frame THROUGH THE ALIAS; B parks on word 8
    // of the same frame through the first mapping.
    g_word_wait_a = reinterpret_cast<volatile uint32_t *>(TEST_VADDR_ALIAS);
    g_word_wait_b = reinterpret_cast<volatile uint32_t *>(TEST_VADDR + 8);
    *g_word_wait_a = 0;
    *g_word_wait_b = 0;

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
    g_word_ret_a = 999;
    g_word_ret_b = 999;

    // Park B first so it owns the bucket queue head.
    const int64_t tid_b = sys_thread_create(futex_word_waiter_b, nullptr, top_b, &mock_frame);
    KTEST_EXPECT(tid_b > 0);
    bool b_parked = false;
    for (int i = 0; i < 200 && !b_parked; i++) {
        scheduler_yield();
        b_parked = word_waiter_parked(tid_b);
    }
    KTEST_EXPECT(b_parked);

    const int64_t tid_a = sys_thread_create(futex_word_waiter_a, nullptr, top_a, &mock_frame);
    KTEST_EXPECT(tid_a > 0);
    bool a_parked = false;
    for (int i = 0; i < 200 && !a_parked; i++) {
        scheduler_yield();
        a_parked = word_waiter_parked(tid_a);
    }
    KTEST_EXPECT(a_parked);

    // Wake word 0 through the FIRST mapping while the waiter parked through
    // the alias: same physical word, so the wake must reach it.
    KTEST_EXPECT_EQ(sys_futex(reinterpret_cast<volatile uint32_t *>(TEST_VADDR), FUTEX_WAKE, 1), 1);
    bool a_done = false;
    for (int i = 0; i < 200 && !a_done; i++) {
        scheduler_yield();
        a_done = g_word_ret_a != 999;
    }
    KTEST_EXPECT(a_done);
    KTEST_EXPECT_EQ(g_word_ret_a, 0);

    // B parks on a different word of the same frame: it must stay parked,
    // and no further waiter exists on word 0 through either mapping.
    for (int i = 0; i < 200; i++)
        scheduler_yield();
    KTEST_EXPECT_EQ(g_word_ret_b, 999);
    KTEST_EXPECT(word_waiter_parked(tid_b));
    KTEST_EXPECT_EQ(sys_futex(reinterpret_cast<volatile uint32_t *>(TEST_VADDR), FUTEX_WAKE, 1), 0);
    KTEST_EXPECT_EQ(sys_futex(reinterpret_cast<volatile uint32_t *>(TEST_VADDR_ALIAS), FUTEX_WAKE, 1), 0);

    // Drain B through the ALIAS mapping of its own word: same physical
    // word reached through a different virtual address.
    KTEST_EXPECT_EQ(sys_futex(reinterpret_cast<volatile uint32_t *>(TEST_VADDR_ALIAS + 8), FUTEX_WAKE, 1), 1);
    bool b_done = false;
    for (int i = 0; i < 200 && !b_done; i++) {
        scheduler_yield();
        b_done = g_word_ret_b != 999;
    }
    KTEST_EXPECT(b_done);
    KTEST_EXPECT_EQ(g_word_ret_b, 0);

    int32_t status = 0;
    (void)process_waitpid(tid_a, &status, 0);
    (void)process_waitpid(tid_b, &status, 0);

    free(stack_a);
    free(stack_b);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR_ALIAS);
    pmm_free_frame(page);
    free(vma);
    free(vma_alias);
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
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
    // The thread never exits through SYS_THREAD_EXIT: drain its TLS
    // mapping before severing, or the node-PTE pair outlives the test.
    if (thread->tls_lo != 0) {
        (void)munmap_process_range(thread, thread->tls_lo, thread->tls_len);
        thread->tls_lo = 0;
        thread->tls_len = 0;
    }
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

    // Signal death leaves the thread's TLS mapping in the shared list.
    drain_abandoned_tls(leader);

    free(stack);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
}

// ---- exec from a process with live sibling threads ----

static volatile uint32_t *g_exec_futex_addr;

static void exec_futex_waiter_thread()
{
    sys_futex(g_exec_futex_addr, FUTEX_WAIT, 0);
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            process_exit(0);
        scheduler_yield();
    }
}

// POSIX: exec terminates all other threads of the process. The members
// must die BEFORE the address space is swapped, or they keep running on
// the freed old page tables and the deferred reaper re-frees what the
// exec already freed. Drives the real SYS_EXEC dispatch end to end from
// ktest context: the leader gets a private throwaway address space (so
// the old pml4 is a real freed object observable through its PMM
// refcount), two members park in a futex on it, and /bin/init.elf is
// loaded over the group.
#define EXEC_GROUP_CHECK(cond)                                                                                         \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ktest_record_failure(#cond, __FILE__, __LINE__);                                                           \
            goto cleanup;                                                                                              \
        }                                                                                                              \
    } while (0)
#define EXEC_GROUP_CHECK_EQ(a, b) EXEC_GROUP_CHECK((a) == (b))

KTEST(exec_from_threaded_group_kills_siblings_first)
{
    Process *leader = process_get_current();
    if (!leader) {
        ktest_record_failure("leader != nullptr", __FILE__, __LINE__);
        return;
    }

    const uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    Spinlock *orig_vma_lock_ptr = leader->vma_lock_ptr;
    const uint64_t orig_exec_entry = leader->exec_entry;
    char orig_name[32];
    kstring::strncpy(orig_name, leader->name, 31);

    // Declared up front: every failure jump must reach cleanup with the
    // full cleanup set in scope. Everything else lives in the act block
    // below so the jumps only ever leave scopes, never enter them.
    bool exec_ok = false;
    uint64_t *old_pml4 = nullptr;
    void *page = nullptr;
    VMA *vma = nullptr;
    void *stack_a = nullptr;
    void *stack_b = nullptr;
    int64_t tid_a = -1;
    int64_t tid_b = -1;

    // A real old address space (not the shared kernel pml4): the exec
    // must free it exactly once, and the pml4 frame's refcount observes
    // both "never freed" and "still referenced".
    old_pml4 = vmm_create_address_space();
    {
        EXEC_GROUP_CHECK(old_pml4 != nullptr);
        leader->page_table = old_pml4;

        page = pmm_alloc_frame();
        EXEC_GROUP_CHECK(page != nullptr);
        Result<void> map = vmm_replace_page_in(old_pml4, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                               PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        EXEC_GROUP_CHECK(map.ok());

        vma = static_cast<VMA *>(malloc(sizeof(VMA)));
        EXEC_GROUP_CHECK(vma != nullptr);
        vma->start = TEST_VADDR;
        vma->end = TEST_VADDR + 4096;
        vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
        vma->type = VMAType::Anonymous;
        vma->next = nullptr;
        leader->vmalist->head = vma;

        // The futex word and the exec path string share the surgical page.
        // Write through the physical alias so the stores do not depend on
        // which page table this core currently has loaded.
        uint8_t *page_alias = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(page)));
        g_exec_futex_addr = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
        *reinterpret_cast<volatile uint32_t *>(page_alias) = 0;
        kstring::strncpy(reinterpret_cast<char *>(page_alias + 0x400), "/bin/init.elf", 31);

        SyscallFrame mock_frame = {};
        mock_frame.cs = 0x08;
        mock_frame.ss = 0x10;
        mock_frame.rflags = 0x202;

        stack_a = malloc(4096);
        stack_b = malloc(4096);
        EXEC_GROUP_CHECK(stack_a != nullptr);
        EXEC_GROUP_CHECK(stack_b != nullptr);
        void *top_a = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack_a) + 4096);
        void *top_b = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack_b) + 4096);

        tid_a = sys_thread_create(exec_futex_waiter_thread, nullptr, top_a, &mock_frame);
        tid_b = sys_thread_create(exec_futex_waiter_thread, nullptr, top_b, &mock_frame);
        EXEC_GROUP_CHECK(tid_a > 0);
        EXEC_GROUP_CHECK(tid_b > 0);

        // Both members park in the futex wait (queue waits park in
        // Waiting, not Blocked) before the exec fires.
        bool a_parked = false, b_parked = false;
        for (int i = 0; i < 200 && !(a_parked && b_parked); i++) {
            scheduler_yield();
            a_parked = word_waiter_parked(tid_a);
            b_parked = word_waiter_parked(tid_b);
        }
        EXEC_GROUP_CHECK(a_parked);
        EXEC_GROUP_CHECK(b_parked);

        const uint64_t rc = syscall_handler(SYS_EXEC, TEST_VADDR + 0x400, 0, 0, &mock_frame);
        EXEC_GROUP_CHECK_EQ(rc, 0ULL);
        exec_ok = true;

        // The dying members' exit cascade group-signaled the exec'ing thread
        // (each killed member's process_exit group-kills its survivors). A
        // user-mode exec'er delivers that pending SIGKILL at the syscall
        // trampoline and the fresh image dies before its first instruction,
        // so the exec swap must leave the survivor's pending mask clean.
        EXEC_GROUP_CHECK_EQ(leader->signals.pending, 0ULL);

        // The siblings were killed and reaped before the swap: gone from
        // the process list (their structs may still sit on the deferred
        // list, severed, until the next reap pass).
        bool a_gone = false, b_gone = false;
        for (int i = 0; i < 500 && !(a_gone && b_gone); i++) {
            scheduler_yield();
            a_gone = process_find_by_pid(static_cast<uint64_t>(tid_a)) == nullptr;
            b_gone = process_find_by_pid(static_cast<uint64_t>(tid_b)) == nullptr;
        }
        EXEC_GROUP_CHECK(a_gone);
        EXEC_GROUP_CHECK(b_gone);

        // The leader survived, on the new address space (new pml4, new
        // VMA list, new name from the exec'ed binary).
        EXEC_GROUP_CHECK(process_get_current() == leader);
        EXEC_GROUP_CHECK(leader->page_table != old_pml4);
        EXEC_GROUP_CHECK(leader->page_table != nullptr);
        EXEC_GROUP_CHECK(leader->vmalist->head != vma);
        EXEC_GROUP_CHECK(kstring::strcmp(leader->name, "init.elf") == 0);

        // The old address space was freed exactly once: the pml4 frame's
        // refcount reached zero right here, before any frame can be
        // reused.
        const uint64_t old_pml4_frame = reinterpret_cast<uint64_t>(old_pml4) - vmm_get_hhdm_offset();
        EXEC_GROUP_CHECK_EQ(pmm_get_refcount(reinterpret_cast<void *>(old_pml4_frame)), 0u);

        // Let the reap passes collect the severed sibling structs: every
        // fd reference they held comes back to the leader alone.
        for (int i = 0; i < 100; i++)
            scheduler_yield();
        EXEC_GROUP_CHECK_EQ(leader->fdtab->refs, 1u);
    }

cleanup:
    if (exec_ok) {
        // Leave the exec'ed page tables before freeing them: this core
        // still runs on them (kernel half), and the pml4 frame must not
        // go back to the PMM under a live CR3.
        vmm_switch_address_space(
            reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));
        vma_free_all(leader->vmalist->head);
        vmm_free_address_space(leader->page_table);
    } else {
        // Failure path: the exec never swapped anything — the members are
        // still alive on the surgical address space. Kill and drain them
        // by hand, then undo the surgery.
        process_group_kill_siblings(leader);
        for (int i = 0; i < 500; i++) {
            scheduler_yield();
            Process *pa = tid_a > 0 ? process_find_by_pid(static_cast<uint64_t>(tid_a)) : nullptr;
            Process *pb = tid_b > 0 ? process_find_by_pid(static_cast<uint64_t>(tid_b)) : nullptr;
            if ((pa == nullptr || pa->state == ProcessState_Zombie) &&
                (pb == nullptr || pb->state == ProcessState_Zombie))
                break;
        }
        int32_t status = 0;
        if (tid_a > 0)
            (void)process_waitpid(tid_a, &status, 0);
        if (tid_b > 0)
            (void)process_waitpid(tid_b, &status, 0);
        vmm_switch_address_space(
            reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));
        if (page) {
            vmm_unmap_page_in(old_pml4, TEST_VADDR);
            pmm_free_frame(page);
        }
        if (old_pml4)
            vmm_free_address_space(old_pml4);
        free(vma);
    }
    free(stack_a);
    free(stack_b);
    leader->signals.pending = 0;
    leader->page_table = const_cast<uint64_t *>(orig_page_table);
    leader->vmalist->head = orig_vma_list;
    leader->vma_lock_ptr = orig_vma_lock_ptr;
    leader->exec_entry = orig_exec_entry;
    kstring::strncpy(leader->name, orig_name, 31);
}

#undef EXEC_GROUP_CHECK
#undef EXEC_GROUP_CHECK_EQ

// ---- thread creation must be gated while exec tears the group down ----

#define GATE_CHECK(cond)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ktest_record_failure(#cond, __FILE__, __LINE__);                                                           \
            goto cleanup;                                                                                              \
        }                                                                                                              \
    } while (0)
#define GATE_CHECK_EQ(a, b) GATE_CHECK((a) == (b))

static void gate_parked_thread()
{
    sys_futex(g_group_futex_addr, FUTEX_WAIT, 0);
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            sys_thread_exit(0);
        scheduler_yield();
    }
}

static void gate_race_child_entry()
{
    sys_thread_exit(0);
}

static volatile bool g_gate_race_run = true;
static uint8_t *g_gate_race_stacks = nullptr;
static SyscallFrame g_gate_race_frame;

// Hammers thread creates against the group while the leader execs: every
// create must either land before the kill scan (the member then dies with
// the group) or be refused with -11 while the gate is up. The fatal-signal
// check inside the loop is what lets the teardown's SIGKILL collect the
// creator instead of waiting out the exec's death deadline.
static void gate_race_creator_thread()
{
    for (int i = 0; g_gate_race_run && i < 2000; i++) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            sys_thread_exit(0);
        const int slot = i % 64;
        void *top = g_gate_race_stacks + static_cast<size_t>(slot + 1) * 4096;
        (void)sys_thread_create(gate_race_child_entry, nullptr, top, &g_gate_race_frame, 0, 0, THREAD_DETACHED);
        scheduler_yield();
    }
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            sys_thread_exit(0);
        scheduler_yield();
    }
}

KTEST(exec_gates_thread_create_during_teardown)
{
    Process *leader = process_get_current();
    if (!leader) {
        ktest_record_failure("leader != nullptr", __FILE__, __LINE__);
        return;
    }

    const uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    Spinlock *orig_vma_lock_ptr = leader->vma_lock_ptr;
    const uint64_t orig_exec_entry = leader->exec_entry;
    char orig_name[32];
    kstring::strncpy(orig_name, leader->name, 31);

    bool exec_ok = false;
    uint64_t *old_pml4 = nullptr;
    void *page = nullptr;
    VMA *vma = nullptr;
    void *creator_stack = nullptr;
    void *race_stacks = nullptr;
    int64_t parked_tid = -1;
    int64_t creator_tid = -1;

    {
        old_pml4 = vmm_create_address_space();
        GATE_CHECK(old_pml4 != nullptr);
        leader->page_table = old_pml4;

        page = pmm_alloc_frame();
        GATE_CHECK(page != nullptr);
        GATE_CHECK(vmm_replace_page_in(old_pml4, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                       PTE_PRESENT | PTE_USER | PTE_WRITABLE)
                       .ok());

        vma = static_cast<VMA *>(malloc(sizeof(VMA)));
        GATE_CHECK(vma != nullptr);
        vma->start = TEST_VADDR;
        vma->end = TEST_VADDR + 4096;
        vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
        vma->type = VMAType::Anonymous;
        vma->next = nullptr;
        leader->vmalist->head = vma;

        uint8_t *page_alias = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(page)));
        g_group_futex_addr = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
        *reinterpret_cast<volatile uint32_t *>(page_alias) = 0;
        kstring::strncpy(reinterpret_cast<char *>(page_alias + 0x400), "/bin/init.elf", 31);

        SyscallFrame mock_frame = {};
        mock_frame.cs = 0x08;
        mock_frame.ss = 0x10;
        mock_frame.rflags = 0x202;

        creator_stack = malloc(4096);
        race_stacks = malloc(64 * 4096);
        GATE_CHECK(creator_stack != nullptr);
        GATE_CHECK(race_stacks != nullptr);
        g_gate_race_stacks = static_cast<uint8_t *>(race_stacks);
        g_gate_race_frame = mock_frame;
        void *creator_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(creator_stack) + 4096);

        // The gate: while any member of the caller's group carries
        // exec_in_progress, a create must refuse instead of publishing a
        // thread that would survive the kill scan.
        leader->exec_in_progress = true;
        GATE_CHECK_EQ(sys_thread_create(gate_parked_thread, nullptr, creator_top, &mock_frame), int64_t(-11));
        leader->exec_in_progress = false;

        // Control: with the gate down, the same create succeeds; drain the
        // parked control thread afterwards so the fixture is proven.
        parked_tid = sys_thread_create(gate_parked_thread, nullptr, creator_top, &mock_frame);
        GATE_CHECK(parked_tid > 0);
        {
            Process *child = process_find_by_pid(static_cast<uint64_t>(parked_tid));
            GATE_CHECK(child != nullptr);
            scheduler_remove_from_ready_queue(child);
            child->state = ProcessState_Zombie;
            if (child->tls_lo != 0) {
                (void)munmap_process_range(child, child->tls_lo, child->tls_len);
                child->tls_lo = 0;
                child->tls_len = 0;
            }
            child->vmalist = nullptr;
            child->page_table = nullptr;
            int32_t status = 0;
            GATE_CHECK_EQ(process_waitpid(parked_tid, &status, 0), parked_tid);
            parked_tid = -1;
        }

        // Race: a member hammers creates while the leader execs. The exec
        // must succeed (not be refused by a straggler the kill scan missed)
        // and the group must end up as the leader alone.
        g_gate_race_run = true;
        creator_tid = sys_thread_create(gate_race_creator_thread, nullptr, creator_top, &mock_frame);
        GATE_CHECK(creator_tid > 0);
        for (int i = 0; i < 20; i++)
            scheduler_yield();

        const uint64_t rc = syscall_handler(SYS_EXEC, TEST_VADDR + 0x400, 0, 0, &mock_frame);
        GATE_CHECK_EQ(rc, 0ULL);
        exec_ok = true;
        g_gate_race_run = false;

        bool members_gone = false;
        for (int i = 0; i < 500 && !members_gone; i++) {
            scheduler_yield();
            members_gone = true;
            Process *scan = scheduler_get_process_list();
            if (scan) {
                do {
                    if (scan != leader && scan->leader_pid == leader->pid && scan->state != ProcessState_Zombie)
                        members_gone = false;
                    scan = scan->next;
                } while (scan != scheduler_get_process_list());
            }
        }
        GATE_CHECK(members_gone);
        GATE_CHECK_EQ(leader->signals.pending, 0ULL);
        GATE_CHECK(!leader->exec_in_progress);
    }

cleanup:
    g_gate_race_run = false;
    leader->exec_in_progress = false;
    if (parked_tid > 0) {
        Process *leftover = process_find_by_pid(static_cast<uint64_t>(parked_tid));
        if (leftover)
            signal_send(leftover, SIGKILL);
        for (int i = 0; i < 500; i++) {
            if (process_find_by_pid(static_cast<uint64_t>(parked_tid)) == nullptr)
                break;
            scheduler_yield();
        }
    }
    if (creator_tid > 0) {
        Process *leftover = process_find_by_pid(static_cast<uint64_t>(creator_tid));
        if (leftover)
            signal_send(leftover, SIGKILL);
        for (int i = 0; i < 500; i++) {
            if (process_find_by_pid(static_cast<uint64_t>(creator_tid)) == nullptr)
                break;
            scheduler_yield();
        }
    }
    if (exec_ok) {
        vmm_switch_address_space(
            reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));
        vma_free_all(leader->vmalist->head);
        vmm_free_address_space(leader->page_table);
    } else if (old_pml4) {
        process_group_kill_siblings(leader);
        for (int i = 0; i < 500; i++)
            scheduler_yield();
        vmm_switch_address_space(
            reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));
        vmm_unmap_page_in(old_pml4, TEST_VADDR);
        vmm_free_address_space(old_pml4);
    }
    g_gate_race_stacks = nullptr;
    free(creator_stack);
    free(race_stacks);
    // On the success path the exec's vma_free_all already released the old
    // list's nodes - including this test's surgical VMA. Only the refusal
    // path still owns it.
    if (!exec_ok)
        free(vma);
    leader->signals.pending = 0;
    leader->page_table = const_cast<uint64_t *>(orig_page_table);
    leader->vmalist->head = orig_vma_list;
    leader->vma_lock_ptr = orig_vma_lock_ptr;
    leader->exec_entry = orig_exec_entry;
    kstring::strncpy(leader->name, orig_name, 31);
}

#undef GATE_CHECK
#undef GATE_CHECK_EQ

// ---- blocking waitpid must be killable ----

static volatile int64_t g_waitpid_ret;
static volatile int64_t g_waitpid_child_tid;
static volatile bool g_waitpid_spinner_run;

static void waitpid_spinner_thread()
{
    while (g_waitpid_spinner_run)
        scheduler_yield();
    sys_thread_exit(0);
}

static void waitpid_blocker_thread()
{
    // The child must be this thread's own: waitpid walks the caller's
    // children list (the documented join-from-creating-thread limit).
    SyscallFrame frame = {};
    frame.cs = 0x08;
    frame.ss = 0x10;
    frame.rflags = 0x202;

    void *spin_stack = malloc(4096);
    void *spin_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(spin_stack) + 4096);
    g_waitpid_child_tid = sys_thread_create(waitpid_spinner_thread, nullptr, spin_top, &frame);

    int32_t status = 0;
    g_waitpid_ret = process_waitpid(g_waitpid_child_tid, &status, 0);
    free(spin_stack);
    sys_thread_exit(0);
}

// A thread blocked in waitpid must be SIGKILL-able: the fatal-signal
// escape breaks the block instead of re-parking forever. Both exec and
// exit-group wait for every member's death — an unkillable parked waiter
// hangs them. Red evidence: pre-fix the blocker only returns once its
// child actually exits, reaping it (a tid, not -1).
KTEST(waitpid_breaks_on_fatal_signal)
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

    void *block_stack = malloc(4096);
    KTEST_EXPECT(block_stack != nullptr);
    void *block_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(block_stack) + 4096);

    g_waitpid_spinner_run = true;
    g_waitpid_ret = 999; // sentinel: no return yet
    g_waitpid_child_tid = -1;

    const int64_t block_tid = sys_thread_create(waitpid_blocker_thread, nullptr, block_top, &mock_frame);
    KTEST_EXPECT(block_tid > 0);
    if (block_tid <= 0) {
        free(block_stack);
        leader->page_table = orig_page_table;
        return;
    }

    // Bounded wait for the blocker to park on its child's wait queue.
    bool parked = false;
    for (int i = 0; i < 200 && !parked; i++) {
        scheduler_yield();
        Process *b = process_find_by_pid(static_cast<uint64_t>(block_tid));
        parked = b && (b->state == ProcessState_Waiting || b->state == ProcessState_Blocked);
    }
    KTEST_EXPECT(parked);

    signal_send(process_find_by_pid(static_cast<uint64_t>(block_tid)), SIGKILL);

    // The blocker must come back instead of sleeping through the signal.
    bool returned = false;
    for (int i = 0; i < 500 && !returned; i++) {
        scheduler_yield();
        returned = g_waitpid_ret != 999;
    }

    // Release the child either way: post-fix the blocker has already
    // returned; pre-fix this is what eventually wakes it, so the red run
    // drains cleanly instead of parking the blocker forever.
    g_waitpid_spinner_run = false;
    bool drained = false;
    for (int i = 0; i < 500 && !drained; i++) {
        scheduler_yield();
        drained = process_find_by_pid(static_cast<uint64_t>(block_tid)) == nullptr &&
                  process_find_by_pid(static_cast<uint64_t>(g_waitpid_child_tid)) == nullptr;
    }
    KTEST_EXPECT(drained);

    int32_t status = 0;
    (void)process_waitpid(block_tid, &status, 0);

    free(block_stack);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;

    // Assertions last: failures here leave nothing behind.
    KTEST_EXPECT(returned);
    KTEST_EXPECT_EQ(g_waitpid_ret, static_cast<int64_t>(-1));
}
