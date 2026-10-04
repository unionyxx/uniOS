#include <kernel/fs/vfs.h>
#include <kernel/ktest.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/futex.h>
#include <libk/kstring.h>
#include <uapi/syscalls.h>

#ifdef DEBUG

// Fault injection at process_fork's two clone allocations (declared in
// scheduler.cpp): the ktest flips one, the fork must roll the partial clone
// back and refuse. The hooks simulate the failure AFTER the real allocation
// ran, so the rollback path releases real resources (a copied fd table with
// bumped vnode refs, a real VmaList object).
extern bool g_ktest_fail_fd_table_copy;
extern bool g_ktest_fail_vma_list_alloc;

void signal_send(Process *p, int sig);
extern "C" int64_t sys_memfd_create(const char *name, unsigned int flags);

#define FORK_CHECK(cond)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ktest_record_failure(#cond, __FILE__, __LINE__);                                                           \
            goto cleanup;                                                                                              \
        }                                                                                                              \
    } while (0)
#define FORK_CHECK_EQ(a, b) FORK_CHECK((a) == (b))

namespace {
constexpr uint64_t kUserPage = 4096;
constexpr uint64_t kTestVaddr = 0x10000000ULL;
volatile uint32_t *g_park_word = nullptr;

// The forked child parks here (a signal-aware futex wait) so the test can
// kill it cleanly, exactly like the tls fork ktest's child.
void fork_child_park()
{
    sys_futex(g_park_word, FUTEX_WAIT, 0);
    while (true) {
        if (scheduler_fatal_signal_pending(process_get_current()))
            process_exit(0);
        scheduler_yield();
    }
}

// Snapshot of every pid in the process list; a refused fork must add none.
size_t snapshot_list_pids(uint64_t *out, size_t cap)
{
    size_t count = 0;
    Process *scan = scheduler_get_process_list();
    if (!scan)
        return 0;
    do {
        if (count < cap)
            out[count++] = scan->pid;
        scan = scan->next;
    } while (scan != scheduler_get_process_list());
    return count;
}

bool any_new_pid(const uint64_t *before, size_t before_len)
{
    Process *scan = scheduler_get_process_list();
    if (!scan)
        return false;
    do {
        bool known = false;
        for (size_t i = 0; i < before_len; i++) {
            if (before[i] == scan->pid) {
                known = true;
                break;
            }
        }
        if (!known)
            return true;
        scan = scan->next;
    } while (scan != scheduler_get_process_list());
    return false;
}
} // namespace

