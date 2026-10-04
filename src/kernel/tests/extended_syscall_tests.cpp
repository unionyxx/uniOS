#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/epoll.h>
#include <kernel/sync/futex.h>
#include <kernel/sync/mutex.h>
#include <kernel/syscall.h>
#include <kernel/time/timer.h>
#include <kernel/user_ptr.h>
#include <libk/kstd.h>
#include <libk/kstring.h>
#include <uapi/syscalls.h>
#include <uapi/syscalls_ext.h>

extern "C" int64_t sys_mprotect(void *addr, size_t len, int prot);
void signal_send(Process *p, int sig);

static int test_find_free_fd(Process *p)
{
    if (!p)
        return -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!p->fdtab->fds[i].used)
            return i;
    }
    return -1;
}

static void dummy_thread_entry()
{
    while (true) {
        scheduler_yield();
    }
}

static volatile uint32_t *g_test_futex_addr = nullptr;
static void futex_waiter_thread()
{
    sys_futex(g_test_futex_addr, FUTEX_WAIT, 0);
    while (true) {
        scheduler_yield();
    }
}

static volatile int64_t g_timed_futex_ret = 0;
// Park in a 10 s timed wait: a death that only arrives at the deadline means
// the fatal-signal recheck is missing from the timed park path. A prompt
// death (and a -4 return) means the wake/recheck machinery reached the
// waiter. The ktest runs single-core where a parked waiter is always woken
// by the signal's state check; the cross-core lost-wake window the recheck
// closes is exercised by construction - the timed branch now parks through
// the same scheduler_wait_rechecked as the untimed branch. The entry exits
// via sys_thread_exit, not process_exit: a member death by group-kill would
// signal the pid-0 ktest task and poison every later signal-aware wait.
static void timed_futex_waiter_thread()
{
    g_timed_futex_ret = sys_futex(g_test_futex_addr, FUTEX_WAIT, 0, 10000);
    if (scheduler_fatal_signal_pending(process_get_current()))
        sys_thread_exit(0);
    while (true) {
        scheduler_yield();
    }
}

KTEST(extended_syscalls_timed_futex_fatal_signal)
{
    Process *current = process_get_current();
    KTEST_EXPECT(current != nullptr);

    uint64_t *orig_page_table = current->page_table;
    VMA *orig_vma_list = current->vmalist->head;
    if (!current->page_table)
        current->page_table = vmm_get_kernel_pml4();

    const uint64_t test_vaddr = 0x10000000ULL;
    void *futex_phys = pmm_alloc_frame();
    KTEST_EXPECT(futex_phys != nullptr);
    Result<void> futex_map = vmm_replace_page_in(
        current->page_table, test_vaddr, reinterpret_cast<uint64_t>(futex_phys), PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(futex_map.ok());
    volatile uint32_t *uval = reinterpret_cast<volatile uint32_t *>(test_vaddr);
    KSTAC();
    *uval = 0;
    KCLAC();

    VMA *futex_vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(futex_vma != nullptr);
    futex_vma->start = test_vaddr;
    futex_vma->end = test_vaddr + 4096;
    futex_vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    futex_vma->type = VMAType::Anonymous;
    futex_vma->next = nullptr;
    current->vmalist->head = futex_vma;

    g_test_futex_addr = uval;
    g_timed_futex_ret = 0;
    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *stack_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    int64_t thread_pid = sys_thread_create(timed_futex_waiter_thread, nullptr, stack_top, &mock_frame);
    KTEST_EXPECT(thread_pid > 0);

    // Wait for the waiter to actually park in the timed futex wait.
    bool parked = false;
    for (int i = 0; i < 200 && !parked; i++) {
        scheduler_yield();
        Process *t = process_find_by_pid(static_cast<uint64_t>(thread_pid));
        parked = t && (t->state == ProcessState_Blocked || t->state == ProcessState_Waiting);
    }
    KTEST_EXPECT(parked);

    // Kill it mid-wait: the wait must report the interruption (-4) and the
    // thread must die now, not at its 10 s deadline.
    Process *waiter = process_find_by_pid(static_cast<uint64_t>(thread_pid));
    if (waiter)
        signal_send(waiter, SIGKILL);

    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        gone = process_find_by_pid(static_cast<uint64_t>(thread_pid)) == nullptr;
    }
    KTEST_EXPECT(gone);
    KTEST_EXPECT_EQ(g_timed_futex_ret, int64_t(-4)); // -EINTR

    free(stack);
    g_test_futex_addr = nullptr;
    vmm_unmap_page_in(current->page_table, test_vaddr);
    pmm_free_frame(futex_phys);
    free(futex_vma);

    current->page_table = orig_page_table;
    current->vmalist->head = orig_vma_list;
}

