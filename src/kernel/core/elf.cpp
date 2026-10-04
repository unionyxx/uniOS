#include <kernel/debug.h>
#include <kernel/elf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/tls.h>
#include <libk/kstring.h>
#include <stddef.h>
#include <stdint.h>

static constexpr uint64_t k_user_stack_top = 0x0000700000000000ULL;
static constexpr uint64_t k_page_size = 0x1000ULL;
// Everything loadable must stay in the user half of the address space; a
// segment reaching into the kernel half could remap kernel pages through
// load_segment's flag-merge path.
static constexpr uint64_t k_user_address_limit = 0x0000800000000000ULL;

[[nodiscard]] static bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    return __builtin_add_overflow(a, b, out);
}

[[nodiscard]] static bool mul_overflow_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    return __builtin_mul_overflow(a, b, out);
}

[[nodiscard]] bool elf_validate(const uint8_t *data, uint64_t size)
{
    if (!data || size < sizeof(Elf64_Ehdr))
        return false;
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(data);
    if (*reinterpret_cast<const uint32_t *>(ehdr->e_ident) != ELF_MAGIC)
        return false;
    if (ehdr->e_ident[4] != ELFCLASS64)
        return false;
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        return false;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return false;
    if (ehdr->e_machine != EM_X86_64)
        return false;
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr) || ehdr->e_phnum == 0)
        return false;
    if (ehdr->e_entry >= k_user_address_limit)
        return false;

    uint64_t phdr_bytes = 0;
    if (mul_overflow_u64(ehdr->e_phnum, sizeof(Elf64_Phdr), &phdr_bytes))
        return false;

    uint64_t phdr_end = 0;
    if (add_overflow_u64(ehdr->e_phoff, phdr_bytes, &phdr_end))
        return false;
    if (ehdr->e_phoff > size || phdr_end > size)
        return false;

    const auto *phdr = reinterpret_cast<const Elf64_Phdr *>(data + ehdr->e_phoff);
    bool entry_covered = false;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;
        if (phdr[i].p_filesz > phdr[i].p_memsz)
            return false;
        uint64_t file_end = 0;
        if (add_overflow_u64(phdr[i].p_offset, phdr[i].p_filesz, &file_end) || file_end > size)
            return false;
        uint64_t mem_end = 0;
        if (add_overflow_u64(phdr[i].p_vaddr, phdr[i].p_memsz, &mem_end))
            return false;
        if (phdr[i].p_vaddr >= k_user_address_limit || mem_end > k_user_address_limit)
            return false;
        if (phdr[i].p_memsz > 0 && ehdr->e_entry >= phdr[i].p_vaddr && ehdr->e_entry < mem_end)
            entry_covered = true;

        // Overlapping PT_LOAD segments would merge their page permissions
        // (writable AND executable) and double-map physical frames. uniOS
        // binaries are single-segment; reject anything overlapping outright.
        for (uint16_t j = 0; j < i; j++) {
            if (phdr[j].p_type != PT_LOAD || phdr[j].p_memsz == 0 || phdr[i].p_memsz == 0)
                continue;
            uint64_t other_end = 0;
            if (add_overflow_u64(phdr[j].p_vaddr, phdr[j].p_memsz, &other_end))
                return false;
            if (phdr[i].p_vaddr < other_end && phdr[j].p_vaddr < mem_end)
                return false;
        }
    }

    // The entry point must land inside an actual segment; a dangling entry
    // would fault before the first instruction and is indistinguishable from
    // loader failure further up the stack.
    return entry_covered;
}

