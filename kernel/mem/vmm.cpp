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
    if (hhdm_request.response == NULL) return;
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
