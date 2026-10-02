#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <kernel/syscall.h>
#include <kernel/tls.h>
#include <libk/kstring.h>
#include <uapi/syscalls.h>
#include <uapi/syscalls_ext.h>
#include <uapi/tcb.h>

void signal_send(Process *p, int sig);

namespace {

constexpr uint64_t TEST_VADDR = 0x10000000ULL;
// Template page, deliberately below the TLS allocator's 0x100000000 floor:
// a failed test's early return leaks its PTE, and everything tls_install
// maps first-fits from the floor upward — a leaked floor-adjacent mapping
// would fail every later install (vmm_map_page_in refuses present PTEs).
constexpr uint64_t TEST_TEMPLATE_VADDR = 0x10020000ULL;
constexpr uint64_t kTemplateBytes = 64;
constexpr uint64_t kTemplateAlign = 32;
constexpr uint64_t kUserPage = 4096;

// vmm_virt_to_phys_in returns the offset-inclusive physical address, so the
// direct-map alias of any user VA in the target pml4 is one lookup away.
const uint8_t *tls_direct_read(const Process *p, uint64_t va)
{
    uint64_t *pml4 = p->page_table ? p->page_table : vmm_get_kernel_pml4();
    const uint64_t phys = vmm_virt_to_phys_in(pml4, va);
    if (phys == 0)
        return nullptr;
    return reinterpret_cast<const uint8_t *>(vmm_phys_to_virt(phys));
}

} // namespace

static volatile uint32_t *g_tls_park_word;

// Parks in a futex wait so the assertions can inspect the thread's TLS
// state while it is alive on any core, then dies through the group-kill
// signal path (process_exit: the TLS mapping stays, the leader's teardown
// owns it — exactly the group-teardown contract).
static void tls_parked_waiter()
{
    sys_futex(g_tls_park_word, FUTEX_WAIT, 0);
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            process_exit(0);
        scheduler_yield();
    }
}

// Parks like the waiter, but on wake it leaves through SYS_THREAD_EXIT —
// the path whose TLS unmap discipline is under test.
static void tls_parked_exiter()
{
    sys_futex(g_tls_park_word, FUTEX_WAIT, 0);
    sys_thread_exit(0);
}

KTEST(tls_tcb_valid_on_create)
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
    g_tls_park_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_tls_park_word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + kUserPage;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->is_cow = false;
    vma->next = nullptr;
    leader->vmalist->head = vma;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(kUserPage);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + kUserPage);

    // The ktest leader has no PT_TLS (template size 0): the create must
    // still install the TCB — the always-TCB invariant.
    const int64_t tid = sys_thread_create(tls_parked_waiter, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    bool parked = false;
    for (int i = 0; i < 200 && !parked; i++) {
        scheduler_yield();
        Process *t = process_find_by_pid(static_cast<uint64_t>(tid));
        parked = t && (t->state == ProcessState_Blocked || t->state == ProcessState_Waiting);
    }
    KTEST_EXPECT(parked);

    uint64_t tls_lo = 0;
    uint64_t tls_len = 0;
    Process *thread = process_find_by_pid(static_cast<uint64_t>(tid));
    KTEST_EXPECT(thread != nullptr);
    if (thread) {
        KTEST_EXPECT(thread->fs_base != 0);
        KTEST_EXPECT(thread->tls_lo != 0);
        tls_lo = thread->tls_lo;
        tls_len = thread->tls_len;

        const UniTcb *tcb = reinterpret_cast<const UniTcb *>(tls_direct_read(thread, thread->fs_base));
        KTEST_EXPECT(tcb != nullptr);
        if (tcb) {
            KTEST_EXPECT_EQ(tcb->self, thread->fs_base);
            KTEST_EXPECT_EQ(tcb->tid, static_cast<uint64_t>(tid));
        }
    }

    process_group_kill_siblings(leader);
    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        gone = process_find_by_pid(static_cast<uint64_t>(tid)) == nullptr;
    }
    KTEST_EXPECT(gone);

    int32_t status = 0;
    (void)process_waitpid(tid, &status, 0);

    // Signal death leaves the mapping in the shared list: the leader owns
    // the teardown, so the recorded range leaves through the standard unmap.
    if (tls_lo != 0)
        KTEST_EXPECT(munmap_process_range(leader, tls_lo, tls_len));

    free(stack);
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
}