KTEST(extended_syscalls_futex)
{
    Process *current = process_get_current();
    KTEST_EXPECT(current != nullptr);

    uint64_t *orig_page_table = current->page_table;
    VMA *orig_vma_list = current->vmalist->head;

    if (!current->page_table)
        current->page_table = vmm_get_kernel_pml4();

    // sys_futex validates and reads through user VMAs now: map a real user
    // page for the futex word and install its VMA.
    const uint64_t test_vaddr = 0x10000000ULL;
    void *futex_phys = pmm_alloc_frame();
    KTEST_EXPECT(futex_phys != nullptr);
    Result<void> futex_map = vmm_replace_page_in(
        current->page_table, test_vaddr, reinterpret_cast<uint64_t>(futex_phys), PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(futex_map.ok());
    volatile uint32_t *uval = reinterpret_cast<volatile uint32_t *>(test_vaddr);
    KSTAC();
    *uval = 42;
    KCLAC();

    VMA *futex_vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(futex_vma != nullptr);
    futex_vma->start = test_vaddr;
    futex_vma->end = test_vaddr + 4096;
    futex_vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    futex_vma->type = VMAType::Anonymous;
    futex_vma->next = nullptr;
    current->vmalist->head = futex_vma;

    volatile uint32_t val = 42;

    volatile uint32_t *unaligned_uaddr = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(uval) | 1);
    int64_t res = sys_futex(unaligned_uaddr, FUTEX_WAIT, 42);
    KTEST_EXPECT_EQ(res, -22); // -EINVAL

    res = sys_futex(nullptr, FUTEX_WAIT, 42);
    KTEST_EXPECT_EQ(res, -14); // -EFAULT

    // kernel-space addresses are rejected outright
    res = sys_futex(&val, FUTEX_WAIT, 42);
    KTEST_EXPECT_EQ(res, -14); // -EFAULT

    res = sys_futex(uval, FUTEX_WAIT, 100);
    KTEST_EXPECT_EQ(res, -11); // -EAGAIN (val != expected)

    res = sys_futex(uval, FUTEX_WAKE, 1);
    KTEST_EXPECT_EQ(res, 0); // nobody waiting

    // Test actual blocking and waking to ensure the futex lock is correctly released
    KSTAC();
    *uval = 0;
    KCLAC();
    g_test_futex_addr = uval;
    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *stack_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    int64_t thread_pid = sys_thread_create(futex_waiter_thread, nullptr, stack_top, &mock_frame);
    KTEST_EXPECT(thread_pid > 0);

    // Yield to allow the waiter thread to run and block
    for (int i = 0; i < 5; i++) {
        scheduler_yield();
    }

    // A count of 0 is literal, POSIX-style: it wakes no waiter at all.
    {
        int64_t woken0 = sys_futex(uval, FUTEX_WAKE, 0);
        KTEST_EXPECT_EQ(woken0, 0);
        Process *waiter = process_find_by_pid(static_cast<uint64_t>(thread_pid));
        KTEST_EXPECT(waiter != nullptr);
        KTEST_EXPECT(waiter->state == ProcessState_Blocked || waiter->state == ProcessState_Waiting);
    }

    // Wake the waiter thread
    int64_t woken = sys_futex(uval, FUTEX_WAKE, 1);
    KTEST_EXPECT_EQ(woken, 1);

    // Yield to let the waiter thread resume
    for (int i = 0; i < 5; i++) {
        scheduler_yield();
    }

    // Call WAKE again to verify we do not deadlock on bucket->lock
    int64_t woken2 = sys_futex(uval, FUTEX_WAKE, 1);
    KTEST_EXPECT_EQ(woken2, 0);

    // Reap the child thread
    Process *child = process_find_by_pid(static_cast<uint64_t>(thread_pid));
    KTEST_EXPECT(child != nullptr);
    scheduler_remove_from_ready_queue(child);
    child->state = ProcessState_Zombie;
    // The thread never exits through SYS_THREAD_EXIT, so its TLS mapping
    // must leave by hand: the head restore below would drop the VMA node
    // while the PTE stays mapped, poisoning every later first-fit.
    if (child->tls_lo != 0) {
        (void)munmap_process_range(child, child->tls_lo, child->tls_len);
        child->tls_lo = 0;
        child->tls_len = 0;
    }
    // Sever the child's references (not the shared list itself) so the
    // deferred reaper never frees them out from under the test's own
    // cleanup below.
    child->vmalist = nullptr;
    child->page_table = nullptr;

    int32_t status = 0;
    int64_t reaped_pid = process_waitpid(thread_pid, &status, 0);
    KTEST_EXPECT_EQ(reaped_pid, thread_pid);

    free(stack);
    g_test_futex_addr = nullptr;
    vmm_unmap_page_in(current->page_table, test_vaddr);
    pmm_free_frame(futex_phys);
    free(futex_vma);

    current->page_table = orig_page_table;
    current->vmalist->head = orig_vma_list;
}

KTEST(extended_syscalls_thread_create)
{
    Process *parent = process_get_current();
    KTEST_EXPECT(parent != nullptr);

    uint64_t *orig_page_table = parent->page_table;
    if (!parent->page_table)
        parent->page_table = vmm_get_kernel_pml4();

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    void *stack_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    int64_t thread_pid = sys_thread_create(dummy_thread_entry, nullptr, stack_top, &mock_frame);
    KTEST_EXPECT(thread_pid > 0);

    Process *child = process_find_by_pid(static_cast<uint64_t>(thread_pid));
    KTEST_EXPECT(child != nullptr);
    KTEST_EXPECT_EQ(child->parent_pid, parent->pid);
    KTEST_EXPECT_EQ(child->state, ProcessState_Ready);

    // pull it out of the run queues before the scheduler ever touches it
    scheduler_remove_from_ready_queue(child);
    child->state = ProcessState_Zombie;
    // The thread never runs or exits: its TLS mapping (installed at
    // create) must leave by hand or the node-PTE pair outlives the test.
    if (child->tls_lo != 0) {
        (void)munmap_process_range(child, child->tls_lo, child->tls_len);
        child->tls_lo = 0;
        child->tls_len = 0;
    }

    int32_t status = 0;
    int64_t reaped_pid = process_waitpid(thread_pid, &status, 0);
    KTEST_EXPECT_EQ(reaped_pid, thread_pid);

    free(stack);
    parent->page_table = orig_page_table;
}

