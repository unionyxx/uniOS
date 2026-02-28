#include <kernel/elf.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vma.h>
#include <kernel/process.h>
#include <libk/kstring.h>
#include <stddef.h>

[[nodiscard]] bool elf_validate(const uint8_t* data, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) return false;
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
    if (*reinterpret_cast<const uint32_t*>(ehdr->e_ident) != ELF_MAGIC) return false;
    if (ehdr->e_ident[4] != ELFCLASS64) return false;
    if (ehdr->e_ident[5] != ELFDATA2LSB) return false;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return false;
    return ehdr->e_machine == EM_X86_64;
}

static void load_segment(const uint8_t* data, const Elf64_Phdr& phdr, uint64_t* target_pml4, Process* proc, bool is_user) {
    uint64_t vaddr = phdr.p_vaddr;
    uint64_t filesz = phdr.p_filesz;
    uint64_t memsz = phdr.p_memsz;
    uint64_t offset = phdr.p_offset;
    uint64_t num_pages = (memsz + 0xFFF) / 0x1000;
    uint64_t flags = PTE_PRESENT | (is_user ? PTE_USER : 0);
    if (phdr.p_flags & PF_W) flags |= PTE_WRITABLE;
    if (!is_user && (phdr.p_flags & PF_R)) flags |= PTE_USER;

    if (proc) static_cast<void>(vma_add(&proc->vma_list, vaddr & ~0xFFFULL, (vaddr + memsz + 0xFFFULL) & ~0xFFFULL, flags, (phdr.p_flags & PF_X) ? VMAType::Text : VMAType::Data));

    uint64_t bytes_copied = 0;
    for (uint64_t p = 0; p < num_pages; p++) {
        void* frame = pmm_alloc_frame();
        if (!frame) return;
        uint64_t page_vaddr = (vaddr & ~0xFFFULL) + (p * 0x1000);
        if (target_pml4) vmm_map_page_in(target_pml4, page_vaddr, reinterpret_cast<uint64_t>(frame), flags);
        else vmm_map_page(page_vaddr, reinterpret_cast<uint64_t>(frame), flags);

        uint8_t* dest = reinterpret_cast<uint8_t*>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(frame)));
        kstring::zero_memory(dest, 0x1000);
        if (bytes_copied < filesz) {
            uint64_t copy_start = (p == 0) ? (vaddr & 0xFFFULL) : 0;
            uint64_t copy_amount = 0x1000 - copy_start;
            if (bytes_copied + copy_amount > filesz) copy_amount = filesz - bytes_copied;
            if (copy_amount > 0) {
                kstring::memcpy(dest + copy_start, data + offset + bytes_copied, copy_amount);
                bytes_copied += copy_amount;
            }
        }
    }
}

[[nodiscard]] uint64_t elf_load(const uint8_t* data, uint64_t size, Process* proc) {
    if (!elf_validate(data, size)) return 0;
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
    const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
    uint64_t* target_pml4 = proc ? proc->page_table : nullptr;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) load_segment(data, phdr[i], target_pml4, proc, false);
    }
    return ehdr->e_entry;
}

[[nodiscard]] uint64_t elf_load_user(const uint8_t* data, uint64_t size, Process* proc) {
    if (!elf_validate(data, size)) return 0;
    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
    const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
    uint64_t* target_pml4 = proc ? proc->page_table : nullptr;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) load_segment(data, phdr[i], target_pml4, proc, true);
    }

    constexpr int USER_STACK_PAGES = 16;
    constexpr uint64_t USER_STACK_TOP = 0x7FFFF000ULL;
    const uint64_t stack_base = USER_STACK_TOP - (USER_STACK_PAGES * 0x1000);
    if (proc) static_cast<void>(vma_add(&proc->vma_list, stack_base, USER_STACK_TOP, PTE_PRESENT | PTE_WRITABLE | PTE_USER, VMAType::Stack));

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void* frame = pmm_alloc_frame();
        if (frame) {
            uint64_t vaddr = stack_base + i * 0x1000;
            if (target_pml4) vmm_map_page_in(target_pml4, vaddr, reinterpret_cast<uint64_t>(frame), PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            else vmm_map_page(vaddr, reinterpret_cast<uint64_t>(frame), PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            kstring::zero_memory(reinterpret_cast<void*>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(frame))), 0x1000);
        }
    }
    return ehdr->e_entry;
}
