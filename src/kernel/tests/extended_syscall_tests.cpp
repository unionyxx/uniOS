#include <kernel/ktest.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <kernel/sync/epoll.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/fs/pipe.h>
#include <kernel/fs/vfs.h>
#include <kernel/syscall.h>
#include <uapi/syscalls.h>
#include <uapi/syscalls_ext.h>
#include <libk/kstd.h>
#include <libk/kstring.h>
#include <kernel/panic.h>

extern "C" int64_t sys_mprotect(void *addr, size_t len, int prot);

static int test_find_free_fd(Process *p)
{
    if (!p)
        return -1;
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (!p->fd_table[i].used)
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

static volatile uint32_t g_test_futex = 0;
static void futex_waiter_thread()
{
    sys_futex(&g_test_futex, FUTEX_WAIT, 0);
    while (true) {
        scheduler_yield();
    }
}

KTEST(extended_syscalls_futex)
{
    Process *current = process_get_current();
    KTEST_EXPECT(current != nullptr);

    uint64_t *orig_page_table = current->page_table;
    if (!current->page_table)
        current->page_table = vmm_get_kernel_pml4();

    volatile uint32_t val = 42;

    volatile uint32_t *unaligned_uaddr = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uintptr_t>(&val) | 1);
    int64_t res = sys_futex(unaligned_uaddr, FUTEX_WAIT, 42);
    KTEST_EXPECT_EQ(res, -22); // -EINVAL

    res = sys_futex(nullptr, FUTEX_WAIT, 42);
    KTEST_EXPECT_EQ(res, -14); // -EFAULT

    res = sys_futex(&val, FUTEX_WAIT, 100);
    KTEST_EXPECT_EQ(res, -11); // -EAGAIN (val != expected)

    res = sys_futex(&val, FUTEX_WAKE, 1);
    KTEST_EXPECT_EQ(res, 0); // nobody waiting

    // Test actual blocking and waking to ensure the futex lock is correctly released
    g_test_futex = 0;
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

    // Wake the waiter thread
    int64_t woken = sys_futex(&g_test_futex, FUTEX_WAKE, 1);
    KTEST_EXPECT_EQ(woken, 1);

    // Yield to let the waiter thread resume
    for (int i = 0; i < 5; i++) {
        scheduler_yield();
    }

    // Call WAKE again to verify we do not deadlock on bucket->lock
    int64_t woken2 = sys_futex(&g_test_futex, FUTEX_WAKE, 1);
    KTEST_EXPECT_EQ(woken2, 0);

    // Reap the child thread
    Process *child = process_find_by_pid(static_cast<uint64_t>(thread_pid));
    KTEST_EXPECT(child != nullptr);
    scheduler_remove_from_ready_queue(child);
    child->state = ProcessState_Zombie;

    int32_t status = 0;
    int64_t reaped_pid = process_waitpid(thread_pid, &status, 0);
    KTEST_EXPECT_EQ(reaped_pid, thread_pid);

    free(stack);
    current->page_table = orig_page_table;
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
    VMA *orig_vma_list = current->vma_list;
    uint32_t orig_vma_count = current->vma_count;

    if (!current->page_table)
        current->page_table = vmm_get_kernel_pml4();

    uint64_t test_vaddr = 0x10000000ULL;
    void *phys = pmm_alloc_frame();
    KTEST_EXPECT(phys != nullptr);

    Result<void> map_res = vmm_map_page_in(current->page_table, test_vaddr, reinterpret_cast<uint64_t>(phys), PTE_PRESENT | PTE_USER);
    KTEST_EXPECT(map_res.ok());

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = test_vaddr;
    vma->end = test_vaddr + 4096;
    vma->flags = PTE_PRESENT | PTE_USER;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;

    current->vma_list = vma;
    current->vma_count = 1;

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
    current->vma_list = orig_vma_list;
    current->vma_count = orig_vma_count;
}