[[nodiscard]] bool elf_tls_info(const uint8_t *data, uint64_t size, uint64_t *template_offset, uint64_t *memsz,
                                uint64_t *align, uint64_t *template_vaddr, uint64_t *filesz, ElfTlsMalform *malformed)
{
    if (template_offset)
        *template_offset = 0;
    if (memsz)
        *memsz = 0;
    if (align)
        *align = 0;
    if (template_vaddr)
        *template_vaddr = 0;
    if (filesz)
        *filesz = 0;
    if (malformed)
        *malformed = ElfTlsMalform::None;
    if (!data || !template_offset || !memsz || !align)
        return false;

    if (size < sizeof(Elf64_Ehdr))
        return false;
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(data);
    if (*reinterpret_cast<const uint32_t *>(ehdr->e_ident) != ELF_MAGIC)
        return false;
    if (ehdr->e_ident[4] != ELFCLASS64)
        return false;
    if (ehdr->e_ident[5] != ELFDATA2LSB)
        return false;
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr) || ehdr->e_phnum == 0)
        return false;

    uint64_t phdr_bytes = 0;
    if (mul_overflow_u64(ehdr->e_phnum, sizeof(Elf64_Phdr), &phdr_bytes))
        return false;
    uint64_t phdr_end = 0;
    if (add_overflow_u64(ehdr->e_phoff, phdr_bytes, &phdr_end))
        return false;
    if (ehdr->e_phoff > size || phdr_end > size)
        return false;

    const auto *phdr = reinterpret_cast<const Elf64_Phdr *>(data + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_TLS)
            continue;
        // Same sanity rules elf_validate applies to PT_LOAD: the file bytes
        // must stay inside the image and filesz must not exceed memsz —
        // the exec path memcpy's filesz bytes out of the image and sizes
        // its bounce buffer by memsz. A violation of a PRESENT PT_TLS is
        // reported through the malformed out so the exec refusal can name
        // the field; image-level failures (truncated table, bad identity)
        // stay None — elf_validate owns those.
        if (phdr[i].p_filesz > phdr[i].p_memsz) {
            if (malformed)
                *malformed = ElfTlsMalform::FileszOverMemsz;
            return false;
        }
        uint64_t file_end = 0;
        if (add_overflow_u64(phdr[i].p_offset, phdr[i].p_filesz, &file_end) || file_end > size) {
            if (malformed)
                *malformed = ElfTlsMalform::OffsetBounds;
            return false;
        }
        // The recorded vaddr becomes the clone source for sys_thread_create
        // (safe_copy_from_user, a fault-fixup walk): without a user-half
        // bound a crafted header points the clone at the kernel's mapped
        // half and copies kernel memory into a user TLS block.
        constexpr uint64_t kKernelHalfFloor = 0xFFFF800000000000ULL;
        uint64_t template_end = 0;
        if (add_overflow_u64(phdr[i].p_vaddr, phdr[i].p_memsz, &template_end) || template_end > kKernelHalfFloor) {
            if (malformed)
                *malformed = ElfTlsMalform::VaddrBounds;
            return false;
        }
        // A non-power-of-two p_align cannot position the block under the
        // thread pointer; report it here so the refusal names the field
        // instead of failing deep inside tls_install.
        if (phdr[i].p_align != 0 && (phdr[i].p_align & (phdr[i].p_align - 1)) != 0) {
            if (malformed)
                *malformed = ElfTlsMalform::AlignNotPowerOfTwo;
            return false;
        }
        *template_offset = phdr[i].p_offset;
        *memsz = phdr[i].p_memsz;
        *align = phdr[i].p_align;
        if (template_vaddr)
            *template_vaddr = phdr[i].p_vaddr;
        if (filesz)
            *filesz = phdr[i].p_filesz;
        return true;
    }
    return false;
}

[[nodiscard]] static const char *elf_tls_malform_field(ElfTlsMalform malform)
{
    switch (malform) {
        case ElfTlsMalform::FileszOverMemsz:
            return "p_filesz exceeds p_memsz";
        case ElfTlsMalform::OffsetBounds:
            return "p_offset + p_filesz leaves the image";
        case ElfTlsMalform::VaddrBounds:
            return "p_vaddr + p_memsz leaves the user half";
        case ElfTlsMalform::AlignNotPowerOfTwo:
            return "p_align is not a power of two";
        case ElfTlsMalform::None:
            break;
    }
    return "none";
}

