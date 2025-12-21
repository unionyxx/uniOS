#include "vmm.h"
#include "pmm.h"
#include "limine.h"

// Limine HHDM request (Higher Half Direct Map)
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

// Limine Kernel Address request
__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

static uint64_t* pml4 = nullptr;
static uint64_t hhdm_offset = 0;

static uint64_t* get_next_level(uint64_t* current_level, uint64_t index, bool alloc) {
    if (current_level[index] & PTE_PRESENT) {
        uint64_t phys = current_level[index] & 0x000FFFFFFFFFF000;
        return (uint64_t*)(phys + hhdm_offset);
    }

    if (!alloc) return nullptr;

    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;

    uint64_t phys = (uint64_t)frame;
    current_level[index] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    
    uint64_t* next_level = (uint64_t*)(phys + hhdm_offset);
    for (int i = 0; i < 512; i++) next_level[i] = 0; // Clear new page table
    
    return next_level;
}

void vmm_init() {
    if (hhdm_request.response == nullptr) return;
    hhdm_offset = hhdm_request.response->offset;

    // Get current CR3 (Physical address of PML4)
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    
    // Access PML4 via HHDM
    pml4 = (uint64_t*)(cr3 + hhdm_offset);
}

uint64_t vmm_phys_to_virt(uint64_t phys) {
    return phys + hhdm_offset;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = get_next_level(pml4, pml4_index, true);
    if (!pdpt) return;

    uint64_t* pd = get_next_level(pdpt, pdpt_index, true);
    if (!pd) return;

    uint64_t* pt = get_next_level(pd, pd_index, true);
    if (!pt) return;

    pt[pt_index] = phys | flags;
    
    // Invalidate TLB
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_index] & PTE_PRESENT)) return 0;
    uint64_t* pdpt = (uint64_t*)((pml4[pml4_index] & 0x000FFFFFFFFFF000) + hhdm_offset);
    
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) return 0;
    uint64_t* pd = (uint64_t*)((pdpt[pdpt_index] & 0x000FFFFFFFFFF000) + hhdm_offset);
    
    if (!(pd[pd_index] & PTE_PRESENT)) return 0;
    uint64_t* pt = (uint64_t*)((pd[pd_index] & 0x000FFFFFFFFFF000) + hhdm_offset);
    
    if (!(pt[pt_index] & PTE_PRESENT)) return 0;
    
    return (pt[pt_index] & 0x000FFFFFFFFFF000) + (virt & 0xFFF);
}

uint64_t* vmm_get_kernel_pml4() {
    return pml4;
}

// Get the next page table level (with custom pml4)
static uint64_t* get_next_level_in(uint64_t* current_level, uint64_t index, bool alloc) {
    if (current_level[index] & PTE_PRESENT) {
        uint64_t phys = current_level[index] & 0x000FFFFFFFFFF000;
        return (uint64_t*)(phys + hhdm_offset);
    }

    if (!alloc) return nullptr;

    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;

    uint64_t phys = (uint64_t)frame;
    current_level[index] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    
    uint64_t* next_level = (uint64_t*)(phys + hhdm_offset);
    for (int i = 0; i < 512; i++) next_level[i] = 0;
    
    return next_level;
}

void vmm_map_page_in(uint64_t* target_pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = get_next_level_in(target_pml4, pml4_index, true);
    if (!pdpt) return;

    uint64_t* pd = get_next_level_in(pdpt, pdpt_index, true);
    if (!pd) return;

    uint64_t* pt = get_next_level_in(pd, pd_index, true);
    if (!pt) return;

    pt[pt_index] = phys | flags;
}

uint64_t* vmm_create_address_space() {
    // Allocate a new PML4
    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;
    
    uint64_t* new_pml4 = (uint64_t*)((uint64_t)frame + hhdm_offset);
    
    // Clear the new PML4
    for (int i = 0; i < 512; i++) new_pml4[i] = 0;
    
    // Copy kernel mappings (upper half - indices 256-511)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = pml4[i];
    }
    
    return new_pml4;
}

// Get HHDM offset for external use
uint64_t vmm_get_hhdm_offset() {
    return hhdm_offset;
}

// Helper: Clone a page table level (recursive for PDPT -> PD -> PT)
static void clone_page_table_level(uint64_t* src, uint64_t* dst, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PTE_PRESENT)) {
            dst[i] = 0;
            continue;
        }
        
        uint64_t src_phys = src[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t flags = src[i] & 0xFFF;
        
        if (level == 1) {
            // Level 1 = PT (Page Table): Copy the actual physical page
            void* new_frame = pmm_alloc_frame();
            if (!new_frame) {
                dst[i] = 0;
                continue;
            }
            
            // Copy page content
            uint64_t* src_page = (uint64_t*)(src_phys + hhdm_offset);
            uint64_t* dst_page = (uint64_t*)((uint64_t)new_frame + hhdm_offset);
            for (int j = 0; j < 512; j++) {
                dst_page[j] = src_page[j];
            }
            
            dst[i] = (uint64_t)new_frame | flags;
        } else {
            // Levels 2-3: Allocate new table and recurse
            void* new_table = pmm_alloc_frame();
            if (!new_table) {
                dst[i] = 0;
                continue;
            }
            
            uint64_t* new_table_virt = (uint64_t*)((uint64_t)new_table + hhdm_offset);
            uint64_t* src_table = (uint64_t*)(src_phys + hhdm_offset);
            
            // Zero new table first
            for (int j = 0; j < 512; j++) new_table_virt[j] = 0;
            
            // Recursively clone
            clone_page_table_level(src_table, new_table_virt, level - 1);
            
            dst[i] = (uint64_t)new_table | flags;
        }
    }
}