KTEST(tls_template_clone)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    const uint64_t orig_fs_base = leader->fs_base;
    const uint64_t orig_template_va = leader->tls_template_va;
    const uint64_t orig_template_size = leader->tls_template_size;
    const uint64_t orig_align = leader->tls_align;
    if (!leader->page_table)
        leader->page_table = vmm_get_kernel_pml4();

    void *page = pmm_alloc_frame();
    KTEST_EXPECT(page != nullptr);
    Result<void> map = vmm_replace_page_in(leader->page_table, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                           PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(map.ok());
    g_tls_park_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_tls_park_word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + kUserPage;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->is_cow = false;
    vma->next = nullptr;
    leader->vmalist->head = vma;

    // Template surgery: a user page holding known bytes, wired to the ktest
    // task's TLS facts the way a PT_TLS-carrying image would be. Stores go
    // through the direct map so they do not depend on which page table this
    // core currently has loaded.
    void *template_frame = pmm_alloc_frame();
    KTEST_EXPECT(template_frame != nullptr);
    Result<void> tmap =
        vmm_replace_page_in(leader->page_table, TEST_TEMPLATE_VADDR, reinterpret_cast<uint64_t>(template_frame),
                            PTE_PRESENT | PTE_USER | PTE_WRITABLE);
    KTEST_EXPECT(tmap.ok());

    uint8_t image[kTemplateBytes];
    for (uint32_t i = 0; i < kTemplateBytes; i++)
        image[i] = static_cast<uint8_t>(i * 13 + 5);
    if (tmap.ok()) {
        auto *dst = reinterpret_cast<volatile uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(template_frame)));
        for (uint32_t i = 0; i < kTemplateBytes; i++)
            dst[i] = image[i];
    }

    leader->tls_template_va = TEST_TEMPLATE_VADDR;
    leader->tls_template_size = kTemplateBytes;
    leader->tls_align = kTemplateAlign;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(kUserPage);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + kUserPage);

    const int64_t tid = sys_thread_create(tls_parked_waiter, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    bool parked = false;
    for (int i = 0; i < 200 && !parked; i++) {
        scheduler_yield();
        Process *t = process_find_by_pid(static_cast<uint64_t>(tid));
        parked = t && (t->state == ProcessState_Blocked || t->state == ProcessState_Waiting);
    }
    KTEST_EXPECT(parked);

    uint64_t tls_lo = 0;
    uint64_t tls_len = 0;
    Process *thread = process_find_by_pid(static_cast<uint64_t>(tid));
    KTEST_EXPECT(thread != nullptr);
    if (thread) {
        KTEST_EXPECT(thread->fs_base != 0);
        KTEST_EXPECT(thread->tls_lo != 0);

        // The clone's block [fs_base - size, fs_base) holds the template
        // image byte for byte; align 32 rounds 64 bytes to 64, so the
        // thread pointer sits exactly one block above the mapping start.
        const uint8_t *block = tls_direct_read(thread, thread->fs_base - kTemplateBytes);
        KTEST_EXPECT(block != nullptr);
        if (block)
            KTEST_EXPECT(kstring::memcmp(block, image, kTemplateBytes) == 0);

        KTEST_EXPECT_EQ(thread->fs_base, thread->tls_lo + kTemplateBytes);
        KTEST_EXPECT_EQ(thread->tls_len, kUserPage);

        const UniTcb *tcb = reinterpret_cast<const UniTcb *>(tls_direct_read(thread, thread->fs_base));
        KTEST_EXPECT(tcb != nullptr);
        if (tcb)
            KTEST_EXPECT_EQ(tcb->tid, static_cast<uint64_t>(tid));

        tls_lo = thread->tls_lo;
        tls_len = thread->tls_len;
    }

    process_group_kill_siblings(leader);
    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        gone = process_find_by_pid(static_cast<uint64_t>(tid)) == nullptr;
    }
    KTEST_EXPECT(gone);

    int32_t status = 0;
    (void)process_waitpid(tid, &status, 0);

    if (tls_lo != 0)
        KTEST_EXPECT(munmap_process_range(leader, tls_lo, tls_len));

    leader->fs_base = orig_fs_base;
    leader->tls_template_va = orig_template_va;
    leader->tls_template_size = orig_template_size;
    leader->tls_align = orig_align;

    free(stack);
    if (tmap.ok()) {
        vmm_unmap_page_in(leader->page_table, TEST_TEMPLATE_VADDR);
        pmm_free_frame(template_frame);
    }
    vmm_unmap_page_in(leader->page_table, TEST_VADDR);
    pmm_free_frame(page);
    free(vma);
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
}