[[nodiscard]] bool elf_install_tls(Process *proc, const uint8_t *image, uint64_t image_size)
{
    uint64_t offset = 0, memsz = 0, align = 0, vaddr = 0, filesz = 0;
    ElfTlsMalform malform = ElfTlsMalform::None;
    if (!elf_tls_info(image, image_size, &offset, &memsz, &align, &vaddr, &filesz, &malform)) {
        if (malform != ElfTlsMalform::None) {
            // One policy for every malformed PT_TLS: refuse the exec and
            // name the field on serial. The old split silently degraded
            // filesz/offset/vaddr violations to a TCB-only install (an app
            // mysteriously without __thread storage) while a bad p_align
            // hard-failed inside tls_install with no field named.
            DEBUG_ERROR("exec: refusing image with malformed PT_TLS: %s", elf_tls_malform_field(malform));
            return false;
        }
        // No usable PT_TLS: outputs are zeroed, so this is the TCB-only
        // install of the always-TCB invariant.
        memsz = 0;
        align = 0;
    }

    // Bounce the template through the kernel heap: tls_install reads its
    // source as a plain kernel pointer, and the image's file bytes stop at
    // filesz — the .tbss tail ([filesz, memsz)) must be zeroed here, the
    // file never carries it.
    uint8_t *template_src = nullptr;
    if (memsz > 0) {
        template_src = static_cast<uint8_t *>(malloc(memsz));
        if (!template_src)
            return false;
        kstring::copy_memory(template_src, image + offset, filesz);
        kstring::zero_memory(template_src + filesz, memsz - filesz);
    }

    const uint64_t start = tls_install(proc, template_src, memsz, align);
    free(template_src);
    return start != 0;
}

[[nodiscard]] static bool ensure_segment_vma(Process *proc, uint64_t start, uint64_t end, uint64_t flags, VMAType type)
{
    if (!proc)
        return true;

    VMA *prev = nullptr;
    VMA *curr = proc->vmalist->head;
    while (curr && curr->end < start) {
        prev = curr;
        curr = curr->next;
    }

    if (!curr || end < curr->start)
        return vma_add(&proc->vmalist->head, start, end, flags, type) != nullptr;

    curr->start = curr->start < start ? curr->start : start;
    curr->end = curr->end > end ? curr->end : end;
    curr->flags |= flags;
    if (type == VMAType::Data)
        curr->type = VMAType::Data;

    while (curr->next && curr->next->start <= curr->end) {
        VMA *next = curr->next;
        curr->end = curr->end > next->end ? curr->end : next->end;
        curr->flags |= next->flags;
        if (next->type == VMAType::Data)
            curr->type = VMAType::Data;
        curr->next = next->next;
        free(next);
    }

    if (prev && prev->end >= curr->start) {
        prev->end = prev->end > curr->end ? prev->end : curr->end;
        prev->flags |= curr->flags;
        if (curr->type == VMAType::Data)
            prev->type = VMAType::Data;
        prev->next = curr->next;
        free(curr);
    }

    return true;
}

static void rollback_loaded_page(uint64_t *target_pml4, uint64_t vaddr, uint64_t phys)
{
    if (target_pml4)
        vmm_unmap_page_in(target_pml4, vaddr);
    if (phys)
        pmm_free_frame(reinterpret_cast<void *>(phys & ~0xFFFULL));
}

