#include <kernel/debug.h>
#include <kernel/ktest.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>

KTEST(vmm_map_unmap)
{
    uint64_t virt = 0x500000000ULL; // Test address in user space
    void *frame = pmm_alloc_frame();
    KTEST_EXPECT(frame != nullptr);
    uint64_t phys = reinterpret_cast<uint64_t>(frame);

    // Map the page
    vmm_map_page(virt, phys, PTE_PRESENT | PTE_USER | PTE_WRITABLE);

    // Verify physical address matches
    uint64_t phys_mapped = vmm_virt_to_phys(virt);
    KTEST_EXPECT_EQ(phys_mapped, phys);

    // Unmap the page
    // Note: vmm_unmap_page_in is the current implementation for unmapping
    uint64_t *kernel_pml4 = vmm_get_kernel_pml4();
    vmm_unmap_page_in(kernel_pml4, virt);

    // Verify it's unmapped
    phys_mapped = vmm_virt_to_phys(virt);
    KTEST_EXPECT_EQ(phys_mapped, 0);

    pmm_free_frame(frame);
}

KTEST(vmm_p2v_v2p)
{
    void *frame = pmm_alloc_frame();
    KTEST_EXPECT(frame != nullptr);
    uint64_t phys = reinterpret_cast<uint64_t>(frame);

    uint64_t virt = vmm_phys_to_virt(phys);
    KTEST_EXPECT(virt != 0);
    KTEST_EXPECT(virt >= vmm_get_hhdm_offset());

    // Test the internal HHDM mapping (kernel space)
    uint64_t phys_back = vmm_virt_to_phys(virt);
    KTEST_EXPECT_EQ(phys_back, phys);

    pmm_free_frame(frame);
}

KTEST(vmm_kstack_region_bounds)
{
    // The region is [START, END): 1 GiB directly below the MMIO window.
    KTEST_EXPECT(!vmm_is_kernel_stack_region(VMM_KSTACK_REGION_START - 1));
    KTEST_EXPECT(vmm_is_kernel_stack_region(VMM_KSTACK_REGION_START));
    KTEST_EXPECT(vmm_is_kernel_stack_region(VMM_KSTACK_REGION_END - 1));
    KTEST_EXPECT(!vmm_is_kernel_stack_region(VMM_KSTACK_REGION_END));
    // Neighbouring windows are outside: user-space top, the HHDM start, and
    // the MMIO/DMA window itself (== END).
    KTEST_EXPECT(!vmm_is_kernel_stack_region(0x0000800000000000ULL - 1));
    KTEST_EXPECT(!vmm_is_kernel_stack_region(0xFFFF800000000000ULL));
    KTEST_EXPECT(!vmm_is_kernel_stack_region(0xFFFF800000000000ULL + 0x1000));
}

KTEST(vmm_kstack_pml4_precreated)
{
    // Address spaces snapshot g_pml4's kernel half at creation, so the
    // region's PML4 entry must already exist in a freshly created one (the
    // same constraint the MMIO window pre-creation covers).
    uint64_t *pml4 = vmm_create_address_space();
    KTEST_EXPECT(pml4 != nullptr);
    if (!pml4)
        return;
    const uint64_t entry = pml4[(VMM_KSTACK_REGION_START >> 39) & 0x1FF];
    KTEST_EXPECT_EQ(entry & PTE_PRESENT, PTE_PRESENT);
    vmm_free_address_space(pml4);
}

KTEST(vmm_kstack_guard_mapping)
{
    const uint64_t stack_pages = KERNEL_STACK_SIZE / 4096;
    void *frames = pmm_alloc_frames(stack_pages);
    KTEST_EXPECT(frames != nullptr);
    const uint64_t phys = reinterpret_cast<uint64_t>(frames);

    const uint64_t base = vmm_map_kernel_stack(phys);
    KTEST_EXPECT(base != 0);
    KTEST_EXPECT(vmm_is_kernel_stack_region(base));

    const uint64_t guard = base - 0x1000;
    const uint64_t *pml4 = vmm_get_kernel_pml4();

    // Guard page: no translation, but an explicitly marked non-present leaf
    // PTE (PTE_KSTACK_GUARD) rather than a never-mapped slot. Asserted by
    // table walk instead of faulting on it.
    KTEST_EXPECT_EQ(vmm_virt_to_phys_in(pml4, guard), 0);
    KTEST_EXPECT_EQ(vmm_get_pte_in(pml4, guard), PTE_KSTACK_GUARD);

    // The 16 stack pages: present, writable, supervisor, backing exactly the
    // frames the caller allocated.
    for (uint64_t i = 0; i < stack_pages; i++) {
        const uint64_t va = base + i * 0x1000;
        KTEST_EXPECT_EQ(vmm_virt_to_phys_in(pml4, va), phys + i * 0x1000);
        const uint64_t pte = vmm_get_pte_in(pml4, va);
        KTEST_EXPECT_EQ(pte & PTE_PRESENT, PTE_PRESENT);
        KTEST_EXPECT_EQ(pte & PTE_WRITABLE, PTE_WRITABLE);
        KTEST_EXPECT_EQ(pte & PTE_USER, 0);
    }

    // The mapped base is real writable memory through the region VA.
    *reinterpret_cast<uint64_t *>(base) = 0x0BADC0DE0BADC0DEULL;
    KTEST_EXPECT_EQ(*reinterpret_cast<uint64_t *>(base), 0x0BADC0DE0BADC0DEULL);

    // Unmap clears the whole 17-page slot, guard marker included.
    vmm_unmap_kernel_stack(base);
    for (uint64_t i = 0; i < VMM_KSTACK_PAGES; i++) {
        const uint64_t va = guard + i * 0x1000;
        KTEST_EXPECT_EQ(vmm_virt_to_phys_in(pml4, va), 0);
        KTEST_EXPECT_EQ(vmm_get_pte_in(pml4, va), 0);
    }

    // Slot recycling: the freed slot is handed out again (LIFO), guard
    // reinstalled.
    const uint64_t recycled = vmm_map_kernel_stack(phys);
    KTEST_EXPECT(recycled != 0);
    KTEST_EXPECT_EQ(recycled, base);
    KTEST_EXPECT_EQ(vmm_get_pte_in(pml4, guard), PTE_KSTACK_GUARD);

    vmm_unmap_kernel_stack(recycled);
    for (uint64_t i = 0; i < stack_pages; i++)
        pmm_free_frame(reinterpret_cast<void *>(phys + i * 0x1000));
}
