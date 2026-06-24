#pragma once
#include <libk/result.h>
#include <stddef.h>
#include <stdint.h>

constexpr uint64_t PTE_PRESENT = (1ULL << 0);
constexpr uint64_t PTE_WRITABLE = (1ULL << 1);
constexpr uint64_t PTE_USER = (1ULL << 2);
constexpr uint64_t PTE_PWT = (1ULL << 3);
constexpr uint64_t PTE_PCD = (1ULL << 4);
constexpr uint64_t PTE_PAT = (1ULL << 7);
constexpr uint64_t PTE_NX = (1ULL << 63);
constexpr uint64_t PTE_SHARED = (1ULL << 52); // Software bit: Shared Memory (no CoW)

constexpr uint64_t PTE_MMIO = (PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX);
constexpr uint64_t PTE_UC = PTE_MMIO; // PCD|PWT maps to Strong Uncacheable with default PAT
constexpr uint64_t PTE_WC = (PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_NX);

/** @brief Pre-initialization of the Virtual Memory Manager. */
void vmm_early_init();

/** @brief Initializes the Virtual Memory Manager and sets up kernel paging. */
void vmm_init();

/** @brief Sets up page protections for kernel sections (read-only, no-execute). */
void vmm_protect_kernel();

/** @brief Maps a physical address to a virtual address in the current address space. */
Result<void> vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/** @brief Maps a physical address to a virtual address in a specific PML4. */
Result<void> vmm_map_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/** @brief Unmaps a virtual address from a specific PML4. */
void vmm_unmap_page_in(uint64_t *pml4, uint64_t virt);

/** @brief Translates a virtual address to its physical address in the current address space. */
[[nodiscard]] uint64_t vmm_virt_to_phys(uint64_t virt);

/** @brief Translates a virtual address to its physical address in a specific PML4. */
[[nodiscard]] uint64_t vmm_virt_to_phys_in(const uint64_t *pml4, uint64_t virt);
[[nodiscard]] uint64_t vmm_get_page_flags_in(const uint64_t *pml4, uint64_t virt);

/** @brief Maps a physical address to its HHDM (Higher Half Direct Map) virtual address. */
[[nodiscard]] uint64_t vmm_phys_to_virt(uint64_t phys);

[[nodiscard]] uint64_t *vmm_create_address_space();
void vmm_switch_address_space(const uint64_t *pml4_phys);

[[nodiscard]] uint64_t *vmm_get_kernel_pml4();

constexpr uint64_t KERNEL_STACK_TOP = 0xFFFFFF8000000000ULL;
constexpr size_t KERNEL_STACK_SIZE = 65536;

[[nodiscard]] uint64_t *vmm_clone_address_space(const uint64_t *src_pml4);
void vmm_free_address_space(const uint64_t *pml4);

[[nodiscard]] uint64_t vmm_get_hhdm_offset();

[[nodiscard]] uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size);

void vmm_remap_framebuffer(uint64_t virt_addr, uint64_t size);

struct DMAAllocation
{
    uint64_t virt;
    uint64_t phys;
    uint64_t size;
};

[[nodiscard]] DMAAllocation vmm_alloc_dma(size_t pages);
[[nodiscard]] DMAAllocation vmm_alloc_dma_with_flags(size_t pages, uint64_t flags);
void vmm_free_dma(const DMAAllocation &alloc);

bool vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code);
void vmm_invalidate_tlb(uint64_t virt);
void vmm_invalidate_tlb_range(uint64_t virt_start, size_t pages);