// Clone an entire address space (deep copy user pages, share kernel pages)
uint64_t* vmm_clone_address_space(uint64_t* src_pml4) {
    if (!src_pml4) return nullptr;
    
    // Allocate new PML4
    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;
    
    uint64_t* new_pml4 = (uint64_t*)((uint64_t)frame + hhdm_offset);
    
    // Zero the new PML4
    for (int i = 0; i < 512; i++) new_pml4[i] = 0;
    
    // Copy kernel mappings (upper half - indices 256-511) BY REFERENCE
    // These are shared between all processes
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = src_pml4[i];
    }
    
    // Deep copy user mappings (lower half - indices 0-255)
    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & PTE_PRESENT)) {
            new_pml4[i] = 0;
            continue;
        }
        
        uint64_t src_phys = src_pml4[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t flags = src_pml4[i] & 0xFFF;
        
        // Allocate new PDPT
        void* new_pdpt = pmm_alloc_frame();
        if (!new_pdpt) {
            new_pml4[i] = 0;
            continue;
        }
        
        uint64_t* new_pdpt_virt = (uint64_t*)((uint64_t)new_pdpt + hhdm_offset);
        uint64_t* src_pdpt = (uint64_t*)(src_phys + hhdm_offset);
        
        for (int j = 0; j < 512; j++) new_pdpt_virt[j] = 0;
        
        // Clone PDPT -> PD -> PT -> Pages (level 3 -> 2 -> 1)
        clone_page_table_level(src_pdpt, new_pdpt_virt, 3);
        
        new_pml4[i] = (uint64_t)new_pdpt | flags;
    }
    
    return new_pml4;
}

// Helper: Free a page table level recursively
static void free_page_table_level(uint64_t* table, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT)) continue;
        
        uint64_t phys = table[i] & 0x000FFFFFFFFFF000ULL;
        
        if (level == 1) {
            // Level 1 = PT: Free the physical page
            pmm_free_frame((void*)phys);
        } else {
            // Levels 2-3: Recurse then free table
            uint64_t* sub_table = (uint64_t*)(phys + hhdm_offset);
            free_page_table_level(sub_table, level - 1);
            pmm_free_frame((void*)phys);
        }
    }
}

// Free all user-space pages in an address space
void vmm_free_address_space(uint64_t* target_pml4) {
    if (!target_pml4) return;
    if (target_pml4 == pml4) return;  // Don't free kernel PML4!
    
    // Free user half only (indices 0-255)
    for (int i = 0; i < 256; i++) {
        if (!(target_pml4[i] & PTE_PRESENT)) continue;
        
        uint64_t phys = target_pml4[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t* pdpt = (uint64_t*)(phys + hhdm_offset);
        
        free_page_table_level(pdpt, 3);
        pmm_free_frame((void*)phys);
    }
    
    // Free the PML4 itself
    uint64_t pml4_phys = (uint64_t)target_pml4 - hhdm_offset;
    pmm_free_frame((void*)pml4_phys);
}

void vmm_switch_address_space(uint64_t* new_pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
}

// MMIO virtual address allocator
// Start at a high kernel address that won't conflict with other mappings
static uint64_t mmio_next_virt = 0xFFFFFFFF90000000ULL;

uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size) {
    if (size == 0) return 0;
    
    // Align to page boundary
    uint64_t phys_page = phys_addr & ~0xFFFULL;
    uint64_t offset = phys_addr & 0xFFF;
    uint64_t pages = (size + offset + 0xFFF) / 0x1000;
    
    // Get virtual address for this mapping
    uint64_t virt_base = mmio_next_virt;
    mmio_next_virt += pages * 0x1000;
    
    // Map each page with MMIO flags (uncacheable)
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virt = virt_base + i * 0x1000;
        uint64_t phys = phys_page + i * 0x1000;
        vmm_map_page(virt, phys, PTE_MMIO);
    }
    
    // Return virtual address with original offset
    return virt_base + offset;
}

DMAAllocation vmm_alloc_dma(size_t pages) {
    DMAAllocation alloc = {0, 0, 0};
    
    void* phys_ptr = pmm_alloc_frames(pages);
    if (!phys_ptr) return alloc;
    
    uint64_t phys = (uint64_t)phys_ptr;
    
    // Get virtual address
    uint64_t virt_base = mmio_next_virt;
    mmio_next_virt += pages * 0x1000;
    
    // Map pages
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page(virt_base + i * 0x1000, phys + i * 0x1000, PTE_MMIO);
    }
    
    alloc.virt = virt_base;
    alloc.phys = phys;
    alloc.size = pages * 0x1000;
    
    return alloc;
}

void vmm_remap_framebuffer(uint64_t virt_addr, uint64_t size) {
    if (size == 0) return;
    
    // Align to page boundaries
    uint64_t virt_start = virt_addr & ~0xFFFULL;
    uint64_t virt_end = (virt_addr + size + 0xFFF) & ~0xFFFULL;
    uint64_t pages = (virt_end - virt_start) / 0x1000;
    
    // Remap each page with Write-Combining flags
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virt = virt_start + i * 0x1000;
        
        // Get current physical address
        uint64_t phys = vmm_virt_to_phys(virt);
        if (phys == 0) continue;  // Skip unmapped pages
        
        // Remap with WC flags (this overwrites the existing mapping)
        vmm_map_page(virt, phys & ~0xFFFULL, PTE_WC);
    }
}
