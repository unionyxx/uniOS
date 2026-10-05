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

/** @brief Overwrites an existing mapping in the current address space. */
Result<void> vmm_replace_page(uint64_t virt, uint64_t phys, uint64_t flags);

/** @brief Maps a physical address to a virtual address in a specific PML4. */
Result<void> vmm_map_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

/** @brief Unmaps a virtual address from a specific PML4. */
void vmm_unmap_page_in(uint64_t *pml4, uint64_t virt);

/** @brief Unmaps without any TLB invalidation; the caller batches one
 *  vmm_invalidate_tlb_range() after the whole range is cleared (and
 *  BEFORE freeing the underlying frames). */
void vmm_unmap_page_no_flush(uint64_t *pml4, uint64_t virt);

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

// --- Kernel-stack guard region -------------------------------------------
//
// Upper-half layout of the kernel PML4 (each entry covers 512 GiB):
//   [0xFFFF800000000000, ...)                         HHDM (BootInfo, grows with RAM)
//   [VMM_KSTACK_REGION_START, VMM_KSTACK_REGION_END)  kernel-stack region (slot 507)
//   [0xFFFFFE0000000000, 0xFFFFFF8000000000)          MMIO/DMA window (slots 508-510)
// The stack region occupies PML4 slot 507, the last slot below the MMIO
// window, so it can collide neither with the HHDM (however much RAM is
// mapped) nor with the window. Its PML4 entry (and the PDPT frame under it)
// is pre-created in vmm_init() for the same reason as the MMIO window's
// entries: address spaces snapshot g_pml4's kernel half at creation time,
// so a kernel-half PML4 entry installed later would be invisible to every
// existing process. PD/PT tables under it are created on demand as stacks
// are mapped.
//
// Each kernel stack occupies one 17-page slot:
//   slot + 0x00000: guard page — leaf PTE installed non-present
//                   (PTE_KSTACK_GUARD marker), any access faults
//   slot + 0x01000: KERNEL_STACK_SIZE/4096 present RW pages (the stack)
// Process::stack_base keeps pointing at the lowest PRESENT page, so
// rsp0 = stack_base + KERNEL_STACK_SIZE and all bootstrap/canary math are
// unchanged from the old HHDM-mapped stacks — only the guard page below is
// new. A kernel-mode non-present #PF anywhere inside the region panics as
// "kernel stack overflow" (vmm_handle_page_fault).
constexpr uint64_t VMM_KSTACK_REGION_START = 0xFFFFFD8000000000ULL;
constexpr uint64_t VMM_KSTACK_REGION_END = 0xFFFFFE0000000000ULL;   // == MMIO window start
constexpr uint64_t VMM_KSTACK_PAGES = KERNEL_STACK_SIZE / 4096 + 1; // stack pages + guard
constexpr uint64_t VMM_KSTACK_SLOT_SIZE = VMM_KSTACK_PAGES * 4096;  // 17 pages = 68 KiB
// 1 GiB / 68 KiB = 15420 concurrent stacks: comfortably above the 4096 the
// scheduler design calls for across CONFIG_SMP_MAX_CPUS cores.
constexpr uint64_t VMM_KSTACK_REGION_CAPACITY =
    (VMM_KSTACK_REGION_END - VMM_KSTACK_REGION_START) / VMM_KSTACK_SLOT_SIZE;

// Non-present leaf PTE marking a slot's guard page: P=0 (every access
// faults) with NX as a software marker, so a deliberately guarded page is
// distinguishable from a never-mapped one by reading the raw leaf PTE
// (bits other than P are software-defined in a non-present entry).
constexpr uint64_t PTE_KSTACK_GUARD = PTE_NX;

/** @brief O(1) predicate: does addr fall inside the kernel-stack guard region? */
[[nodiscard]] bool vmm_is_kernel_stack_region(uint64_t addr);

/** @brief Maps the KERNEL_STACK_SIZE/4096 physically contiguous frames from
 *  stack_phys into a fresh slot of the kernel-stack region: guard page
 *  non-present below, the frames RW above it, one batched cross-CPU flush.
 *  Returns the mapped stack base (lowest present page) or 0 if the region
 *  or the page tables are exhausted; the frames stay caller-owned either
 *  way. */
[[nodiscard]] uint64_t vmm_map_kernel_stack(uint64_t stack_phys);

/** @brief Clears the 17 slot PTEs (guard included), completes the cross-CPU
 *  invalidation, and recycles the slot. The caller may free the backing
 *  frames to the PMM as soon as this returns (flush-before-free invariant,
 *  same contract as vmm_free_dma). */
void vmm_unmap_kernel_stack(uint64_t stack_base);

/** @brief Raw leaf PTE for virt in the given PML4: 0 when the walk does not
 *  reach a leaf, otherwise the leaf entry with every bit intact (a
 *  non-present leaf returns its software bits, e.g. PTE_KSTACK_GUARD). */
[[nodiscard]] uint64_t vmm_get_pte_in(const uint64_t *pml4, uint64_t virt);

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

// DMA allocation policy flag (not a PTE flag; vmm_alloc_dma_with_flags strips
// it before mapping): restrict the physical frames to ZONE_DMA32 (below
// 4 GiB) for 32-bit-only DMA devices. Allocation fails (all-zero
// DMAAllocation) rather than handing out high memory.
constexpr uint64_t VMM_DMA_32BIT = (1ULL << 62);

[[nodiscard]] DMAAllocation vmm_alloc_dma(size_t pages);
[[nodiscard]] DMAAllocation vmm_alloc_dma_with_flags(size_t pages, uint64_t flags);
void vmm_free_dma(const DMAAllocation &alloc);

bool vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code);
void vmm_invalidate_tlb(uint64_t virt);
void vmm_invalidate_tlb_range(uint64_t virt_start, size_t pages);

// Overwrites an already-present mapping (COW, mprotect, cache-type remap).
// vmm_map_page_in() refuses present slots; this variant is the escape hatch.
Result<void> vmm_replace_page_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

// AP bring-up: adopt the current shootdown sequence so the fresh core is not
// expected to ack invalidations that predate its existence.
void vmm_tlb_mark_this_cpu_synced();
