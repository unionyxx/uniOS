#pragma once
#include <stdint.h>
#include <stddef.h>

// Page flags
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_PWT       (1ull << 3)  // Page Write-Through
#define PTE_PCD       (1ull << 4)  // Page Cache Disable
#define PTE_PAT       (1ull << 7)  // PAT bit (for 4KB pages)
#define PTE_NX        (1ull << 63)

// Combined flags for MMIO (uncacheable)
#define PTE_MMIO      (PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT)

// Write-Combining: PCD=1, PWT=0, PAT=0 -> PAT index 2 (configured to WC in pat_init)
#define PTE_WC        (PTE_PRESENT | PTE_WRITABLE | PTE_PCD)

void vmm_init();
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_virt_to_phys(uint64_t virt);
uint64_t vmm_phys_to_virt(uint64_t phys);
uint64_t* vmm_create_address_space();
void vmm_switch_address_space(uint64_t* pml4_phys);
uint64_t* vmm_get_kernel_pml4();

// Process isolation support
// Fixed kernel stack virtual address - same in every process
#define KERNEL_STACK_TOP  0xFFFFFF8000000000ULL
#define KERNEL_STACK_SIZE 16384  // 16KB per process

// Clone an address space (deep copy user pages, share kernel pages)
uint64_t* vmm_clone_address_space(uint64_t* src_pml4);

// Free all user-space pages in an address space
void vmm_free_address_space(uint64_t* pml4);

// Get HHDM offset for physical->virtual conversion
uint64_t vmm_get_hhdm_offset();

// Map MMIO region (allocates virtual address, maps with uncacheable flags)
uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size);

// Remap framebuffer with Write-Combining for improved graphics performance
void vmm_remap_framebuffer(uint64_t virt_addr, uint64_t size);

struct DMAAllocation {
    uint64_t virt;
    uint64_t phys;
    uint64_t size;
};

// Allocate contiguous physical memory for DMA
DMAAllocation vmm_alloc_dma(size_t pages);

