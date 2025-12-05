#pragma once
#include <stdint.h>

// Page flags
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_NX        (1ull << 63)

void vmm_init();
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_virt_to_phys(uint64_t virt);
uint64_t vmm_phys_to_virt(uint64_t phys);
uint64_t* vmm_create_address_space();
void vmm_switch_address_space(uint64_t* pml4_phys);
uint64_t* vmm_get_kernel_pml4();