KTEST(extended_syscalls_mprotect)
{
    Process *current = process_get_current();
    KTEST_EXPECT(current != nullptr);

    uint64_t *orig_page_table = current->page_table;
    VMA *orig_vma_list = current->vmalist->head;

    if (!current->page_table)
        current->page_table = vmm_get_kernel_pml4();

    uint64_t test_vaddr = 0x10000000ULL;
    void *phys = pmm_alloc_frame();
    KTEST_EXPECT(phys != nullptr);

    // The bootloader identity-maps low RAM in the kernel PML4, so this slot is
    // already present: replace it deliberately (this is the mprotect test, not
    // the fresh-map path).
    Result<void> map_res =
        vmm_replace_page_in(current->page_table, test_vaddr, reinterpret_cast<uint64_t>(phys), PTE_PRESENT | PTE_USER);
    KTEST_EXPECT(map_res.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = test_vaddr;
    vma->end = test_vaddr + 4096;
    vma->flags = PTE_PRESENT | PTE_USER;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;

    current->vmalist->head = vma;

    // unaligned addr
    int64_t res = sys_mprotect(reinterpret_cast<void *>(test_vaddr | 1), 4096, PROT_READ | PROT_WRITE);
    KTEST_EXPECT_EQ(res, -22);

    // unmapped region
    res = sys_mprotect(reinterpret_cast<void *>(0x20000000ULL), 4096, PROT_READ | PROT_WRITE);
    KTEST_EXPECT_EQ(res, -12);

    // write-enable
    res = sys_mprotect(reinterpret_cast<void *>(test_vaddr), 4096, PROT_READ | PROT_WRITE);
    KTEST_EXPECT_EQ(res, 0);
    KTEST_EXPECT((vma->flags & PTE_WRITABLE) != 0);
    uint64_t current_flags = vmm_get_page_flags_in(current->page_table, test_vaddr);
    KTEST_EXPECT((current_flags & PTE_WRITABLE) != 0);

    // execute-enable (NX cleared)
    res = sys_mprotect(reinterpret_cast<void *>(test_vaddr), 4096, PROT_READ | PROT_EXEC);
    KTEST_EXPECT_EQ(res, 0);
    current_flags = vmm_get_page_flags_in(current->page_table, test_vaddr);
    KTEST_EXPECT((current_flags & PTE_NX) == 0);

    vmm_unmap_page_in(current->page_table, test_vaddr);
    pmm_free_frame(phys);
    free(vma);

    current->page_table = orig_page_table;
    current->vmalist->head = orig_vma_list;
}

KTEST(extended_syscalls_epoll)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    // sys_epoll_ctl/wait validate their user pointers now, so the test needs
    // a real user mapping for the epoll_event structures.
    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;

    if (!p->page_table)
        p->page_table = vmm_get_kernel_pml4();

    uint64_t test_vaddr = 0x10000000ULL;
    void *phys = pmm_alloc_frame();
    KTEST_EXPECT(phys != nullptr);
    Result<void> map_res = vmm_replace_page_in(p->page_table, test_vaddr, reinterpret_cast<uint64_t>(phys),
                                               PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map_res.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = test_vaddr;
    vma->end = test_vaddr + 4096;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;

    p->vmalist->head = vma;

    struct epoll_event *user_ev = reinterpret_cast<struct epoll_event *>(test_vaddr);
    struct epoll_event *user_events = reinterpret_cast<struct epoll_event *>(test_vaddr + 64);

    int64_t epfd = sys_epoll_create(0);
    KTEST_EXPECT_EQ(epfd, -22); // size <= 0

    epfd = sys_epoll_create(10);
    KTEST_EXPECT(epfd >= 3);

    int pipe_id = pipe_create();
    KTEST_EXPECT(pipe_id >= 0);

    int read_fd = test_find_free_fd(p);
    KTEST_EXPECT(read_fd >= 0);
    p->fdtab->fds[read_fd].used = true;
    p->fdtab->fds[read_fd].vnode = pipe_get_vnode(pipe_id, false);
    p->fdtab->fds[read_fd].flags = 0;

    int write_fd = test_find_free_fd(p);
    KTEST_EXPECT(write_fd >= 0);
    p->fdtab->fds[write_fd].used = true;
    p->fdtab->fds[write_fd].vnode = pipe_get_vnode(pipe_id, true);
    p->fdtab->fds[write_fd].flags = 0;

    KSTAC();
    user_ev->events = EPOLLIN;
    user_ev->data.fd = read_fd;
    KCLAC();
    int64_t res = sys_epoll_ctl(static_cast<int>(epfd), EPOLL_CTL_ADD, read_fd, user_ev);
    KTEST_EXPECT_EQ(res, 0);

    // duplicate add
    res = sys_epoll_ctl(static_cast<int>(epfd), EPOLL_CTL_ADD, read_fd, user_ev);
    KTEST_EXPECT_EQ(res, -17); // -EEXIST

    // kernel pointers are rejected by validation
    struct epoll_event kernel_ev = {};
    kernel_ev.events = EPOLLIN;
    res = sys_epoll_ctl(static_cast<int>(epfd), EPOLL_CTL_ADD, write_fd, &kernel_ev);
    KTEST_EXPECT_EQ(res, -14); // -EFAULT

    res = sys_epoll_wait(static_cast<int>(epfd), user_events, 2, 0);
    KTEST_EXPECT_EQ(res, 0); // pipe empty

    int64_t written = pipe_write(pipe_id, "test", 4);
    KTEST_EXPECT_EQ(written, 4);

    res = sys_epoll_wait(static_cast<int>(epfd), user_events, 2, 0);
    KTEST_EXPECT_EQ(res, 1);
    int32_t ev_fd = 0;
    uint32_t ev_events = 0;
    KSTAC();
    ev_fd = user_events[0].data.fd;
    ev_events = user_events[0].events;
    KCLAC();
    KTEST_EXPECT_EQ(ev_fd, read_fd);
    KTEST_EXPECT((ev_events & EPOLLIN) != 0);

    char buf[4];
    int64_t read_bytes = pipe_read(pipe_id, buf, 4);
    KTEST_EXPECT_EQ(read_bytes, 4);

    res = sys_epoll_wait(static_cast<int>(epfd), user_events, 2, 0);
    KTEST_EXPECT_EQ(res, 0); // pipe drained

    vfs_close(read_fd);
    vfs_close(write_fd);
    vfs_close(static_cast<int>(epfd));

    vmm_unmap_page_in(p->page_table, test_vaddr);
    pmm_free_frame(phys);
    free(vma);

    p->page_table = orig_page_table;
    p->vmalist->head = orig_vma_list;
}

// Writer thread for the epoll wake test: yields for a while, then writes to
// the pipe so a sleeper in sys_epoll_wait must be woken by a real producer.
static void epoll_writer_thread(void *arg)
{
    int pipe_id = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
    for (int i = 0; i < 20; i++)
        scheduler_yield();
    pipe_write(pipe_id, "WAKE", 4);
    while (true)
        scheduler_yield();
}

// Common setup for the blocking epoll tests: maps a user page for the events
// array, creates an epoll instance and a registered pipe read end.
struct EpollBlockFixture
{
    VMA *vma;
    void *phys;
    int64_t epfd;
    int pipe_id;
    int read_fd;
    struct epoll_event *user_ev;
    struct epoll_event *user_events;
};

static bool epoll_block_fixture_setup(Process *p, EpollBlockFixture &f)
{
    const uint64_t test_vaddr = 0x10000000ULL;
    f.vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    f.phys = pmm_alloc_frame();
    if (!f.vma || !f.phys)
        return false;
    if (!vmm_replace_page_in(p->page_table, test_vaddr, reinterpret_cast<uint64_t>(f.phys),
                             PTE_PRESENT | PTE_USER | PTE_WRITABLE)
             .ok())
        return false;
    f.vma->start = test_vaddr;
    f.vma->end = test_vaddr + 4096;
    f.vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    f.vma->type = VMAType::Anonymous;
    f.vma->next = nullptr;
    p->vmalist->head = f.vma;

    f.user_ev = reinterpret_cast<struct epoll_event *>(test_vaddr);
    f.user_events = f.user_ev;

    f.epfd = sys_epoll_create(4);
    f.pipe_id = pipe_create();
    f.read_fd = test_find_free_fd(p);
    if (f.epfd < 3 || f.pipe_id < 0 || f.read_fd < 0)
        return false;

    p->fdtab->fds[f.read_fd].used = true;
    p->fdtab->fds[f.read_fd].vnode = pipe_get_vnode(f.pipe_id, false);
    p->fdtab->fds[f.read_fd].flags = 0;

    KSTAC();
    f.user_ev->events = EPOLLIN;
    f.user_ev->data.fd = f.read_fd;
    KCLAC();
    return sys_epoll_ctl(static_cast<int>(f.epfd), EPOLL_CTL_ADD, f.read_fd, f.user_ev) == 0;
}

static void epoll_block_fixture_teardown(Process *p, EpollBlockFixture &f, uint64_t *orig_page_table,
                                         VMA *orig_vma_list)
{
    vfs_close(f.read_fd);
    vfs_close(static_cast<int>(f.epfd));
    pipe_close_write(f.pipe_id);
    pipe_close_read(f.pipe_id);
    vmm_unmap_page_in(p->page_table, 0x10000000ULL);
    pmm_free_frame(f.phys);
    free(f.vma);
    p->page_table = orig_page_table;
    p->vmalist->head = orig_vma_list;
}

KTEST(extended_syscalls_epoll_timeout)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;
    if (!p->page_table)
        p->page_table = vmm_get_kernel_pml4();

    EpollBlockFixture f = {};
    KTEST_EXPECT(epoll_block_fixture_setup(p, f));
    if (!f.vma)
        return;

    // Blocking wait with a timeout on a pipe nobody writes to: the timeout
    // must actually fire (nothing else wakes the epoll queue on a quiet
    // system, so only the tick-driven deadline returns the sleeper).
    const uint64_t start = timer_get_ticks();
    int64_t r = sys_epoll_wait(static_cast<int>(f.epfd), f.user_events, 2, 300);
    const uint64_t elapsed = timer_get_ticks() - start;
    KTEST_EXPECT_EQ(r, 0);
    KTEST_EXPECT(elapsed >= 200); // ~300ms budget with a generous margin

    epoll_block_fixture_teardown(p, f, orig_page_table, orig_vma_list);
}