KTEST(tls_exit_unmaps)
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
    g_tls_park_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);
    *g_tls_park_word = 0;

    VMA *vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    KTEST_EXPECT(vma != nullptr);
    vma->start = TEST_VADDR;
    vma->end = TEST_VADDR + kUserPage;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->is_cow = false;
    vma->next = nullptr;
    leader->vmalist->head = vma;

    SyscallFrame mock_frame = {};
    mock_frame.cs = 0x08;
    mock_frame.ss = 0x10;
    mock_frame.rflags = 0x202;

    void *stack = malloc(kUserPage);
    KTEST_EXPECT(stack != nullptr);
    void *top = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(stack) + kUserPage);

    const int64_t tid = sys_thread_create(tls_parked_exiter, nullptr, top, &mock_frame);
    KTEST_EXPECT(tid > 0);

    bool parked = false;
    for (int i = 0; i < 200 && !parked; i++) {
        scheduler_yield();
        Process *t = process_find_by_pid(static_cast<uint64_t>(tid));
        parked = t && (t->state == ProcessState_Blocked || t->state == ProcessState_Waiting);
    }
    KTEST_EXPECT(parked);

    uint64_t tls_lo = 0;
    Process *thread = process_find_by_pid(static_cast<uint64_t>(tid));
    KTEST_EXPECT(thread != nullptr);
    if (thread) {
        KTEST_EXPECT(thread->tls_lo != 0);
        tls_lo = thread->tls_lo;
        // Present before the exit: the mapping is a live VMA in the list
        // the whole group shares.
        KTEST_EXPECT(vma_find(leader->vmalist->head, tls_lo) != nullptr);
    }

    // Wake it: SYS_THREAD_EXIT runs the TLS unmap on the way out.
    KTEST_EXPECT_EQ(sys_futex(g_tls_park_word, FUTEX_WAKE, 1), 1);

    bool gone = false;
    for (int i = 0; i < 500 && !gone; i++) {
        scheduler_yield();
        Process *t = process_find_by_pid(static_cast<uint64_t>(tid));
        gone = !t || t->state == ProcessState_Zombie;
    }
    KTEST_EXPECT(gone);

    // The recorded TLS range left the shared VMA list.
    KTEST_EXPECT(vma_find(leader->vmalist->head, tls_lo) == nullptr);

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

static void tls_fork_child_park()
{
    sys_futex(g_tls_park_word, FUTEX_WAIT, 0);
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            process_exit(0);
        scheduler_yield();
    }
}

// This test rewrites the leader's page_table pointer, so a leaked failure
// return would leave later ktests running with a page table that is not
// the one this core has loaded (the fs:0 round-trip ktest dereferences
// through exactly that assumption). Every failure path must reach the
// restore block: the goto pattern from the exec-group test.
#define TLS_FORK_CHECK(cond)                                                                                           \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ktest_record_failure(#cond, __FILE__, __LINE__);                                                           \
            goto cleanup;                                                                                              \
        }                                                                                                              \
    } while (0)
