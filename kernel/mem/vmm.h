#pragma once
#include <stdint.h>
#include <stddef.h>

// Page flags
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_PWT       (1ull << 3)  // Page Write-Through
#define PTE_PCD       (1ull << 4)  // Page Cache Disable
#define PTE_NX        (1ull << 63)

// Combined flags for MMIO (uncacheable)
#define PTE_MMIO      (PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT)

void vmm_init();
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_virt_to_phys(uint64_t virt);
uint64_t vmm_phys_to_virt(uint64_t phys);
uint64_t* vmm_create_address_space();
void vmm_switch_address_space(uint64_t* pml4_phys);
uint64_t* vmm_get_kernel_pml4();

// Map MMIO region (allocates virtual address, maps with uncacheable flags)
uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size);

struct DMAAllocation {
    uint64_t virt;
    uint64_t phys;
    uint64_t size;
};

// Allocate contiguous physical memory for DMA
DMAAllocation vmm_alloc_dma(size_t pages);