KTEST(fork_alloc_failure_rolls_back)
{
    const uint64_t *orig_page_table = nullptr;
    VMA *orig_vma_list = nullptr;
    uint64_t *throwaway_pml4 = nullptr;
    VMA *vma = nullptr;
    void *page = nullptr;
    uint8_t *page_alias = nullptr;
    int64_t memfd = -1;
    uint64_t child_pids[8] = {0};
    size_t child_pid_count = 0;

    Process *leader = process_get_current();
    FORK_CHECK(leader != nullptr);
    orig_page_table = leader->page_table;
    orig_vma_list = leader->vmalist->head;

    // A private throwaway address space: the kernel pml4 is not clonable
    // (its low-memory identity mappings have no PMM refcounts).
    throwaway_pml4 = vmm_create_address_space();
    FORK_CHECK(throwaway_pml4 != nullptr);
    leader->page_table = throwaway_pml4;

    page = pmm_alloc_frame();
    FORK_CHECK(page != nullptr);
    page_alias = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(page)));
    FORK_CHECK(vmm_replace_page_in(throwaway_pml4, kTestVaddr, reinterpret_cast<uint64_t>(page),
                                   PTE_PRESENT | PTE_USER | PTE_WRITABLE)
                   .ok());
    *reinterpret_cast<volatile uint32_t *>(page_alias) = 0;
    g_park_word = reinterpret_cast<volatile uint32_t *>(kTestVaddr);

    vma = static_cast<VMA *>(malloc(sizeof(VMA)));
    FORK_CHECK(vma != nullptr);
    vma->start = kTestVaddr;
    vma->end = kTestVaddr + kUserPage;
    vma->flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE;
    vma->type = VMAType::Anonymous;
    vma->next = nullptr;
    leader->vmalist->head = vma;

    // The fd-rollback assertion needs a vnode whose refcount the copy bumps
    // and the rollback must give back.
    memfd = sys_memfd_create(nullptr, 0);
    FORK_CHECK(memfd >= 3);

    // Everything below lives in one act block: failure jumps only ever
    // leave scopes, never enter them.
    {
        const uint64_t refs_before =
            leader->fdtab->fds[memfd].vnode ? static_cast<uint64_t>(leader->fdtab->fds[memfd].vnode->ref_count) : 0;
        FORK_CHECK(refs_before != 0);

        SyscallFrame fork_frame = {};
        fork_frame.rip = reinterpret_cast<uint64_t>(fork_child_park);
        fork_frame.rsp = reinterpret_cast<uint64_t>(page_alias) + kUserPage;
        fork_frame.cs = 0x08;
        fork_frame.ss = 0x10;
        fork_frame.rflags = 0x202;

        // Control: with the hooks clear, the fork from the throwaway space must
        // succeed - otherwise the failure cases below would pass for the wrong
        // reason (a broken fixture, not a rollback).
        {
            const uint64_t control_pid = process_fork(&fork_frame);
            FORK_CHECK(control_pid != static_cast<uint64_t>(-1));
            FORK_CHECK(control_pid != 0);
            if (child_pid_count < sizeof(child_pids) / sizeof(child_pids[0]))
                child_pids[child_pid_count++] = control_pid;

            bool parked = false;
            for (int i = 0; i < 200 && !parked; i++) {
                scheduler_yield();
                Process *c = process_find_by_pid(control_pid);
                parked = c && (c->state == ProcessState_Blocked || c->state == ProcessState_Waiting);
            }
            FORK_CHECK(parked);

            Process *child = process_find_by_pid(control_pid);
            if (child)
                signal_send(child, SIGKILL);
            bool gone = false;
            for (int i = 0; i < 500 && !gone; i++) {
                scheduler_yield();
                gone = process_find_by_pid(control_pid) == nullptr;
            }
            FORK_CHECK(gone);
            FORK_CHECK_EQ(leader->fdtab->fds[memfd].vnode->ref_count, refs_before);
        }

        // fd-table failure: rollback releases the copied table (vnode refs back)
        // and no child is published.
        {
            uint64_t before[32];
            const size_t before_len = snapshot_list_pids(before, sizeof(before) / sizeof(before[0]));
            g_ktest_fail_fd_table_copy = true;
            const uint64_t rc = process_fork(&fork_frame);
            g_ktest_fail_fd_table_copy = false;
            FORK_CHECK_EQ(rc, static_cast<uint64_t>(-1));
            FORK_CHECK(!any_new_pid(before, before_len));
            FORK_CHECK_EQ(leader->fdtab->fds[memfd].vnode->ref_count, refs_before);
        }

        // vma-list failure: same refusal, no child, no fd leak either (the
        // successful fd copy before the failed vma allocation must be released).
        {
            uint64_t before[32];
            const size_t before_len = snapshot_list_pids(before, sizeof(before) / sizeof(before[0]));
            g_ktest_fail_vma_list_alloc = true;
            const uint64_t rc = process_fork(&fork_frame);
            g_ktest_fail_vma_list_alloc = false;
            FORK_CHECK_EQ(rc, static_cast<uint64_t>(-1));
            FORK_CHECK(!any_new_pid(before, before_len));
            FORK_CHECK_EQ(leader->fdtab->fds[memfd].vnode->ref_count, refs_before);
        }

        // The leader's signals stay clean through it all: a rolled-back fork
        // never publishes, so nothing can signal the ktest task.
        FORK_CHECK_EQ(leader->signals.pending, 0ULL);
    }

cleanup:
    g_ktest_fail_fd_table_copy = false;
    g_ktest_fail_vma_list_alloc = false;
    g_park_word = nullptr;

    // Drain any child a failed check left parked in the throwaway space.
    for (size_t i = 0; i < child_pid_count; i++) {
        Process *leftover = process_find_by_pid(child_pids[i]);
        if (leftover)
            signal_send(leftover, SIGKILL);
    }
    for (size_t i = 0; i < child_pid_count; i++) {
        for (int j = 0; j < 500; j++) {
            if (process_find_by_pid(child_pids[i]) == nullptr)
                break;
            scheduler_yield();
        }
    }

    if (memfd >= 3)
        vfs_close(static_cast<int>(memfd));

    leader->page_table = const_cast<uint64_t *>(orig_page_table);
    leader->vmalist->head = orig_vma_list;

    if (throwaway_pml4) {
        // Leave the throwaway page tables before freeing them: context
        // switches during the test loaded them into CR3 (the kernel half
        // maps fine), and the pml4 frame must not go back to the PMM under
        // a live CR3.
        vmm_switch_address_space(
            reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset()));
        vmm_unmap_page_in(throwaway_pml4, kTestVaddr);
        vmm_free_address_space(throwaway_pml4);
    }
    if (page)
        pmm_free_frame(page);
    free(vma);
}

#endif // DEBUG