[[nodiscard]] static bool load_segment(const uint8_t *data, uint64_t data_size, const Elf64_Phdr &phdr,
                                       uint64_t *target_pml4, Process *proc, bool is_user)
{
    const uint64_t vaddr = phdr.p_vaddr;
    const uint64_t filesz = phdr.p_filesz;
    const uint64_t memsz = phdr.p_memsz;
    const uint64_t offset = phdr.p_offset;
    if (memsz == 0)
        return true;
    if (filesz > memsz)
        return false;

    uint64_t file_end = 0;
    if (add_overflow_u64(offset, filesz, &file_end) || file_end > data_size)
        return false;

    uint64_t segment_end = 0;
    uint64_t rounded_segment_end = 0;
    if (add_overflow_u64(vaddr, memsz, &segment_end) ||
        add_overflow_u64(segment_end, k_page_size - 1, &rounded_segment_end)) {
        return false;
    }

    const uint64_t page_base = vaddr & ~(k_page_size - 1);
    const uint64_t page_end = rounded_segment_end & ~(k_page_size - 1);
    if (page_end < page_base)
        return false;
    const uint64_t num_pages = (page_end - page_base) / k_page_size;

    uint64_t flags = PTE_PRESENT | (is_user ? PTE_USER : 0);
    if (phdr.p_flags & PF_W)
        flags |= PTE_WRITABLE;
    if (!(phdr.p_flags & PF_X))
        flags |= PTE_NX;

    if (proc) {
        VMAType type = (phdr.p_flags & PF_X) ? VMAType::Text : VMAType::Data;
        if (!ensure_segment_vma(proc, page_base, page_end, flags, type)) {
            DEBUG_ERROR("load_segment: failed to add VMA");
            return false;
        }
    }

    uint64_t bytes_copied = 0;
    for (uint64_t p = 0; p < num_pages; p++) {
        const uint64_t page_vaddr = page_base + (p * k_page_size);
        uint64_t phys = target_pml4 ? vmm_virt_to_phys_in(target_pml4, page_vaddr) : vmm_virt_to_phys(page_vaddr);
        if (phys != 0 && !pmm_is_managed(reinterpret_cast<void *>(phys & ~(k_page_size - 1)))) {
            // A page-table entry yielded a physical address outside managed
            // RAM: the tables were corrupted before we got here. Fail loudly
            // instead of dereferencing a bogus HHDM address.
            DEBUG_ERROR("load_segment: corrupt phys 0x%llx for vaddr 0x%llx (segment vaddr 0x%llx memsz 0x%llx)",
                        (unsigned long long)phys, (unsigned long long)page_vaddr, (unsigned long long)vaddr,
                        (unsigned long long)memsz);
            panic("load_segment: corrupt physical address in page tables");
        }
        if (phys == 0) {
            void *frame = pmm_alloc_frame();
            if (!frame)
                return false;
            phys = reinterpret_cast<uint64_t>(frame);
            const bool mapped = target_pml4 ? vmm_map_page_in(target_pml4, page_vaddr, phys, flags).ok()
                                            : vmm_map_page(page_vaddr, phys, flags).ok();
            if (!mapped) {
                pmm_free_frame(frame);
                return false;
            }
            kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(phys)), k_page_size);
        } else if (target_pml4) {
            // A page shared with an earlier mapping (cannot happen for
            // validated non-overlapping segments; kept for robustness): keep
            // the frame, refresh permissions in place.
            uint64_t existing_flags = vmm_get_page_flags_in(target_pml4, page_vaddr);
            uint64_t merged_flags = existing_flags | flags;
            if (((existing_flags & PTE_NX) == 0) || ((flags & PTE_NX) == 0))
                merged_flags &= ~PTE_NX;
            if (!vmm_replace_page_in(target_pml4, page_vaddr, phys & ~0xFFFULL, merged_flags).ok())
                return false;
        } else {
            uint64_t merged_flags = flags;
            if (!(phdr.p_flags & PF_X))
                merged_flags |= PTE_NX;
            else
                merged_flags &= ~PTE_NX;
            if (!vmm_replace_page(page_vaddr, phys & ~0xFFFULL, merged_flags).ok())
                return false;
        }

        uint8_t *dest = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(phys & ~0xFFFULL));
        if (bytes_copied < filesz) {
            uint64_t copy_start = (p == 0) ? (vaddr & (k_page_size - 1)) : 0;
            uint64_t copy_amount = k_page_size - copy_start;
            if (bytes_copied + copy_amount > filesz)
                copy_amount = filesz - bytes_copied;
            if (copy_amount > 0) {
                kstring::memcpy(dest + copy_start, data + offset + bytes_copied, copy_amount);
                bytes_copied += copy_amount;
            }
        }

        // Zero the BSS tail of this page ([filesz, memsz)) even when the page
        // already existed, so no stale file/segment data leaks into BSS.
        // zero_start must be clamped to the page: on pages deep in the BSS
        // (vaddr + filesz below page_vaddr) the unclamped subtraction wraps
        // dest backwards and the zero-fill lands in unrelated physical memory
        // below the segment's frame — silently shredding whatever lives there.
        const uint64_t seg_hi =
            (page_vaddr + k_page_size) < (vaddr + memsz) ? (page_vaddr + k_page_size) : (vaddr + memsz);
        const uint64_t file_hi = seg_hi < (vaddr + filesz) ? seg_hi : (vaddr + filesz);
        const uint64_t zero_start = file_hi > page_vaddr ? file_hi : page_vaddr;
        if (seg_hi > zero_start)
            kstring::zero_memory(dest + (zero_start - page_vaddr), seg_hi - zero_start);
    }
    return true;
}

