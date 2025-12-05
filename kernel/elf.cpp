#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"
#include <stddef.h>

// Helper to copy memory
static void memcpy(void* dest, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}

// Helper to zero memory
static void memset(void* dest, uint8_t val, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = val;
}

bool elf_validate(const uint8_t* data, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) return false;
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    
    // Check magic
    if (*(uint32_t*)ehdr->e_ident != ELF_MAGIC) return false;
    
    // Check class (64-bit)
    if (ehdr->e_ident[4] != ELFCLASS64) return false;
    
    // Check endianness (little)
    if (ehdr->e_ident[5] != ELFDATA2LSB) return false;
    
    // Check type (executable)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return false;
    
    // Check machine (x86_64)
    if (ehdr->e_machine != EM_X86_64) return false;
    
    return true;
}

uint64_t elf_load(const uint8_t* data, uint64_t size) {
    if (!elf_validate(data, size)) return 0;
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)(data + ehdr->e_phoff);
    
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
        
        for (uint64_t p = 0; p < num_pages; p++) {
            void* frame = pmm_alloc_frame();
            if (!frame) return 0;
            
            uint64_t page_vaddr = (vaddr & ~0xFFF) + (p * 0x1000);
            uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
            
            // If user-accessible, add user flag
            if (phdr[i].p_flags & PF_R) flags |= PTE_USER;
            
            vmm_map_page(page_vaddr, (uint64_t)frame, flags);
            
            // Copy data to this page through higher-half mapping
            void* dest = (void*)vmm_phys_to_virt((uint64_t)frame);
            
            // Calculate how much of this page has file data
            uint64_t page_offset = p * 0x1000;
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
uint64_t elf_load_user(const uint8_t* data, uint64_t size) {
    if (!elf_validate(data, size)) return 0;
    
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    const Elf64_Phdr* phdr = (const Elf64_Phdr*)(data + ehdr->e_phoff);
    
    // Load each PT_LOAD segment
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) continue;
        
        uint64_t vaddr = phdr[i].p_vaddr;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t memsz = phdr[i].p_memsz;
        uint64_t offset = phdr[i].p_offset;
        
        uint64_t num_pages = (memsz + 0xFFF) / 0x1000;
        uint64_t bytes_copied = 0;
        
        for (uint64_t p = 0; p < num_pages; p++) {
            void* frame = pmm_alloc_frame();
            if (!frame) return 0;
            
            uint64_t page_vaddr = (vaddr & ~0xFFF) + (p * 0x1000);
            // Always set USER flag for Ring 3
            uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            
            vmm_map_page(page_vaddr, (uint64_t)frame, flags);
            
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
    
    // Also map a user stack at 0x7FFF0000
    void* stack_frame = pmm_alloc_frame();
    if (stack_frame) {
        vmm_map_page(0x7FFF0000, (uint64_t)stack_frame, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        void* stack_dest = (void*)vmm_phys_to_virt((uint64_t)stack_frame);
        memset(stack_dest, 0, 0x1000);
    }
    
    return ehdr->e_entry;
}
