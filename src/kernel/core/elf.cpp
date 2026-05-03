#include <kernel/debug.h>
#include <kernel/elf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <libk/kstring.h>
#include <stddef.h>
#include <stdint.h>

static constexpr uint64_t k_user_stack_top = 0x0000700000000000ULL;
static constexpr uint64_t k_page_size = 0x1000ULL;

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
    }

    return true;
}

[[nodiscard]] static bool ensure_segment_vma(Process *proc, uint64_t start, uint64_t end, uint64_t flags, VMAType type)
{
    if (!proc)
        return true;

    VMA *prev = nullptr;
    VMA *curr = proc->vma_list;
    while (curr && curr->end < start) {
        prev = curr;
        curr = curr->next;
    }

    if (!curr || end < curr->start)
        return vma_add(&proc->vma_list, start, end, flags, type) != nullptr;

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
            uint64_t existing_flags = vmm_get_page_flags_in(target_pml4, page_vaddr);
            uint64_t merged_flags = existing_flags | flags;
            // If either the existing mapping or the new segment is executable, keep the page executable.
            if (((existing_flags & PTE_NX) == 0) || ((flags & PTE_NX) == 0))
                merged_flags &= ~PTE_NX;
            if (!vmm_map_page_in(target_pml4, page_vaddr, phys & ~0xFFFULL, merged_flags).ok())
                return false;
        } else {
            uint64_t merged_flags = flags;
            if (!(phdr.p_flags & PF_X))
                merged_flags |= PTE_NX;
            else
                merged_flags &= ~PTE_NX;
            if (!vmm_map_page(page_vaddr, phys & ~0xFFFULL, merged_flags).ok())
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

    constexpr int USER_STACK_PAGES = 8; // 32 KB default stack
    const uint64_t stack_base = k_user_stack_top - (USER_STACK_PAGES * k_page_size);
    if (proc) {
        if (!vma_add(&proc->vma_list, stack_base, k_user_stack_top, PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NX,
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
        const bool mapped = target_pml4 ? vmm_map_page_in(target_pml4, vaddr, frame_phys,
                                                          PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)
                                               .ok()
                                       : vmm_map_page(vaddr, frame_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)
                                             .ok();
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