[[nodiscard]] uint64_t elf_load(const uint8_t *data, uint64_t size, Process *proc)
{
    if (!elf_validate(data, size))
        return 0;
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(data);
    const auto *phdr = reinterpret_cast<const Elf64_Phdr *>(data + ehdr->e_phoff);
    uint64_t *target_pml4 = proc ? proc->page_table : nullptr;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && !load_segment(data, size, phdr[i], target_pml4, proc, false))
            return 0;
    }
    return ehdr->e_entry;
}

[[nodiscard]] uint64_t elf_load_user(const uint8_t *data, uint64_t size, Process *proc)
{
    if (!elf_validate(data, size))
        return 0;
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(data);
    const auto *phdr = reinterpret_cast<const Elf64_Phdr *>(data + ehdr->e_phoff);
    uint64_t *target_pml4 = proc ? proc->page_table : nullptr;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && !load_segment(data, size, phdr[i], target_pml4, proc, true))
            return 0;
    }

    // Record the image's TLS facts for the exec / boot-launch install that
    // follows. Only the file-backed prefix [p_vaddr, p_vaddr + p_filesz)
    // is ever read back: the exec path copies from this image buffer, and
    // thread creations walk the live user space. The .tbss tail VAs are
    // never read - see the template clone in sys_thread_create. No PT_TLS
    // -> all zero, matching an absent template. Set unconditionally: a
    // loader copied from a live process carries the OLD image's values.
    if (proc) {
        uint64_t t_offset = 0, t_memsz = 0, t_align = 0, t_vaddr = 0, t_filesz = 0;
        if (elf_tls_info(data, size, &t_offset, &t_memsz, &t_align, &t_vaddr, &t_filesz)) {
            proc->tls_template_va = t_vaddr;
            proc->tls_template_size = t_memsz;
            proc->tls_template_filesz = t_filesz;
            proc->tls_align = t_align;
        } else {
            proc->tls_template_va = 0;
            proc->tls_template_size = 0;
            proc->tls_template_filesz = 0;
            proc->tls_align = 0;
        }
    }

    constexpr int USER_STACK_PAGES = 8; // 32 KB default stack
    const uint64_t stack_base = k_user_stack_top - (USER_STACK_PAGES * k_page_size);
    if (proc) {
        if (!vma_add(&proc->vmalist->head, stack_base, k_user_stack_top, PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX,
                     VMAType::Stack)) {
            DEBUG_ERROR("elf_load_user: failed to add stack VMA");
            return 0;
        }
    }

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void *frame = pmm_alloc_frame();
        if (!frame) {
            for (int j = 0; j < i; j++) {
                const uint64_t vaddr = stack_base + static_cast<uint64_t>(j) * k_page_size;
                const uint64_t phys = target_pml4 ? vmm_virt_to_phys_in(target_pml4, vaddr) : 0;
                rollback_loaded_page(target_pml4, vaddr, phys);
            }
            return 0;
        }

        const uint64_t vaddr = stack_base + static_cast<uint64_t>(i) * k_page_size;
        const uint64_t frame_phys = reinterpret_cast<uint64_t>(frame);
        const bool mapped =
            target_pml4
                ? vmm_map_page_in(target_pml4, vaddr, frame_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX).ok()
                : vmm_map_page(vaddr, frame_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX).ok();
        if (!mapped) {
            pmm_free_frame(frame);
            for (int j = 0; j < i; j++) {
                const uint64_t old_vaddr = stack_base + static_cast<uint64_t>(j) * k_page_size;
                const uint64_t phys = target_pml4 ? vmm_virt_to_phys_in(target_pml4, old_vaddr) : 0;
                rollback_loaded_page(target_pml4, old_vaddr, phys);
            }
            return 0;
        }
        kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(frame_phys)), k_page_size);
    }
    return ehdr->e_entry;
}
