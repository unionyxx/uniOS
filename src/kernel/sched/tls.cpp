#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/tls.h>
#include <libk/kstring.h>
#include <uapi/tcb.h>

namespace {

constexpr uint64_t kTlsPage = 4096;
// Same first-fit floor as sys_mmap's anonymous allocations, and the same
// user-space ceiling (USER_STACK_TOP is private to syscall.cpp).
constexpr uint64_t kTlsCeiling = 0x0000700000000000ULL;
constexpr uint64_t kTlsMapFloor = 0x100000000ULL;

// Write len bytes to dst (user VA in the target pml4) through the kernel
// direct map, page by page; src == null zeroes instead. Returns false when
// a page is not translated (caller mapped it wrong).
[[nodiscard]] bool tls_write_user_range(const uint64_t *pml4, uint64_t dst, const void *src, uint64_t len)
{
    const uint8_t *in = static_cast<const uint8_t *>(src);
    while (len > 0) {
        const uint64_t page_off = dst & (kTlsPage - 1);
        const uint64_t chunk = (kTlsPage - page_off) < len ? (kTlsPage - page_off) : len;
        const uint64_t phys = vmm_virt_to_phys_in(pml4, dst);
        if (phys == 0)
            return false;
        uint8_t *out = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(phys));
        if (in)
            kstring::copy_memory(out, in, chunk);
        else
            kstring::zero_memory(out, chunk);
        dst += chunk;
        if (in)
            in += chunk;
        len -= chunk;
    }
    return true;
}

} // namespace

uint64_t tls_install(Process *proc, const void *template_src, uint64_t tls_size, uint64_t tls_align)
{
    if (!proc || !proc->vmalist)
        return 0;
    if (tls_align != 0 && (tls_align & (tls_align - 1)) != 0)
        return 0;

    // The linker's TPOFF offsets round the block up to p_align ALONE (ld
    // and lld both do; the old 16 floor here shifted every __thread
    // variable below its link-time offset for any p_align < 16 image).
    // p_align 0 (degenerate) rounds like 1.
    const uint64_t align = tls_align > 1 ? tls_align : 1;
    const uint64_t padded = (tls_size + align - 1) & ~(align - 1);
    const uint64_t map_len = (padded + sizeof(UniTcb) + kTlsPage - 1) & ~(kTlsPage - 1);

    uint64_t *const target_pml4 = proc->page_table ? proc->page_table : vmm_get_kernel_pml4();

    // Address selection, VMA publication and PTE installation under one
    // vma-lock hold, mirroring the sys_mmap anonymous path: munmap and
    // fork's clone snapshot under this same lock, so nothing can steal the
    // frames of a half-installed mapping.
    const uint64_t lock = spinlock_acquire_irqsave(proc->vma_lock_ptr);

    uint64_t start = kTlsMapFloor;
    for (bool overlap = true; overlap;) {
        overlap = false;
        for (const VMA *curr = proc->vmalist->head; curr; curr = curr->next) {
            if (start < curr->end && start + map_len > curr->start) {
                start = (curr->end + kTlsPage - 1) & ~(kTlsPage - 1);
                overlap = true;
                break;
            }
        }
    }
    if (start + map_len >= kTlsCeiling) {
        spinlock_release_irqrestore(proc->vma_lock_ptr, lock);
        return 0;
    }

    const uint64_t pte_flags = PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX;
    if (!vma_add(&proc->vmalist->head, start, start + map_len, pte_flags, VMAType::Data)) {
        spinlock_release_irqrestore(proc->vma_lock_ptr, lock);
        return 0;
    }

    // Unwind on any failure: free the frames mapped so far, remove the
    // just-published VMA. vmm_unmap_page_in invalidates the TLB entry, so
    // the frames are safe to hand back to the PMM.
    uint64_t mapped = 0;
    for (; mapped < map_len; mapped += kTlsPage) {
        void *frame = pmm_alloc_frame();
        if (!frame)
            break;
        if (!vmm_map_page_in(target_pml4, start + mapped, reinterpret_cast<uint64_t>(frame), pte_flags).ok()) {
            pmm_free_frame(frame);
            break;
        }
        kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(frame))), kTlsPage);
    }

    const uint64_t fs_base = start + padded;
    UniTcb tcb;
    tcb.self = fs_base;
    tcb.tid = proc->pid;

    // The linker's TPOFF offsets run against the PADDED size: the template
    // lands at [fs_base - padded, fs_base - padded + tls_size) and the
    // alignment gap sits directly below the TCB, zero by the fresh frames.
    // Placing the template at fs_base - tls_size instead shifts every
    // __thread variable below the gap whenever tls_size is not a multiple
    // of the alignment - silent variable corruption.
    const bool installed =
        mapped == map_len &&
        (tls_size == 0 || tls_write_user_range(target_pml4, fs_base - padded, template_src, tls_size)) &&
        tls_write_user_range(target_pml4, fs_base, &tcb, sizeof(tcb));

    if (!installed) {
        while (mapped > 0) {
            mapped -= kTlsPage;
            const uint64_t phys = vmm_virt_to_phys_in(target_pml4, start + mapped);
            vmm_unmap_page_in(target_pml4, start + mapped);
            if (phys != 0)
                pmm_free_frame(reinterpret_cast<void *>(phys));
        }
        vma_remove(&proc->vmalist->head, start, start + map_len);
        spinlock_release_irqrestore(proc->vma_lock_ptr, lock);
        return 0;
    }

    proc->fs_base = fs_base;
    spinlock_release_irqrestore(proc->vma_lock_ptr, lock);
    return start;
}
