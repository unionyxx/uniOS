#include <kernel/elf.h>
#include <kernel/mm/vmm.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/vma.h>
#include <kernel/process.h>
#include <libk/kstring.h>
#include <stddef.h>

// Use kstring memory utilities
using kstring::memset;
using kstring::memcpy;

bool elf_validate(const uint8_t* data, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) return false;
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    
    if (*(uint32_t*)ehdr->e_ident != ELF_MAGIC) return false;
    if (ehdr->e_ident[4] != ELFCLASS64) return false;
    if (ehdr->e_ident[5] != ELFDATA2LSB) return false;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return false;
    if (ehdr->e_machine != EM_X86_64) return false;
    
    return true;
}

uint64_t elf_load(const uint8_t* data, uint64_t size, Process* proc) {
    if (!elf_validate(data, size)) return 0;
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)(data + ehdr->e_phoff);
    
    uint64_t* target_pml4 = proc ? proc->page_table : nullptr;

    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        
        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        
        // Allocate pages for this segment and copy data
        uint64_t num_pages = (memsz + 0xFFF) / 0x1000;
        uint64_t bytes_copied = 0;
        
        uint64_t flags = PTE_PRESENT;
        if (phdr[i].p_flags & PF_W) flags |= PTE_WRITABLE;
        // If user-accessible, add user flag
        if (phdr[i].p_flags & PF_R) flags |= PTE_USER;

        if (proc) {
            vma_add(&proc->vma_list, vaddr & ~0xFFF, (vaddr + memsz + 0xFFF) & ~0xFFF, 
                    flags, (phdr[i].p_flags & PF_X) ? VMA_TEXT : VMA_DATA);
        }
        
        for (uint64_t p = 0; p < num_pages; p++) {
            void* frame = pmm_alloc_frame();
            if (!frame) return 0;
            
            uint64_t page_vaddr = (vaddr & ~0xFFF) + (p * 0x1000);
            
            if (target_pml4) {
                vmm_map_page_in(target_pml4, page_vaddr, (uint64_t)frame, flags);
            } else {
                vmm_map_page(page_vaddr, (uint64_t)frame, flags);
            }
            
            // Copy data to this page through higher-half mapping
            void* dest = (void*)vmm_phys_to_virt((uint64_t)frame);
            
            // Calculate how much of this page has file data
            uint64_t vaddr_offset = vaddr & 0xFFF; // Offset within first page
            
            // Zero the page first
            memset(dest, 0, 0x1000);
            
            // Copy file data if this page has any
            if (bytes_copied < filesz) {
                uint64_t copy_start = (p == 0) ? vaddr_offset : 0;
                uint64_t copy_amount = 0x1000 - copy_start;
                if (bytes_copied + copy_amount > filesz) {
                    copy_amount = filesz - bytes_copied;
                }
                
                if (copy_amount > 0) {
                    memcpy((uint8_t*)dest + copy_start, data + offset + bytes_copied, copy_amount);
                    bytes_copied += copy_amount;
                }
            }
        }
    }
    
    return ehdr->e_entry;
}

// Load ELF for Ring 3 execution (with user flag on all pages)
uint64_t elf_load_user(const uint8_t* data, uint64_t size, Process* proc) {
    if (!elf_validate(data, size)) return 0;
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)(data + ehdr->e_phoff);
    
    uint64_t* target_pml4 = proc ? proc->page_table : nullptr;

    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        
        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        
        uint64_t num_pages = (memsz + 0xFFF) / 0x1000;
        uint64_t bytes_copied = 0;
        
        uint64_t flags = PTE_PRESENT | PTE_USER;
        if (phdr[i].p_flags & PF_W) flags |= PTE_WRITABLE;

        if (proc) {
            vma_add(&proc->vma_list, vaddr & ~0xFFF, (vaddr + memsz + 0xFFF) & ~0xFFF, 
                    flags, (phdr[i].p_flags & PF_X) ? VMA_TEXT : VMA_DATA);
        }

        for (uint64_t p = 0; p < num_pages; p++) {
            void* frame = pmm_alloc_frame();
            if (!frame) return 0;
            
            uint64_t page_vaddr = (vaddr & ~0xFFF) + (p * 0x1000);
            
            if (target_pml4) {
                vmm_map_page_in(target_pml4, page_vaddr, (uint64_t)frame, flags);
            } else {
                vmm_map_page(page_vaddr, (uint64_t)frame, flags);
            }
            
            void* dest = (void*)vmm_phys_to_virt((uint64_t)frame);
            memset(dest, 0, 0x1000);
            
            if (bytes_copied < filesz) {
                uint64_t copy_start = (p == 0) ? (vaddr & 0xFFF) : 0;
                uint64_t copy_amount = 0x1000 - copy_start;
                if (bytes_copied + copy_amount > filesz) {
                    copy_amount = filesz - bytes_copied;
                }
                
                if (copy_amount > 0) {
                    memcpy((uint8_t*)dest + copy_start, data + offset + bytes_copied, copy_amount);
                    bytes_copied += copy_amount;
                }
            }
        }
    }
    
    // Also map a user stack (64KB = 16 pages) at USER_STACK_TOP
    // Stack grows down, so we map pages below USER_STACK_TOP
    #define USER_STACK_PAGES 16
    #define USER_STACK_SIZE (USER_STACK_PAGES * 0x1000)
    #define USER_STACK_TOP 0x7FFFF000ULL
    
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_SIZE;
    if (proc) {
        vma_add(&proc->vma_list, stack_base, USER_STACK_TOP, PTE_PRESENT | PTE_WRITABLE | PTE_USER, VMA_STACK);
    }

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        void* stack_frame = pmm_alloc_frame();
        if (stack_frame) {
            uint64_t vaddr = stack_base + i * 0x1000;
            if (target_pml4) {
                vmm_map_page_in(target_pml4, vaddr, (uint64_t)stack_frame, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            } else {
                vmm_map_page(vaddr, (uint64_t)stack_frame, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
            }
            void* stack_dest = (void*)vmm_phys_to_virt((uint64_t)stack_frame);
            memset(stack_dest, 0, 0x1000);
        }
    }
    
    return ehdr->e_entry;
}
