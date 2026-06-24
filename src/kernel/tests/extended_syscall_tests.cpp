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

