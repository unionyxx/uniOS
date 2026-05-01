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