KTEST(extended_syscalls_epoll_wake)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;
    if (!p->page_table)
        p->page_table = vmm_get_kernel_pml4();

    EpollBlockFixture f = {};
    KTEST_EXPECT(epoll_block_fixture_setup(p, f));
    if (!f.vma)
        return;

    void *stack = malloc(4096);
    KTEST_EXPECT(stack != nullptr);
    if (!stack) {
        epoll_block_fixture_teardown(p, f, orig_page_table, orig_vma_list);
        return;
    }
    void *stack_top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + 4096);

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    int64_t thread_pid =
        sys_thread_create(reinterpret_cast<void (*)()>(epoll_writer_thread),
                          reinterpret_cast<void *>(static_cast<uintptr_t>(f.pipe_id)), stack_top, &mock_frame);
    KTEST_EXPECT(thread_pid > 0);

    // A writer on another thread must wake the sleeping epoll_wait through
    // pipe_write -> scheduler_wake_all -> the epoll queue nudge: exactly the
    // lost-wakeup window the queued recheck in scheduler_wait_rechecked
    // closes. If the wake were lost this would sit for the full 5 s.
    int64_t r = sys_epoll_wait(static_cast<int>(f.epfd), f.user_events, 2, 5000);
    KTEST_EXPECT_EQ(r, 1);
    if (r == 1) {
        int32_t ev_fd = 0;
        uint32_t ev_events = 0;
        KSTAC();
        ev_fd = f.user_events[0].data.fd;
        ev_events = f.user_events[0].events;
        KCLAC();
        KTEST_EXPECT_EQ(ev_fd, f.read_fd);
        KTEST_EXPECT((ev_events & EPOLLIN) != 0);
    }

    // Reap the writer thread: it shares the test's throwaway VMA list and
    // page table, so detach both before the reaper can free them.
    Process *child = process_find_by_pid(static_cast<uint64_t>(thread_pid));
    KTEST_EXPECT(child != nullptr);
    if (child) {
        scheduler_remove_from_ready_queue(child);
        child->state = ProcessState_Zombie;
        // The writer never exits through SYS_THREAD_EXIT and never touches
        // user memory again: drain its TLS mapping by hand, then sever its
        // references (not the shared list itself).
        if (child->tls_lo != 0) {
            (void)munmap_process_range(child, child->tls_lo, child->tls_len);
            child->tls_lo = 0;
            child->tls_len = 0;
        }
        child->vmalist = nullptr;
        child->page_table = nullptr;
        int32_t status = 0;
        int64_t reaped = process_waitpid(thread_pid, &status, 0);
        KTEST_EXPECT_EQ(reaped, thread_pid);
    }

    free(stack);
    epoll_block_fixture_teardown(p, f, orig_page_table, orig_vma_list);
}

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