#define TLS_FORK_CHECK_EQ(a, b) TLS_FORK_CHECK((a) == (b))

KTEST(tls_fork_copies_fields)
{
    Process *leader = process_get_current();
    KTEST_EXPECT(leader != nullptr);
    if (!leader)
        return;

    uint64_t *orig_page_table = leader->page_table;
    VMA *orig_vma_list = leader->vmalist->head;
    const uint64_t orig_fs_base = leader->fs_base;
    const uint64_t orig_template_va = leader->tls_template_va;
    const uint64_t orig_template_size = leader->tls_template_size;
    const uint64_t orig_align = leader->tls_align;
    const uint64_t orig_tls_lo = leader->tls_lo;
    const uint64_t orig_tls_len = leader->tls_len;

    uint64_t *fork_pml4 = nullptr;
    void *page = nullptr;
    VMA *vma = nullptr;
    uint64_t tls_lo = 0;
    uint64_t child_pid = 0;
    bool page_mapped = false;

    // Everything below lives in one act block: failure jumps only ever
    // leave scopes, never enter them (the exec-group test's rule).
    {
        // A private throwaway address space (the exec-group surgery
        // pattern): the kernel pml4 itself is not clonable — its
        // low-memory identity mappings have no PMM refcounts, and the
        // fork clone refcount_incs every present user-half leaf.
        fork_pml4 = vmm_create_address_space();
        TLS_FORK_CHECK(fork_pml4 != nullptr);
        leader->page_table = fork_pml4;

        page = pmm_alloc_frame();
        TLS_FORK_CHECK(page != nullptr);
        Result<void> map = vmm_replace_page_in(fork_pml4, TEST_VADDR, reinterpret_cast<uint64_t>(page),
                                               PTE_PRESENT | PTE_USER | PTE_WRITABLE);
        TLS_FORK_CHECK(map.ok());
        page_mapped = true;
        // Stores go through the direct map: they do not depend on which page
        // table this core currently has loaded.
        uint8_t *page_alias = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(page)));
        *reinterpret_cast<volatile uint32_t *>(page_alias) = 0;
        g_tls_park_word = reinterpret_cast<volatile uint32_t *>(TEST_VADDR);

        vma = static_cast<VMA *>(malloc(sizeof(VMA)));
        TLS_FORK_CHECK(vma != nullptr);
        vma->start = TEST_VADDR;
        vma->end = TEST_VADDR + kUserPage;
        vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
        vma->type = VMAType::Anonymous;
        vma->is_cow = false;
        vma->next = nullptr;
        leader->vmalist->head = vma;

        // Leader surgery: a real install (what exec will do for a leader) plus
        // the template facts and the recorded range a leader install carries.
        static uint8_t image[kTemplateBytes];
        for (uint32_t i = 0; i < kTemplateBytes; i++)
            image[i] = static_cast<uint8_t>(i * 19 + 7);

        tls_lo = tls_install(leader, image, kTemplateBytes, kTemplateAlign);
        TLS_FORK_CHECK(tls_lo != 0);
        leader->tls_template_va = TEST_VADDR;
        leader->tls_template_size = kTemplateBytes;
        leader->tls_align = kTemplateAlign;
        leader->tls_lo = tls_lo;
        leader->tls_len = kUserPage;

    SyscallFrame fork_frame = {};
    fork_frame.rip = reinterpret_cast<uint64_t>(tls_fork_child_park);
    // Same mock-frame contract as the thread tests: a kernel cs and a
    // stack pointer that points at a real scratch buffer, so the child's
    // entry lands on a writable stack whichever way iretq loads rsp.
    fork_frame.rsp = reinterpret_cast<uint64_t>(page_alias) + kUserPage;
    fork_frame.cs = 0x08;
    fork_frame.ss = 0x10;
    fork_frame.rflags = 0x202;

        child_pid = process_fork(&fork_frame);
        TLS_FORK_CHECK(child_pid != static_cast<uint64_t>(-1));
        TLS_FORK_CHECK(child_pid != 0);

        // Read the child before it can run: the fork copy must land the
        // thread pointer and the group-wide template facts unchanged, while
        // the thread-owned mapping range must NOT carry over (the child is a
        // new leader; its block dies with its own address space).
        Process *child = process_find_by_pid(child_pid);
        TLS_FORK_CHECK(child != nullptr);
        TLS_FORK_CHECK_EQ(child->fs_base, leader->fs_base);
        TLS_FORK_CHECK_EQ(child->tls_template_va, leader->tls_template_va);
        TLS_FORK_CHECK_EQ(child->tls_template_size, leader->tls_template_size);
        TLS_FORK_CHECK_EQ(child->tls_align, leader->tls_align);
        TLS_FORK_CHECK_EQ(child->tls_lo, 0ULL);
        TLS_FORK_CHECK_EQ(child->tls_len, 0ULL);
        TLS_FORK_CHECK_EQ(child->leader_pid, child_pid);

    // The inherited thread pointer stays valid in the COW'd copy: the
    // child's space resolves it to the same self-consistent TCB bytes.
    // (The inherited TCB still carries the parent's tid — exec re-stamps
    // it for the new leader; fork's contract is the field copy only.)
    const UniTcb *tcb = reinterpret_cast<const UniTcb *>(tls_direct_read(child, child->fs_base));
    TLS_FORK_CHECK(tcb != nullptr);
    TLS_FORK_CHECK_EQ(tcb->self, child->fs_base);

        bool child_parked = false;
        for (int i = 0; i < 200 && !child_parked; i++) {
            scheduler_yield();
            Process *c = process_find_by_pid(child_pid);
            child_parked = c && (c->state == ProcessState_Blocked || c->state == ProcessState_Waiting);
        }
        TLS_FORK_CHECK(child_parked);

        // Drain: the parked child dies by signal, its zombie auto-reaps (the
        // ktest leader is the pid-0 kernel task).
        Process *child_live = process_find_by_pid(child_pid);
        if (child_live)
            signal_send(child_live, SIGKILL);
        bool gone = false;
        for (int i = 0; i < 500 && !gone; i++) {
            scheduler_yield();
            gone = process_find_by_pid(child_pid) == nullptr;
        }
        TLS_FORK_CHECK(gone);

        // Teardown: the install's mapping leaves through the standard unmap
        // (its frames are still COW-shared with the dead child's space; the
        // refcounted free handles that), the surgical fields go back, and the
        // throwaway space dies with its page tables.
        TLS_FORK_CHECK(munmap_process_range(leader, tls_lo, kUserPage));
    }

