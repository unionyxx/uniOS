#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <kernel/syscall.h>
#include <libk/kstring.h>
#include <uapi/syscalls_ext.h>

static void fd_dummy_thread_entry()
{
    while (true) {
        scheduler_yield();
    }
}

KTEST(fd_table_fork_copies_entries)
{
    FdTable *parent = fd_table_alloc();
    KTEST_EXPECT(parent != nullptr);
    parent->fds[3].used = true;

    FdTable *child = fd_table_copy(parent);
    KTEST_EXPECT(child != nullptr);
    KTEST_EXPECT(child != parent);
    KTEST_EXPECT(child->fds[3].used);
    child->fds[3].used = false;        // child "closes" its view
    KTEST_EXPECT(parent->fds[3].used); // parent entry unaffected: deep copy

    fd_table_release(parent);
    fd_table_release(child);
}

KTEST(fd_table_thread_shares_live)
{
    FdTable *leader = fd_table_alloc();
    FdTable *thread_view = fd_table_share(leader);
    KTEST_EXPECT(thread_view == leader);
    leader->fds[3].used = true;             // mutation through one pointer
    KTEST_EXPECT(thread_view->fds[3].used); // is visible through the other
    KTEST_EXPECT_EQ(leader->refs, 2u);

    fd_table_release(leader); // leader dies, thread keeps the table
    KTEST_EXPECT_EQ(thread_view->refs, 1u);
    fd_table_release(thread_view);
}

KTEST(fd_table_alloc_marks_stdio_slots)
{
    FdTable *t = fd_table_alloc();
    KTEST_EXPECT(t != nullptr);
    KTEST_EXPECT(t->fds[0].used);
    KTEST_EXPECT(t->fds[1].used);
    KTEST_EXPECT(t->fds[2].used);
    KTEST_EXPECT(!t->fds[3].used);
    KTEST_EXPECT_EQ(t->refs, 1u);
    fd_table_release(t);

    FdTable *bare = fd_table_alloc(false);
    KTEST_EXPECT(bare != nullptr);
    KTEST_EXPECT(!bare->fds[0].used);
    fd_table_release(bare);
}

KTEST(fd_table_thread_create_shares_parent_table)
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

    int64_t tid = sys_thread_create(fd_dummy_thread_entry, nullptr, stack_top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    Process *thread = process_find_by_pid(static_cast<uint64_t>(tid));
    KTEST_EXPECT(thread != nullptr);
    if (thread) {
        // Live sharing, not a copy: same table object, refcount bumped.
        KTEST_EXPECT(thread->fdtab == parent->fdtab);
        KTEST_EXPECT_EQ(parent->fdtab->refs, 2u);
    }

    // Reap the throwaway thread (futex-ktest pattern: it never ran
    // process_exit; the reaper releases its fd-table reference by itself).
    if (thread) {
        scheduler_remove_from_ready_queue(thread);
        thread->state = ProcessState_Zombie;
        // The thread never exits through SYS_THREAD_EXIT: drain its TLS
        // mapping by hand, then sever its references (not the shared list
        // itself) so the reaper cannot follow them.
        if (thread->tls_lo != 0) {
            (void)munmap_process_range(thread, thread->tls_lo, thread->tls_len);
            thread->tls_lo = 0;
            thread->tls_len = 0;
        }
        thread->vmalist = nullptr;
        thread->page_table = nullptr;
    }
    // Reap immediately: the kernel-zombie reaper steals unwaited zombies
    // during a yield (futex-ktest pattern).
    int32_t status = 0;
    KTEST_EXPECT_EQ(process_waitpid(tid, &status, 0), tid);

    free(stack);
    parent->page_table = orig_page_table;
}