extern "C" int64_t sys_memfd_create(const char *name, unsigned int flags);

KTEST(extended_syscalls_memfd)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;

    if (!p->page_table) {
        p->page_table = vmm_get_kernel_pml4();
    }

    int64_t fd = sys_memfd_create(nullptr, 0);
    KTEST_EXPECT(fd >= 3);

    const char *test_str = "Hello Memfd!";
    uint64_t test_len = 12;
    int64_t written = vfs_write(static_cast<int>(fd), test_str, test_len);
    KTEST_EXPECT_EQ(written, static_cast<int64_t>(test_len));

    int64_t seek_res = vfs_seek(static_cast<int>(fd), 0, SEEK_SET);
    KTEST_EXPECT_EQ(seek_res, 0);

    char read_buf[32] = {};
    int64_t bytes_read = vfs_read(static_cast<int>(fd), read_buf, test_len);
    KTEST_EXPECT_EQ(bytes_read, static_cast<int64_t>(test_len));
    KTEST_EXPECT(kstring::strcmp(read_buf, test_str) == 0);

    SyscallFrame frame = {};
    frame.arg4 = MAP_SHARED;
    frame.arg5 = static_cast<uint64_t>(fd);

    uint64_t mmap_res = syscall_handler(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, &frame);
    KTEST_EXPECT(mmap_res != static_cast<uint64_t>(-1));

    volatile char *shared_ptr = reinterpret_cast<volatile char *>(mmap_res);
    int shared_initial = 1;
    KSTAC();
    shared_initial = kstring::strcmp(const_cast<char *>(shared_ptr), test_str);
    shared_ptr[0] = 'y';
    shared_ptr[1] = 'o';
    shared_ptr[2] = 'u';
    KCLAC();
    KTEST_EXPECT_EQ(shared_initial, 0);

    seek_res = vfs_seek(static_cast<int>(fd), 0, SEEK_SET);
    KTEST_EXPECT_EQ(seek_res, 0);

    kstring::zero_memory(read_buf, sizeof(read_buf));
    bytes_read = vfs_read(static_cast<int>(fd), read_buf, test_len);
    KTEST_EXPECT_EQ(bytes_read, static_cast<int64_t>(test_len));
    KTEST_EXPECT(kstring::strcmp(read_buf, "youlo Memfd!") == 0);

    frame.arg4 = MAP_PRIVATE;
    uint64_t p_mmap_res = syscall_handler(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, &frame);
    KTEST_EXPECT(p_mmap_res != static_cast<uint64_t>(-1));
    KTEST_EXPECT(p_mmap_res != mmap_res);

    volatile char *private_ptr = reinterpret_cast<volatile char *>(p_mmap_res);
    int private_initial = 1;
    int shared_after = 1;
    KSTAC();
    private_initial = kstring::strcmp(const_cast<char *>(private_ptr), "youlo Memfd!");
    private_ptr[0] = 'H';
    private_ptr[1] = 'e';
    private_ptr[2] = 'l';
    shared_after = kstring::strcmp(const_cast<char *>(shared_ptr), "youlo Memfd!");
    KCLAC();
    KTEST_EXPECT_EQ(private_initial, 0);
    KTEST_EXPECT_EQ(shared_after, 0);

    seek_res = vfs_seek(static_cast<int>(fd), 0, SEEK_SET);
    KTEST_EXPECT_EQ(seek_res, 0);
    kstring::zero_memory(read_buf, sizeof(read_buf));
    bytes_read = vfs_read(static_cast<int>(fd), read_buf, test_len);
    KTEST_EXPECT(kstring::strcmp(read_buf, "youlo Memfd!") == 0);

    int64_t munmap_res = syscall_handler(SYS_MUNMAP, mmap_res, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    munmap_res = syscall_handler(SYS_MUNMAP, p_mmap_res, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    int close_res = vfs_close(static_cast<int>(fd));
    KTEST_EXPECT_EQ(close_res, 0);

    p->page_table = orig_page_table;
    p->vmalist->head = orig_vma_list;
}

extern "C" int64_t sys_ftruncate(int fd, uint64_t size);
extern "C" int64_t sys_fd_transfer(uint64_t target_pid, int fd);

KTEST(extended_syscalls_fd_transfer)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    int64_t fd = sys_memfd_create(nullptr, 0);
    KTEST_EXPECT(fd >= 3);

    // Test ftruncate
    int64_t trunc_res = sys_ftruncate(static_cast<int>(fd), 8192);
    KTEST_EXPECT_EQ(trunc_res, 0);

    VNode *node = p->fdtab->fds[fd].vnode;
    KTEST_EXPECT(node != nullptr);
    KTEST_EXPECT_EQ(node->size, 8192ULL);

    // Test fd_transfer (transfer to self as target_pid)
    int64_t transferred_fd = sys_fd_transfer(p->pid, static_cast<int>(fd));
    KTEST_EXPECT(transferred_fd >= 3);
    KTEST_EXPECT(transferred_fd != fd);
    KTEST_EXPECT(p->fdtab->fds[transferred_fd].used);
    KTEST_EXPECT_EQ(p->fdtab->fds[transferred_fd].vnode, node);

    // Clean up both FDs
    int close_res1 = vfs_close(static_cast<int>(fd));
    KTEST_EXPECT_EQ(close_res1, 0);

    int close_res2 = vfs_close(static_cast<int>(transferred_fd));
    KTEST_EXPECT_EQ(close_res2, 0);
}