KTEST(extended_syscalls_epoll)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    int64_t epfd = sys_epoll_create(0);
    KTEST_EXPECT_EQ(epfd, -22); // size <= 0

    epfd = sys_epoll_create(10);
    KTEST_EXPECT(epfd >= 3);

    int pipe_id = pipe_create();
    KTEST_EXPECT(pipe_id >= 0);

    int read_fd = test_find_free_fd(p);
    KTEST_EXPECT(read_fd >= 0);
    p->fd_table[read_fd].used = true;
    p->fd_table[read_fd].vnode = pipe_get_vnode(pipe_id, false);
    p->fd_table[read_fd].flags = 0;

    int write_fd = test_find_free_fd(p);
    KTEST_EXPECT(write_fd >= 0);
    p->fd_table[write_fd].used = true;
    p->fd_table[write_fd].vnode = pipe_get_vnode(pipe_id, true);
    p->fd_table[write_fd].flags = 0;

    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = read_fd;
    int64_t res = sys_epoll_ctl(static_cast<int>(epfd), EPOLL_CTL_ADD, read_fd, &ev);
    KTEST_EXPECT_EQ(res, 0);

    // duplicate add
    res = sys_epoll_ctl(static_cast<int>(epfd), EPOLL_CTL_ADD, read_fd, &ev);
    KTEST_EXPECT_EQ(res, -17); // -EEXIST

    struct epoll_event events[2] = {};
    res = sys_epoll_wait(static_cast<int>(epfd), events, 2, 0);
    KTEST_EXPECT_EQ(res, 0); // pipe empty

    int64_t written = pipe_write(pipe_id, "test", 4);
    KTEST_EXPECT_EQ(written, 4);

    res = sys_epoll_wait(static_cast<int>(epfd), events, 2, 0);
    KTEST_EXPECT_EQ(res, 1);
    KTEST_EXPECT_EQ(events[0].data.fd, read_fd);
    KTEST_EXPECT((events[0].events & EPOLLIN) != 0);

    char buf[4];
    int64_t read_bytes = pipe_read(pipe_id, buf, 4);
    KTEST_EXPECT_EQ(read_bytes, 4);

    res = sys_epoll_wait(static_cast<int>(epfd), events, 2, 0);
    KTEST_EXPECT_EQ(res, 0); // pipe drained

    vfs_close(read_fd);
    vfs_close(write_fd);
    vfs_close(static_cast<int>(epfd));
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
    VMA *orig_vma_list = p->vma_list;
    uint32_t orig_vma_count = p->vma_count;

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
    KTEST_EXPECT(kstring::strcmp(const_cast<char *>(shared_ptr), test_str) == 0);

    shared_ptr[0] = 'y';
    shared_ptr[1] = 'o';
    shared_ptr[2] = 'u';

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
    KTEST_EXPECT(kstring::strcmp(const_cast<char *>(private_ptr), "youlo Memfd!") == 0);

    private_ptr[0] = 'H';
    private_ptr[1] = 'e';
    private_ptr[2] = 'l';

    KTEST_EXPECT(kstring::strcmp(const_cast<char *>(shared_ptr), "youlo Memfd!") == 0);

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
    p->vma_list = orig_vma_list;
    p->vma_count = orig_vma_count;
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

    VNode *node = p->fd_table[fd].vnode;
    KTEST_EXPECT(node != nullptr);
    KTEST_EXPECT_EQ(node->size, 8192ULL);

    // Test fd_transfer (transfer to self as target_pid)
    int64_t transferred_fd = sys_fd_transfer(p->pid, static_cast<int>(fd));
    KTEST_EXPECT(transferred_fd >= 3);
    KTEST_EXPECT(transferred_fd != fd);
    KTEST_EXPECT(p->fd_table[transferred_fd].used);
    KTEST_EXPECT_EQ(p->fd_table[transferred_fd].vnode, node);

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
    VMA *orig_vma_list = p->vma_list;
    uint32_t orig_vma_count = p->vma_count;

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
    ptr[0] = 'a';
    ptr[4096] = 'b';
    ptr[8192] = 'c';

    // Unmap the middle page (offset 4096, length 4096)
    int64_t munmap_res = syscall_handler(SYS_MUNMAP, mmap_res + 4096, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    // First page should still be present
    uint64_t phys0 = vmm_virt_to_phys_in(p->page_table, mmap_res);
    KTEST_EXPECT(phys0 != 0);
    KTEST_EXPECT_EQ(ptr[0], 'a');

    // Middle page should be unmapped
    uint64_t phys1 = vmm_virt_to_phys_in(p->page_table, mmap_res + 4096);
    KTEST_EXPECT_EQ(phys1, 0);

    // Third page should still be present
    uint64_t phys2 = vmm_virt_to_phys_in(p->page_table, mmap_res + 8192);
    KTEST_EXPECT(phys2 != 0);
    KTEST_EXPECT_EQ(ptr[8192], 'c');

    // Clean up: unmap first and third pages
    munmap_res = syscall_handler(SYS_MUNMAP, mmap_res, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    munmap_res = syscall_handler(SYS_MUNMAP, mmap_res + 8192, 4096, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    int close_res = vfs_close(static_cast<int>(fd));
    KTEST_EXPECT_EQ(close_res, 0);

    p->page_table = orig_page_table;
    p->vma_list = orig_vma_list;
    p->vma_count = orig_vma_count;
}

KTEST(extended_syscalls_mmap_offset)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vma_list;
    uint32_t orig_vma_count = p->vma_count;

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
    full_ptr[0] = 'X';
    full_ptr[4096] = 'Y';
    full_ptr[8192] = 'Z';

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
    KTEST_EXPECT_EQ(offset_ptr[0], 'Y');    // page 1 content
    KTEST_EXPECT_EQ(offset_ptr[4096], 'Z'); // page 2 content

    // Clean up
    munmap_res = syscall_handler(SYS_MUNMAP, mmap_offset, 8192, 0, &frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    int close_res = vfs_close(static_cast<int>(fd));
    KTEST_EXPECT_EQ(close_res, 0);

    p->page_table = orig_page_table;
    p->vma_list = orig_vma_list;
    p->vma_count = orig_vma_count;
}

struct TestStackFrame {
    uint64_t original_rax;
    SyscallFrame frame;
};

struct alignas(64) TestSignalContext {
    InterruptFrame frame;
    alignas(64) uint8_t fpu_state[1024];
    uint64_t old_mask;
    uint32_t magic;
};

constexpr uint32_t TEST_SIG_CONTEXT_MAGIC = 0x51644374; // 'SigC'

extern "C" void signal_check_interrupt(InterruptFrame *frame);
bool g_in_ktest_signal = false;

KTEST(extended_syscalls_signal_context)
{
    Process *p = process_get_current();
    KTEST_EXPECT(p != nullptr);

    uint64_t *orig_page_table = p->page_table;
    VMA *orig_vma_list = p->vma_list;
    uint32_t orig_vma_count = p->vma_count;

    if (!p->page_table) {
        p->page_table = vmm_get_kernel_pml4();
    }

    // Allocate a page of user memory to use as the user stack
    SyscallFrame mmap_frame = {};
    mmap_frame.arg4 = MAP_PRIVATE | MAP_ANONYMOUS;
    mmap_frame.arg5 = static_cast<uint64_t>(-1);
    mmap_frame.arg6 = 0;
    uint64_t mmap_res = syscall_handler(SYS_MMAP, 0, 4096, PROT_READ | PROT_WRITE, &mmap_frame);
    KTEST_EXPECT(mmap_res != static_cast<uint64_t>(-1));

    // Save current signal state
    SignalControl orig_signals = p->signals;

    // Set up signal handler and restorer
    p->signals.handlers[SIGUSR1] = reinterpret_cast<sighandler_t>(0x123456ULL);
    p->signals.restorer = 0x7890ULL;
    p->signals.pending = (1ULL << SIGUSR1);
    p->signals.blocked = 0x112233ULL;

    // Set up mock register state
    TestStackFrame tf = {};
    tf.original_rax = 0xAAABBBULL;
    tf.frame.rip = 0x9999ULL;
    tf.frame.rsp = mmap_res + 4096; // Top of the mapped page
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
    KTEST_EXPECT_EQ(tf.frame.rip, 0x123456ULL);
    // RSP should have decreased
    KTEST_EXPECT(tf.frame.rsp < mmap_res + 4096);
    // The signal should no longer be pending
    KTEST_EXPECT_EQ(p->signals.pending & (1ULL << SIGUSR1), 0ULL);

    // Verify the data pushed to the user stack:
    // The trampoline is pushed at RSP
    uint64_t tramp_phys = vmm_virt_to_phys(tf.frame.rsp);
    KTEST_EXPECT(tramp_phys != 0);
    uint64_t *tramp_val = reinterpret_cast<uint64_t *>(vmm_phys_to_virt(tramp_phys));
    KTEST_EXPECT_EQ(*tramp_val, 0x7890ULL);

    // The SignalContext starts at RSP + 8
    uint64_t ctx_user_addr = tf.frame.rsp + 8;
    uint64_t ctx_phys = vmm_virt_to_phys(ctx_user_addr);
    KTEST_EXPECT(ctx_phys != 0);
    
    TestSignalContext *u_ctx = reinterpret_cast<TestSignalContext *>(vmm_phys_to_virt(ctx_phys));
    KTEST_EXPECT_EQ(u_ctx->frame.rax, 0xAAABBBULL);
    KTEST_EXPECT_EQ(u_ctx->old_mask, 0x112233ULL);
    KTEST_EXPECT_EQ(u_ctx->magic, TEST_SIG_CONTEXT_MAGIC);

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
    KTEST_EXPECT_EQ(returned_rax, 0xAAABBBULL);
    // RIP and RSP should be restored
    KTEST_EXPECT_EQ(tf.frame.rip, 0x9999ULL);
    KTEST_EXPECT_EQ(tf.frame.rsp, mmap_res + 4096);
    // Callee-saved registers should be restored
    KTEST_EXPECT_EQ(tf.frame.rbx, 0x11ULL);
    KTEST_EXPECT_EQ(tf.frame.rbp, 0x22ULL);
    KTEST_EXPECT_EQ(tf.frame.r12, 0x33ULL);
    KTEST_EXPECT_EQ(tf.frame.r13, 0x44ULL);
    KTEST_EXPECT_EQ(tf.frame.r14, 0x55ULL);
    KTEST_EXPECT_EQ(tf.frame.r15, 0x66ULL);
    // Signal mask should be restored
    KTEST_EXPECT_EQ(p->signals.blocked, 0x112233ULL);

    // --- Test signal_check_interrupt ---
    p->signals.pending = (1ULL << SIGUSR1);
    
    InterruptFrame int_frame = {};
    int_frame.rip = 0xaaaaULL;
    int_frame.rsp = mmap_res + 4096;
    int_frame.cs = 0x23ULL; // Ring 3
    int_frame.ss = 0x1BULL;
    int_frame.rflags = 0x202ULL;
    int_frame.rax = 0x5555ULL;

    signal_check_interrupt(&int_frame);

    // Verify it delivered the signal
    KTEST_EXPECT_EQ(int_frame.rip, 0x123456ULL);
    KTEST_EXPECT_EQ(p->signals.pending & (1ULL << SIGUSR1), 0ULL);

    // Clean up
    uint64_t munmap_res = syscall_handler(SYS_MUNMAP, mmap_res, 4096, 0, &mmap_frame);
    KTEST_EXPECT_EQ(munmap_res, 0);

    p->signals = orig_signals;
    p->page_table = orig_page_table;
    p->vma_list = orig_vma_list;
    p->vma_count = orig_vma_count;
}

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



