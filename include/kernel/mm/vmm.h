#pragma once
#include <stdint.h>
#include <stddef.h>

constexpr uint64_t PTE_PRESENT  = (1ULL << 0);
constexpr uint64_t PTE_WRITABLE = (1ULL << 1);
constexpr uint64_t PTE_USER     = (1ULL << 2);
constexpr uint64_t PTE_PWT      = (1ULL << 3);
constexpr uint64_t PTE_PCD      = (1ULL << 4);
constexpr uint64_t PTE_PAT      = (1ULL << 7);
constexpr uint64_t PTE_NX       = (1ULL << 63);

constexpr uint64_t PTE_MMIO     = (PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT);
constexpr uint64_t PTE_WC       = (PTE_PRESENT | PTE_WRITABLE | PTE_PCD);

void vmm_init();
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_page_in(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);

[[nodiscard]] uint64_t vmm_virt_to_phys(uint64_t virt);
[[nodiscard]] uint64_t vmm_phys_to_virt(uint64_t phys);

[[nodiscard]] uint64_t* vmm_create_address_space();
void vmm_switch_address_space(uint64_t* pml4_phys);

[[nodiscard]] uint64_t* vmm_get_kernel_pml4();

constexpr uint64_t KERNEL_STACK_TOP  = 0xFFFFFF8000000000ULL;
constexpr size_t   KERNEL_STACK_SIZE = 16384;

[[nodiscard]] uint64_t* vmm_clone_address_space(uint64_t* src_pml4);
void vmm_free_address_space(uint64_t* pml4);

[[nodiscard]] uint64_t vmm_get_hhdm_offset();

[[nodiscard]] uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size);

void vmm_remap_framebuffer(uint64_t virt_addr, uint64_t size);

struct DMAAllocation {
    uint64_t virt;
    uint64_t phys;
    uint64_t size;
};

[[nodiscard]] DMAAllocation vmm_alloc_dma(size_t pages);
void vmm_free_dma(DMAAllocation alloc);

bool vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code);