KTEST(extended_syscalls_vma_split_unmap)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;

    if (!p->page_table) {
        p->page_table = vmm_get_kernel_pml4();
    }

    int64_t fd = sys_memfd_create(nullptr, 0);
    KTEST_EXPECT(fd >= 3);

    int64_t trunc_res = sys_ftruncate(static_cast<int>(fd), 12288);
    KTEST_EXPECT_EQ(trunc_res, 0);

    SyscallFrame frame = {};
    frame.arg4 = MAP_SHARED;
    frame.arg5 = static_cast<uint64_t>(fd);

    // Map 3 pages
    uint64_t mmap_res = syscall_handler(SYS_MMAP, 0, 12288, PROT_READ | PROT_WRITE, &frame);
    KTEST_EXPECT(mmap_res != static_cast<uint64_t>(-1));

    // Write to all 3 pages
    volatile char *ptr = reinterpret_cast<volatile char *>(mmap_res);
    KSTAC();
    ptr[0] = 'a';
    ptr[4096] = 'b';
    ptr[8192] = 'c';
    KCLAC();

    // Unmap the middle page (offset 4096, length 4096)
    int64_t munmap_res = syscall_handler(SYS_MUNMAP, mmap_res + 4096, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    // First page should still be present
    uint64_t phys0 = vmm_virt_to_phys_in(p->page_table, mmap_res);
    KTEST_EXPECT(phys0 != 0);
    char first = 0;
    KSTAC();
    first = ptr[0];
    KCLAC();
    KTEST_EXPECT_EQ(first, 'a');

    // Middle page should be unmapped
    uint64_t phys1 = vmm_virt_to_phys_in(p->page_table, mmap_res + 4096);
    KTEST_EXPECT_EQ(phys1, 0);

    // Third page should still be present
    uint64_t phys2 = vmm_virt_to_phys_in(p->page_table, mmap_res + 8192);
    KTEST_EXPECT(phys2 != 0);
    char third = 0;
    KSTAC();
    third = ptr[8192];
    KCLAC();
    KTEST_EXPECT_EQ(third, 'c');

    // Clean up: unmap first and third pages
    munmap_res = syscall_handler(SYS_MUNMAP, mmap_res, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    munmap_res = syscall_handler(SYS_MUNMAP, mmap_res + 8192, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    int close_res = vfs_close(static_cast<int>(fd));
    KTEST_EXPECT_EQ(close_res, 0);

    p->page_table = orig_page_table;
    p->vmalist->head = orig_vma_list;
}

KTEST(extended_syscalls_mmap_offset)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;

    if (!p->page_table) {
        p->page_table = vmm_get_kernel_pml4();
    }

    int64_t fd = sys_memfd_create(nullptr, 0);
    KTEST_EXPECT(fd >= 3);

    // Truncate memfd to 3 pages
    int64_t trunc_res = sys_ftruncate(static_cast<int>(fd), 12288);
    KTEST_EXPECT_EQ(trunc_res, 0);

    // Eagerly write to the 3 pages via virtual space
    SyscallFrame frame = {};
    frame.arg4 = MAP_SHARED;
    frame.arg5 = static_cast<uint64_t>(fd);
    frame.arg6 = 0; // offset 0

    uint64_t mmap_full = syscall_handler(SYS_MMAP, 0, 12288, PROT_READ | PROT_WRITE, &frame);
    KTEST_EXPECT(mmap_full != static_cast<uint64_t>(-1));

    volatile char *full_ptr = reinterpret_cast<volatile char *>(mmap_full);
    KSTAC();
    full_ptr[0] = 'X';
    full_ptr[4096] = 'Y';
    full_ptr[8192] = 'Z';
    KCLAC();

    // Unmap the initial full mapping
    int64_t munmap_res = syscall_handler(SYS_MUNMAP, mmap_full, 12288, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    // Test unaligned offset (should fail with -1)
    frame.arg6 = 1000; // unaligned
    uint64_t mmap_failed = syscall_handler(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, &frame);
    KTEST_EXPECT_EQ(mmap_failed, static_cast<uint64_t>(-1));

    // Map offset-based: starts at offset 4096 (page 1), length 8192 (2 pages)
    frame.arg6 = 4096; // page-aligned offset
    uint64_t mmap_offset = syscall_handler(SYS_MMAP, 0, 8192, PROT_READ | PROT_WRITE, &frame);
    KTEST_EXPECT(mmap_offset != static_cast<uint64_t>(-1));

    volatile char *offset_ptr = reinterpret_cast<volatile char *>(mmap_offset);
    char page1 = 0;
    char page2 = 0;
    KSTAC();
    page1 = offset_ptr[0];    // page 1 content
    page2 = offset_ptr[4096]; // page 2 content
    KCLAC();
    KTEST_EXPECT_EQ(page1, 'Y');
    KTEST_EXPECT_EQ(page2, 'Z');

    // Clean up
    munmap_res = syscall_handler(SYS_MUNMAP, mmap_offset, 8192, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    int close_res = vfs_close(static_cast<int>(fd));
    KTEST_EXPECT_EQ(close_res, 0);

    p->page_table = orig_page_table;
    p->vmalist->head = orig_vma_list;
}

struct TestStackFrame
{
    uint64_t original_rax;
    SyscallFrame frame;
};

struct alignas(64) TestSignalContext
{
    InterruptFrame frame;
    // Must mirror the kernel's SignalContext (FPU_STATE_SIZE).
    alignas(64) uint8_t fpu_state[FPU_STATE_SIZE];
    uint64_t old_mask;
    uint32_t magic;
};

constexpr uint32_t TEST_SIG_CONTEXT_MAGIC = 0x51644374; // 'SigC'

extern "C" void signal_check_interrupt(InterruptFrame *frame);
bool g_in_ktest_signal = false;

// A failed expectation jumps to the cleanup label instead of returning
// mid-test: the surgery this test performs (page table, VMA list, signal
// handlers, a user mapping) must be undone even on failure, or the next
// ktest inherits a poisoned pid-0 task — a stale SIGUSR1 handler pointing
// into nowhere, which a later signal delivery follows straight into
// process_exit.
#define SIG_CTX_CHECK(cond)                                                                                            \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ktest_record_failure(#cond, __FILE__, __LINE__);                                                           \
            goto cleanup;                                                                                              \
        }                                                                                                              \
    } while (0)
#define SIG_CTX_CHECK_EQ(a, b) SIG_CTX_CHECK((a) == (b))

KTEST(extended_syscalls_signal_context)
{
    Process *p = process_get_current();
    if (!p) {
        ktest_record_failure("p != nullptr", __FILE__, __LINE__);
        return;
    }

    const uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vmalist->head;
    const SignalControl orig_signals = p->signals;
    uint64_t mmap_res = static_cast<uint64_t>(-1);
    SyscallFrame mmap_frame = {};
    mmap_frame.arg4 = MAP_PRIVATE | MAP_ANONYMOUS;
    mmap_frame.arg5 = static_cast<uint64_t>(-1);
    mmap_frame.arg6 = 0;

    if (!p->page_table) {
        p->page_table = vmm_get_kernel_pml4();
    }

    // Allocate user memory to use as the user stack. The kernel signal frame
    // carries the full FPU_STATE_SIZE xsave area now, so one page is not
    // enough for the frame + red zone.
    {
        mmap_res = syscall_handler(SYS_MMAP, 0, 12288, PROT_READ | PROT_WRITE, &mmap_frame);
        SIG_CTX_CHECK(mmap_res != static_cast<uint64_t>(-1));

        // Set up signal handler and restorer
        p->signals.handlers[SIGUSR1] = reinterpret_cast<sighandler_t>(0x123456ULL);
        p->signals.restorer = 0x7890ULL;
        p->signals.pending = (1ULL << SIGUSR1);
        p->signals.blocked = 0x112233ULL;

        // Set up mock register state
        TestStackFrame tf = {};
        tf.original_rax = 0xAAABBBULL;
        tf.frame.rip = 0x9999ULL;
        tf.frame.rsp = mmap_res + 12288; // Top of the mapped region
        tf.frame.cs = 0x23ULL;
        tf.frame.ss = 0x1BULL;
        tf.frame.rflags = 0x202ULL;
        tf.frame.rbx = 0x11ULL;
        tf.frame.rbp = 0x22ULL;
        tf.frame.r12 = 0x33ULL;
        tf.frame.r13 = 0x44ULL;
        tf.frame.r14 = 0x55ULL;
        tf.frame.r15 = 0x66ULL;

        // Run signal check (this should deliver SIGUSR1)
        signal_check(&tf.frame);

        // Verify signal check side effects:
        // RIP should point to the signal handler
        SIG_CTX_CHECK_EQ(tf.frame.rip, 0x123456ULL);
        // RSP should have decreased
        SIG_CTX_CHECK(tf.frame.rsp < mmap_res + 12288);
        // The signal should no longer be pending
        SIG_CTX_CHECK_EQ(p->signals.pending & (1ULL << SIGUSR1), 0ULL);

        // Verify the data pushed to the user stack. Read through the
        // virtual mapping: the mapping's pages are independently allocated
        // frames, so reconstructing field addresses from one translated
        // physical base would assume the mapping is physically contiguous.
        // The ktest runs in kernel context on the mapping's own page tables
        // (page_table surgery above), so a direct read sees exactly what
        // the interrupted user thread would see — a raw user-page access,
        // guarded like every other test-side one. The trampoline is pushed
        // at RSP; the SignalContext starts at RSP + 8.
        uint64_t tramp_phys = vmm_virt_to_phys(tf.frame.rsp);
        SIG_CTX_CHECK(tramp_phys != 0);
        uint64_t ctx_phys = vmm_virt_to_phys(tf.frame.rsp + 8);
        SIG_CTX_CHECK(ctx_phys != 0);

        uint64_t tramp_val = 0;
        TestSignalContext *u_ctx = reinterpret_cast<TestSignalContext *>(tf.frame.rsp + 8);
        uint64_t u_rax = 0, u_old_mask = 0, u_magic = 0;
        KSTAC();
        tramp_val = *reinterpret_cast<volatile uint64_t *>(tf.frame.rsp);
        u_rax = u_ctx->frame.rax;
        u_old_mask = u_ctx->old_mask;
        u_magic = u_ctx->magic;
        KCLAC();
        SIG_CTX_CHECK_EQ(tramp_val, 0x7890ULL);
        SIG_CTX_CHECK_EQ(u_rax, 0xAAABBBULL);
        SIG_CTX_CHECK_EQ(u_old_mask, 0x112233ULL);
        SIG_CTX_CHECK_EQ(u_magic, TEST_SIG_CONTEXT_MAGIC);

        // Now simulate userspace returning from the signal handler:
        // The trampoline would execute SYS_SIGRETURN.
        // The user stack pointer would point to the SignalContext (i.e. tramp address is popped)
        tf.frame.rsp += 8;

        // Set g_in_ktest_signal to true to prevent sys_sigreturn from executing iretq and crashing
        g_in_ktest_signal = true;
        uint64_t returned_rax = syscall_handler(SYS_SIGRETURN, 0, 0, 0, &tf.frame);
        g_in_ktest_signal = false;

        // Verify context restoration:
        // Returned value should be the original RAX (restored into RAX in InterruptFrame)
        SIG_CTX_CHECK_EQ(returned_rax, 0xAAABBBULL);
        // RIP and RSP should be restored
        SIG_CTX_CHECK_EQ(tf.frame.rip, 0x9999ULL);
        SIG_CTX_CHECK_EQ(tf.frame.rsp, mmap_res + 12288);
        // Callee-saved registers should be restored
        SIG_CTX_CHECK_EQ(tf.frame.rbx, 0x11ULL);
        SIG_CTX_CHECK_EQ(tf.frame.rbp, 0x22ULL);
        SIG_CTX_CHECK_EQ(tf.frame.r12, 0x33ULL);
        SIG_CTX_CHECK_EQ(tf.frame.r13, 0x44ULL);
        SIG_CTX_CHECK_EQ(tf.frame.r14, 0x55ULL);
        SIG_CTX_CHECK_EQ(tf.frame.r15, 0x66ULL);
        // Signal mask should be restored
        SIG_CTX_CHECK_EQ(p->signals.blocked, 0x112233ULL);

        // --- Test signal_check_interrupt ---
        p->signals.pending = (1ULL << SIGUSR1);

        InterruptFrame int_frame = {};
        int_frame.rip = 0xaaaaULL;
        int_frame.rsp = mmap_res + 12288;
        int_frame.cs = 0x23ULL; // Ring 3
        int_frame.ss = 0x1BULL;
        int_frame.rflags = 0x202ULL;
        int_frame.rax = 0x5555ULL;

        signal_check_interrupt(&int_frame);

        // Verify it delivered the signal
        SIG_CTX_CHECK_EQ(int_frame.rip, 0x123456ULL);
        SIG_CTX_CHECK_EQ(p->signals.pending & (1ULL << SIGUSR1), 0ULL);
    }

cleanup:
    g_in_ktest_signal = false;
    if (mmap_res != static_cast<uint64_t>(-1)) {
        (void)syscall_handler(SYS_MUNMAP, mmap_res, 12288, 0, &mmap_frame);
    }
    p->signals = orig_signals;
    p->page_table = const_cast<uint64_t *>(orig_page_table);
    p->vmalist->head = orig_vma_list;
}

#undef SIG_CTX_CHECK
#undef SIG_CTX_CHECK_EQ

KTEST(extended_vfs_page_cache)
{
    // Create three files of size 200 * 4096 = 819,200 bytes on UniFS
    int fd1 = vfs_open("/file1.txt", O_CREAT | O_RDWR);
    int fd2 = vfs_open("/file2.txt", O_CREAT | O_RDWR);
    int fd3 = vfs_open("/file3.txt", O_CREAT | O_RDWR);

    KTEST_EXPECT(fd1 >= 3);
    KTEST_EXPECT(fd2 >= 3);
    KTEST_EXPECT(fd3 >= 3);

    // Allocate buffer
    uint8_t *buf = static_cast<uint8_t *>(malloc(4096));
    KTEST_EXPECT(buf != nullptr);

    // Fill buffer with some recognizable pattern
    for (int i = 0; i < 4096; i++) {
        buf[i] = static_cast<uint8_t>(i % 256);
    }

    // Write 200 pages to file1.txt
    for (int i = 0; i < 200; i++) {
        int64_t written = vfs_write(fd1, buf, 4096);
        KTEST_EXPECT_EQ(written, 4096);
    }

    // Write 200 pages to file2.txt
    for (int i = 0; i < 200; i++) {
        int64_t written = vfs_write(fd2, buf, 4096);
        KTEST_EXPECT_EQ(written, 4096);
    }

    // Write 200 pages to file3.txt
    // This will exceed the 512 max pages, triggering eviction/flushing of file1.txt pages!
    for (int i = 0; i < 200; i++) {
        int64_t written = vfs_write(fd3, buf, 4096);
        KTEST_EXPECT_EQ(written, 4096);
    }

    // Now seek back and read from file1.txt to verify data is intact (read from disk since it was evicted)
    int64_t seek_res = vfs_seek(fd1, 0, SEEK_SET);
    KTEST_EXPECT_EQ(seek_res, 0);

    uint8_t *read_buf = static_cast<uint8_t *>(malloc(4096));
    KTEST_EXPECT(read_buf != nullptr);

    for (int i = 0; i < 200; i++) {
        int64_t bytes_read = vfs_read(fd1, read_buf, 4096);
        KTEST_EXPECT_EQ(bytes_read, 4096);
        // Verify contents
        for (int j = 0; j < 4096; j++) {
            if (read_buf[j] != buf[j]) {
                KTEST_EXPECT_EQ(read_buf[j], buf[j]);
                break;
            }
        }
    }

    // Close all files
    KTEST_EXPECT_EQ(vfs_close(fd1), 0);
    KTEST_EXPECT_EQ(vfs_close(fd2), 0);
    KTEST_EXPECT_EQ(vfs_close(fd3), 0);

    // Cleanup files from unifs
    vfs_unlink("/file1.txt");
    vfs_unlink("/file2.txt");
    vfs_unlink("/file3.txt");

    free(buf);
    free(read_buf);
}

static Mutex g_test_mutex = MUTEX_INIT;
static volatile int g_pi_thread_step = 0;
static Process *g_low_priority_proc = nullptr;

static void pi_low_priority_thread()
{
    g_low_priority_proc = process_get_current();
    g_low_priority_proc->priority = 2;

    mutex_lock(&g_test_mutex);
    g_pi_thread_step = 1;

    while (g_pi_thread_step == 1) {
        scheduler_yield();
    }

    mutex_unlock(&g_test_mutex);
    // Exit once the test is done asserting (step 3) instead of yielding
    // forever: the task must not outlive the ktest suite, or the boot
    // teardown audit waits on it as a never-reaped kernel task.
    while (g_pi_thread_step != 3) {
        scheduler_yield();
    }
    process_exit(0);
}

KTEST(extended_priority_inheritance)
{
    g_pi_thread_step = 0;
    g_low_priority_proc = nullptr;
    mutex_init(&g_test_mutex);

    Process *thread = scheduler_create_task(pi_low_priority_thread, "pi_test_thread");
    KTEST_EXPECT(thread != nullptr);

    while (g_pi_thread_step == 0) {
        scheduler_yield();
    }

    KTEST_EXPECT(g_low_priority_proc != nullptr);
    KTEST_EXPECT_EQ(g_low_priority_proc->priority, 2);

    Process *current = process_get_current();
    uint8_t orig_priority = current->priority;
    current->priority = 0;

    g_pi_thread_step = 2;

    mutex_lock(&g_test_mutex);

    KTEST_EXPECT_EQ(g_low_priority_proc->priority, 0);

    mutex_unlock(&g_test_mutex);
    current->priority = orig_priority;

    // Release the helper (step 3): it exits and is auto-reaped, leaving no
    // kernel task behind for the teardown audit to wait on.
    g_pi_thread_step = 3;
}