cleanup:
    leader->fs_base = orig_fs_base;
    leader->tls_template_va = orig_template_va;
    leader->tls_template_size = orig_template_size;
    leader->tls_align = orig_align;
    leader->tls_lo = orig_tls_lo;
    leader->tls_len = orig_tls_len;

    // A child that outlived a failed check cannot be left parked in a
    // dying address space: kill and drain it here too.
    if (child_pid != 0) {
        Process *child_live = process_find_by_pid(child_pid);
        if (child_live)
            signal_send(child_live, SIGKILL);
        for (int i = 0; i < 500; i++) {
            if (process_find_by_pid(child_pid) == nullptr)
                break;
            scheduler_yield();
        }
    }

    if (page_mapped) {
        vmm_unmap_page_in(fork_pml4, TEST_VADDR);
        pmm_free_frame(page);
    }
    free(vma);
    if (fork_pml4) {
        // Leave the throwaway page tables before freeing them: this core
        // may still run on them (kernel half) after the drain-phase
        // dispatches, and the pml4 frame must not go back to the PMM
        // under a live CR3.
        vmm_switch_address_space(
            reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));
        vmm_free_address_space(fork_pml4);
    }
    leader->signals.pending = 0;
    leader->page_table = orig_page_table;
    leader->vmalist->head = orig_vma_list;
}
