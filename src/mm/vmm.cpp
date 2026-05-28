#include <boot/boot_info.h>
#include <kernel/arch/x86_64/serial.h>
#include <kernel/debug.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/mm/vmm.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <libk/kstring.h>
#include <stddef.h>

STATIC_ASSERT(sizeof(FileDescriptor) == 32, "FileDescriptor size mismatch");
STATIC_ASSERT(offsetof(Process, fpu_state) == 64, "Process::fpu_state offset mismatch");
STATIC_ASSERT(offsetof(Process, pid) == 4160, "Process::pid offset mismatch");
STATIC_ASSERT(offsetof(Process, sp) == 4216, "Process::sp offset mismatch");
STATIC_ASSERT(offsetof(Process, page_table) == 4240, "Process::page_table offset mismatch");
STATIC_ASSERT(offsetof(Process, vma_list) == 8456, "Process::vma_list offset mismatch");

using kstring::memcpy;

#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/arch/x86_64/idt.h>

static uint64_t *g_pml4 = nullptr;
static uint64_t g_hhdm_offset = 0;

struct TlbShootdown
{
    Spinlock lock = SPINLOCK_INIT;
    volatile uint64_t target_addr;
    volatile int active_cores;
};

static TlbShootdown g_tlb_shootdown;
static uint8_t g_tlb_shootdown_vector = 0;

static void tlb_shootdown_handler(uint8_t vector, void *ctx)
{
    (void)vector;
    (void)ctx;
    uint64_t addr = g_tlb_shootdown.target_addr;
    if (addr) {
        asm volatile("invlpg (%0)" ::"r"(addr) : "memory");
    }
    __sync_sub_and_fetch(&g_tlb_shootdown.active_cores, 1);
}

void vmm_invalidate_tlb(uint64_t virt)
{
    // 1. Invalidate locally on current core
    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");

    // 2. If APIC is enabled and there are other active CPUs, broadcast TLB shootdown IPI
    if (apic_is_enabled()) {
        int others = g_cpu_online_count - 1;
        if (others > 0) {
            uint64_t sl_flags = spinlock_acquire_irqsave(&g_tlb_shootdown.lock);

            g_tlb_shootdown.target_addr = virt;
            g_tlb_shootdown.active_cores = others;

            // Broadcast shootdown IPI using the registered vector
            if (g_tlb_shootdown_vector != 0) {
                apic_send_ipi_all_excluding_self(g_tlb_shootdown_vector);
            }

            // Spin-wait until all other cores have processed the invalidation
            while (g_tlb_shootdown.active_cores > 0) {
                asm volatile("pause");
            }

            g_tlb_shootdown.target_addr = 0;
            spinlock_release_irqrestore(&g_tlb_shootdown.lock, sl_flags);
        }
    }
}

[[nodiscard]] static bool split_huge_page(uint64_t *table, uint64_t index, int level)
{
    uint64_t entry = table[index];
    if (!(entry & PTE_PRESENT) || !(entry & (1ULL << 7)))
        return false;

    void *next_table_frame = pmm_alloc_frame();
    if (!next_table_frame)
        return false;

    uint64_t next_table_phys = reinterpret_cast<uint64_t>(next_table_frame);
    uint64_t *next_table_virt = reinterpret_cast<uint64_t *>(next_table_phys + g_hhdm_offset);
    kstring::zero_memory(next_table_virt, 512 * sizeof(uint64_t));

    uint64_t flags = (entry & 0xFFF) | (entry & (1ULL << 63));
    if (entry & (1ULL << 12)) {
        flags |= (1ULL << 7); // Move PAT bit from 12 (Huge) to 7 (4KB)
    } else {
        flags &= ~(1ULL << 7); // Strip Huge Page flag safely
    }

    if (level == 3) {
        uint64_t base_phys = entry & 0x000FFFFFC0000000ULL;
        for (int i = 0; i < 512; i++) {
            next_table_virt[i] = (base_phys + (static_cast<uint64_t>(i) * 0x200000ULL)) | flags | (1ULL << 7);
        }
    } else if (level == 2) {
        uint64_t base_phys = entry & 0x000FFFFFFFE00000ULL;
        for (int i = 0; i < 512; i++) {
            next_table_virt[i] = (base_phys + (static_cast<uint64_t>(i) * 0x1000ULL)) | flags;
        }
    } else {
        pmm_free_frame(next_table_frame);
        return false;
    }

    table[index] = next_table_phys | (flags & ~0x80ULL);
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    return true;
}

[[nodiscard]] static uint64_t *get_next_level(uint64_t *current_level, uint64_t index, int level, bool alloc,
                                              uint64_t virt)
{
    if (current_level[index] & PTE_PRESENT) {
        if (current_level[index] & (1ULL << 7)) {
            if (!split_huge_page(current_level, index, level))
                return nullptr;
        }
        uint64_t phys = current_level[index] & 0x000FFFFFFFFFF000ULL;
        return reinterpret_cast<uint64_t *>(phys + g_hhdm_offset);
    }

    if (!alloc)
        return nullptr;

    void *frame = pmm_alloc_frame();
    if (!frame)
        return nullptr;

    uint64_t phys = reinterpret_cast<uint64_t>(frame);
    uint64_t flags = PTE_PRESENT | PTE_WRITABLE;
    if (virt < 0x0000800000000000ULL)
        flags |= PTE_USER;

    uint64_t *next_level = reinterpret_cast<uint64_t *>(phys + g_hhdm_offset);
    kstring::zero_memory(next_level, 512 * sizeof(uint64_t));
    asm volatile("sfence" ::: "memory");

    uint64_t expected = 0;
    uint64_t desired = phys | flags;
    if (!__atomic_compare_exchange_n(&current_level[index], &expected, desired, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST)) {
        pmm_free_frame(frame); // Another thread beat us to it
        return reinterpret_cast<uint64_t *>((expected & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    }
    return next_level;
}

void vmm_early_init()
{
    const BootInfo *boot_info = boot_get_info();
    if (boot_info) {
        g_hhdm_offset = boot_info->hhdm_offset;
    }
}

void vmm_init()
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    g_pml4 = reinterpret_cast<uint64_t *>(cr3 + g_hhdm_offset);

    // Initialize TLB shootdown interrupt handler
    g_tlb_shootdown_vector = idt_allocate_free_vector();
    if (g_tlb_shootdown_vector != 0) {
        irq_register_vector_handler(g_tlb_shootdown_vector, tlb_shootdown_handler, nullptr);
    }
}

uint64_t vmm_phys_to_virt(uint64_t phys)
{
    return phys + g_hhdm_offset;
}

Result<void> vmm_map_page_in(uint64_t *target_pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt >= KERNEL_STACK_TOP && (flags & PTE_USER))
        panic("vmm: kernel address with PTE_USER");
    if (virt < 0x0000800000000000ULL && !(flags & PTE_USER))
        panic("vmm: userspace address without PTE_USER");

    uint64_t *pdpt = get_next_level(target_pml4, (virt >> 39) & 0x1FF, 4, true, virt);
    if (!pdpt)
        return Error::NoMemory;

    uint64_t *pd = get_next_level(pdpt, (virt >> 30) & 0x1FF, 3, true, virt);
    if (!pd)
        return Error::NoMemory;

    uint64_t *pt = get_next_level(pd, (virt >> 21) & 0x1FF, 2, true, virt);
    if (!pt)
        return Error::NoMemory;

    pt[(virt >> 12) & 0x1FF] = phys | flags;
    asm volatile("sfence" ::: "memory");
    return Result<void>::success();
}

Result<void> vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4 = g_pml4;
    Process *curr = process_get_current();
    if (curr && curr->page_table)
        pml4 = curr->page_table;

    Result<void> res = vmm_map_page_in(pml4, virt, phys, flags);
    if (res.ok()) {
        vmm_invalidate_tlb(virt);
    }
    return res;
}

[[nodiscard]] static bool vmm_map_page_no_flush_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt >= KERNEL_STACK_TOP && (flags & PTE_USER))
        panic("vmm: kernel address with PTE_USER");
    if (virt < 0x0000800000000000ULL && !(flags & PTE_USER))
        panic("vmm: userspace address without PTE_USER");

    uint64_t *pdpt = get_next_level(pml4, (virt >> 39) & 0x1FF, 4, true, virt);
    if (!pdpt)
        return false;

    uint64_t *pd = get_next_level(pdpt, (virt >> 30) & 0x1FF, 3, true, virt);
    if (!pd)
        return false;

    uint64_t *pt = get_next_level(pd, (virt >> 21) & 0x1FF, 2, true, virt);
    if (!pt)
        return false;

    pt[(virt >> 12) & 0x1FF] = phys | flags;
    return true;
}

static inline void vmm_flush_tlb_all()
{
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

uint64_t vmm_virt_to_phys_in(const uint64_t *pml4, uint64_t virt)
{
    if (!pml4)
        return 0;

    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_index] & PTE_PRESENT))
        return 0;

    const uint64_t *pdpt =
        reinterpret_cast<const uint64_t *>((pml4[pml4_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_index] & PTE_PRESENT))
        return 0;
    if (pdpt[pdpt_index] & (1ULL << 7)) {
        return (pdpt[pdpt_index] & 0x000FFFFFC0000000ULL) + (virt & 0x3FFFFFFFULL);
    }

    const uint64_t *pd = reinterpret_cast<const uint64_t *>((pdpt[pdpt_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    uint64_t pd_index = (virt >> 21) & 0x1FF;
    if (!(pd[pd_index] & PTE_PRESENT))
        return 0;
    if (pd[pd_index] & (1ULL << 7)) {
        return (pd[pd_index] & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFFULL);
    }

    const uint64_t *pt = reinterpret_cast<const uint64_t *>((pd[pd_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    uint64_t pt_index = (virt >> 12) & 0x1FF;
    if (!(pt[pt_index] & PTE_PRESENT))
        return 0;

    return (pt[pt_index] & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFFULL);
}

uint64_t vmm_virt_to_phys(uint64_t virt)
{
    const uint64_t *pml4 = g_pml4;
    const Process *curr = process_get_current();
    if (curr && curr->page_table)
        pml4 = curr->page_table;
    return vmm_virt_to_phys_in(pml4, virt);
}

uint64_t vmm_get_page_flags_in(const uint64_t *pml4, uint64_t virt)
{
    if (!pml4)
        return 0;

    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    if (!(pml4[pml4_index] & PTE_PRESENT))
        return 0;

    const uint64_t *pdpt =
        reinterpret_cast<const uint64_t *>((pml4[pml4_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    if (!(pdpt[pdpt_index] & PTE_PRESENT) || (pdpt[pdpt_index] & (1ULL << 7)))
        return 0;

    const uint64_t *pd = reinterpret_cast<const uint64_t *>((pdpt[pdpt_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    uint64_t pd_index = (virt >> 21) & 0x1FF;
    if (!(pd[pd_index] & PTE_PRESENT) || (pd[pd_index] & (1ULL << 7)))
        return 0;

    const uint64_t *pt = reinterpret_cast<const uint64_t *>((pd[pd_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    uint64_t pt_index = (virt >> 12) & 0x1FF;
    if (!(pt[pt_index] & PTE_PRESENT))
        return 0;

    return pt[pt_index] & (0xFFFULL | PTE_NX | PTE_SHARED);
}

uint64_t *vmm_get_kernel_pml4()
{
    return g_pml4;
}

void vmm_unmap_page_in(uint64_t *target_pml4, uint64_t virt)
{
    uint64_t *pdpt = get_next_level(target_pml4, (virt >> 39) & 0x1FF, 4, false, virt);
    if (!pdpt)
        return;

    uint64_t *pd = get_next_level(pdpt, (virt >> 30) & 0x1FF, 3, false, virt);
    if (!pd)
        return;

    uint64_t *pt = get_next_level(pd, (virt >> 21) & 0x1FF, 2, false, virt);
    if (!pt)
        return;

    pt[(virt >> 12) & 0x1FF] = 0;
    vmm_invalidate_tlb(virt);
}

uint64_t *vmm_create_address_space()
{
    void *frame = pmm_alloc_frame();
    if (!frame)
        return nullptr;

    uint64_t *new_pml4 = reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(frame) + g_hhdm_offset);
    kstring::zero_memory(new_pml4, 512 * sizeof(uint64_t));

    for (int i = 256; i < 512; i++) {
        new_pml4[i] = g_pml4[i];
    }

    return new_pml4;
}

uint64_t vmm_get_hhdm_offset()
{
    return g_hhdm_offset;
}

static void free_page_table_level(uint64_t *table, int level);

[[nodiscard]] static bool clone_page_table_level(uint64_t *src, uint64_t *dst, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PTE_PRESENT)) {
            dst[i] = 0;
            continue;
        }

        uint64_t src_phys = src[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t flags = src[i] & (0xFFFULL | PTE_NX | PTE_SHARED);

        if (level == 1) {
            if (flags & PTE_SHARED) {
                pmm_refcount_inc(reinterpret_cast<void *>(src_phys));
                dst[i] = src_phys | flags;
            } else {
                pmm_refcount_inc(reinterpret_cast<void *>(src_phys));
                if (flags & PTE_WRITABLE) {
                    flags &= ~PTE_WRITABLE;
                    src[i] &= ~PTE_WRITABLE;
                }
                dst[i] = src_phys | flags;
            }
        } else {
            if (src[i] & (1ULL << 7)) {
                panic("vmm_clone: huge pages in userspace not supported yet!");
            }

            void *next_level_frame = pmm_alloc_frame();
            if (!next_level_frame) {
                DEBUG_ERROR("clone_page_table_level: pmm_alloc_frame failed at level %d", level);
                free_page_table_level(dst, level);
                return false;
            }

            uint64_t *next_level_virt =
                reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(next_level_frame) + g_hhdm_offset);
            uint64_t *src_table = reinterpret_cast<uint64_t *>(src_phys + g_hhdm_offset);
            kstring::zero_memory(next_level_virt, 512 * sizeof(uint64_t));

            if (!clone_page_table_level(src_table, next_level_virt, level - 1)) {
                pmm_free_frame(next_level_frame);
                free_page_table_level(dst, level);
                return false;
            }
            dst[i] = reinterpret_cast<uint64_t>(next_level_frame) | flags;
        }
    }
    return true;
}

uint64_t *vmm_clone_address_space(const uint64_t *src_pml4)
{
    if (!src_pml4)
        return nullptr;

    void *frame = pmm_alloc_frame();
    if (!frame)
        return nullptr;

    uint64_t *new_pml4 = reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(frame) + g_hhdm_offset);
    kstring::zero_memory(new_pml4, 512 * sizeof(uint64_t));

    for (int i = 256; i < 512; i++) {
        new_pml4[i] = src_pml4[i];
    }

    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & PTE_PRESENT)) {
            new_pml4[i] = 0;
            continue;
        }

        uint64_t src_phys = src_pml4[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t flags = src_pml4[i] & (0xFFFULL | PTE_NX);

        void *new_pdpt = pmm_alloc_frame();
        if (!new_pdpt) {
            DEBUG_ERROR("vmm_clone_address_space: failed to allocate PDPT for index %d", i);
            vmm_free_address_space(new_pml4);
            return nullptr;
        }

        uint64_t *new_pdpt_virt = reinterpret_cast<uint64_t *>(reinterpret_cast<uint64_t>(new_pdpt) + g_hhdm_offset);
        uint64_t *src_pdpt = reinterpret_cast<uint64_t *>(src_phys + g_hhdm_offset);
        kstring::zero_memory(new_pdpt_virt, 512 * sizeof(uint64_t));

        if (!clone_page_table_level(src_pdpt, new_pdpt_virt, 3)) {
            pmm_free_frame(new_pdpt);
            vmm_free_address_space(new_pml4);
            return nullptr;
        }
        new_pml4[i] = reinterpret_cast<uint64_t>(new_pdpt) | flags;
    }

    asm volatile("sfence" ::: "memory");

    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & ~0xFFFULL) == (reinterpret_cast<uint64_t>(src_pml4) - g_hhdm_offset)) {
        asm volatile("mov %0, %%cr3" ::"r"(current_cr3) : "memory");
    }

    return new_pml4;
}

static void free_page_table_level(uint64_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT))
            continue;

        uint64_t phys = table[i] & 0x000FFFFFFFFFF000ULL;
        if (level == 1) {
            if (pmm_is_managed(reinterpret_cast<void *>(phys))) {
                pmm_refcount_dec(reinterpret_cast<void *>(phys));
            }
        } else {
            uint64_t *sub_table = reinterpret_cast<uint64_t *>(phys + g_hhdm_offset);
            free_page_table_level(sub_table, level - 1);
            pmm_free_frame(reinterpret_cast<void *>(phys));
        }
    }
}

void vmm_free_address_space(const uint64_t *target_pml4)
{
    if (!target_pml4 || target_pml4 == g_pml4)
        return;

    for (int i = 0; i < 256; i++) {
        if (!(target_pml4[i] & PTE_PRESENT))
            continue;

        uint64_t phys = target_pml4[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t *pdpt = reinterpret_cast<uint64_t *>(phys + g_hhdm_offset);
        free_page_table_level(pdpt, 3);
        pmm_free_frame(reinterpret_cast<void *>(phys));
    }

    pmm_free_frame(reinterpret_cast<void *>(reinterpret_cast<uint64_t>(target_pml4) - g_hhdm_offset));
}

void vmm_switch_address_space(const uint64_t *new_pml4_phys)
{
    asm volatile("mov %0, %%cr3" ::"r"(new_pml4_phys) : "memory");
}

void vmm_set_page_flags(uint64_t virt, uint64_t flags)
{
    uint64_t *pml4 = g_pml4;
    Process *curr = process_get_current();
    if (curr && curr->page_table)
        pml4 = curr->page_table;

    uint64_t *pdpt = get_next_level(pml4, (virt >> 39) & 0x1FF, 4, false, virt);
    if (!pdpt)
        return;

    uint64_t *pd = get_next_level(pdpt, (virt >> 30) & 0x1FF, 3, false, virt);
    if (!pd)
        return;

    uint64_t *pt = get_next_level(pd, (virt >> 21) & 0x1FF, 2, false, virt);
    if (!pt)
        return;

    uint64_t index = (virt >> 12) & 0x1FF;
    if (!(pt[index] & PTE_PRESENT))
        return;

    uint64_t old_entry = pt[index];
    uint64_t phys = old_entry & 0x000FFFFFFFFFF000ULL;
    uint64_t preserved = old_entry & (PTE_PRESENT | PTE_USER | PTE_SHARED);

    pt[index] = phys | flags | preserved;
    vmm_invalidate_tlb(virt);
}

void vmm_protect_kernel()
{
    const BootInfo *boot_info = boot_get_info();
    if (!boot_info)
        return;

    extern char __text_start[], __text_end[];
    extern char __rodata_start[], __rodata_end[];
    extern char __requests_start[], __requests_end[];
    extern char __data_start[], __kernel_end[];

    auto protect_range = [](uintptr_t start, uintptr_t end, uint64_t flags) {
        uintptr_t start_aligned = start & ~0xFFFULL;
        uintptr_t end_aligned = (end + 0xFFF) & ~0xFFFULL;
        for (uintptr_t virt = start_aligned; virt < end_aligned; virt += 0x1000) {
            vmm_set_page_flags(virt, flags);
        }
    };

    DEBUG_INFO("Protecting kernel sections...");
    protect_range(reinterpret_cast<uintptr_t>(__text_start), reinterpret_cast<uintptr_t>(__text_end), PTE_PRESENT);
    protect_range(reinterpret_cast<uintptr_t>(__rodata_start), reinterpret_cast<uintptr_t>(__rodata_end),
                  PTE_PRESENT | PTE_NX);
    protect_range(reinterpret_cast<uintptr_t>(__requests_start), reinterpret_cast<uintptr_t>(__requests_end),
                  PTE_PRESENT | PTE_NX);
    protect_range(reinterpret_cast<uintptr_t>(__data_start), reinterpret_cast<uintptr_t>(__kernel_end),
                  PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    DEBUG_SUCCESS("Kernel sections protected");
}

static uint64_t g_mmio_next_virt = 0xFFFFFE0000000000ULL;
static const uint64_t g_mmio_limit_virt = 0xFFFFFF8000000000ULL;
static Spinlock g_dma_lock = SPINLOCK_INIT;
static constexpr uint64_t MASSIVE_MAPPING_THRESHOLD = 16ULL * 1024ULL * 1024ULL * 1024ULL;

struct DMAVirtRange
{
    uint64_t virt;
    uint64_t size;
};

static constexpr size_t MAX_DMA_FREE_RANGES = 64;
static DMAVirtRange g_dma_free_ranges[MAX_DMA_FREE_RANGES] = {};

static uint64_t dma_alloc_virt_range(uint64_t size)
{
    uint64_t sl_flags = spinlock_acquire_irqsave(&g_dma_lock);
    for (size_t i = 0; i < MAX_DMA_FREE_RANGES; i++) {
        if (g_dma_free_ranges[i].size < size)
            continue;

        uint64_t virt = g_dma_free_ranges[i].virt;
        g_dma_free_ranges[i].virt += size;
        g_dma_free_ranges[i].size -= size;
        if (g_dma_free_ranges[i].size == 0)
            g_dma_free_ranges[i] = {};
        spinlock_release_irqrestore(&g_dma_lock, sl_flags);
        return virt;
    }

    if (size == 0 || size > MASSIVE_MAPPING_THRESHOLD) {
        spinlock_release_irqrestore(&g_dma_lock, sl_flags);
        panic("vmm: sanity check failed: massive/invalid dma allocation requested");
    }

    if (g_mmio_next_virt > g_mmio_limit_virt || (g_mmio_limit_virt - g_mmio_next_virt) < size) {
        spinlock_release_irqrestore(&g_dma_lock, sl_flags);
        panic("vmm: mmio/dma virtual address space exhausted");
    }

    uint64_t virt = g_mmio_next_virt;
    g_mmio_next_virt += size;
    spinlock_release_irqrestore(&g_dma_lock, sl_flags);
    return virt;
}

static void dma_free_virt_range(uint64_t virt, uint64_t size)
{
    if (virt == 0 || size == 0)
        return;

    uint64_t sl_flags = spinlock_acquire_irqsave(&g_dma_lock);
    bool inserted = false;

    for (size_t i = 0; i < MAX_DMA_FREE_RANGES; i++) {
        if (g_dma_free_ranges[i].size == 0)
            continue;
        if (g_dma_free_ranges[i].virt + g_dma_free_ranges[i].size == virt) {
            g_dma_free_ranges[i].size += size;
            inserted = true;
            break;
        }
        if (virt + size == g_dma_free_ranges[i].virt) {
            g_dma_free_ranges[i].virt = virt;
            g_dma_free_ranges[i].size += size;
            inserted = true;
            break;
        }
    }

    if (!inserted) {
        for (size_t i = 0; i < MAX_DMA_FREE_RANGES; i++) {
            if (g_dma_free_ranges[i].size == 0) {
                g_dma_free_ranges[i] = {virt, size};
                inserted = true;
                break;
            }
        }
    }

    if (!inserted) {
        DEBUG_ERROR("vmm: DMA free range list exhausted, leaking %llu bytes", size);
    }

    for (size_t i = 0; i < MAX_DMA_FREE_RANGES; i++) {
        if (g_dma_free_ranges[i].size == 0)
            continue;
        for (size_t j = i + 1; j < MAX_DMA_FREE_RANGES; j++) {
            if (g_dma_free_ranges[j].size == 0)
                continue;

            if (g_dma_free_ranges[i].virt + g_dma_free_ranges[i].size == g_dma_free_ranges[j].virt) {
                g_dma_free_ranges[i].size += g_dma_free_ranges[j].size;
                g_dma_free_ranges[j] = {};
                j = i;
                continue;
            }
            if (g_dma_free_ranges[j].virt + g_dma_free_ranges[j].size == g_dma_free_ranges[i].virt) {
                g_dma_free_ranges[i].virt = g_dma_free_ranges[j].virt;
                g_dma_free_ranges[i].size += g_dma_free_ranges[j].size;
                g_dma_free_ranges[j] = {};
                j = i;
            }
        }
    }
    spinlock_release_irqrestore(&g_dma_lock, sl_flags);
}

uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size)
{
    if (size == 0)
        return 0;

    uint64_t phys_page = phys_addr & ~0xFFFULL;
    uint64_t offset = phys_addr & 0xFFF;
    if (size > UINT64_MAX - offset || size + offset > UINT64_MAX - 0xFFFULL)
        return 0;

    uint64_t pages = (size + offset + 0xFFFULL) / 0x1000ULL;
    if (pages == 0 || pages > UINT64_MAX / 0x1000ULL)
        return 0;

    uint64_t map_size = pages * 0x1000ULL;
    if (map_size > MASSIVE_MAPPING_THRESHOLD)
        panic("vmm: sanity check failed: massive mmio mapping requested");

    if (g_mmio_next_virt > g_mmio_limit_virt || (g_mmio_limit_virt - g_mmio_next_virt) < map_size)
        panic("vmm: mmio virtual address space exhausted");

    uint64_t virt_base = dma_alloc_virt_range(map_size);

    if (pages > 1024) {
        BOOT_LOG("VMM: Mapping large MMIO region: %llu pages (0x%lx -> 0x%lx)", pages, phys_page, virt_base);
    }

    for (uint64_t i = 0; i < pages; i++) {
        if (pages > 16384 && (i % 8192 == 0)) {
            BOOT_LOG("VMM: Mapping progress: %llu/%llu pages...", i, pages);
        }
        if (!vmm_map_page_no_flush_in(g_pml4, virt_base + i * 0x1000, phys_page + i * 0x1000, PTE_MMIO)) {
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_page_in(g_pml4, virt_base + j * 0x1000);
            }
            dma_free_virt_range(virt_base, map_size);
            vmm_flush_tlb_all();
            return 0;
        }
    }

    vmm_flush_tlb_all();
    return virt_base + offset;
}

DMAAllocation vmm_alloc_dma_with_flags(size_t pages, uint64_t flags)
{
    DMAAllocation alloc = {0, 0, 0};
    if (pages == 0)
        return alloc;

    void *phys_ptr = pmm_alloc_frames(pages);
    if (!phys_ptr)
        return alloc;

    if (pages > UINT64_MAX / 0x1000ULL) {
        for (size_t frame = 0; frame < pages; frame++) {
            pmm_free_frame(reinterpret_cast<void *>(reinterpret_cast<uint64_t>(phys_ptr) + frame * 0x1000ULL));
        }
        return alloc;
    }

    uint64_t phys = reinterpret_cast<uint64_t>(phys_ptr);
    uint64_t size = static_cast<uint64_t>(pages) * 0x1000ULL;
    uint64_t virt_base = dma_alloc_virt_range(size);

    for (size_t i = 0; i < pages; i++) {
        if (!vmm_map_page_no_flush_in(g_pml4, virt_base + i * 0x1000, phys + i * 0x1000, flags)) {
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page_in(g_pml4, virt_base + j * 0x1000);
            }
            for (size_t frame = 0; frame < pages; frame++) {
                pmm_free_frame(reinterpret_cast<void *>(phys + frame * 0x1000));
            }
            dma_free_virt_range(virt_base, size);
            vmm_flush_tlb_all();
            return alloc;
        }
    }

    vmm_flush_tlb_all();

    alloc.virt = virt_base;
    alloc.phys = phys;
    alloc.size = size;
    return alloc;
}

DMAAllocation vmm_alloc_dma(size_t pages)
{
    return vmm_alloc_dma_with_flags(pages, PTE_UC);
}

void vmm_remap_framebuffer(uint64_t virt_addr, uint64_t size)
{
    if (size == 0)
        return;
    if (virt_addr > UINT64_MAX - size || virt_addr + size > UINT64_MAX - 0xFFFULL)
        return;

    uint64_t virt_start = virt_addr & ~0xFFFULL;
    uint64_t virt_end = (virt_addr + size + 0xFFFULL) & ~0xFFFULL;
    if (virt_end < virt_start)
        return;

    uint64_t pages = (virt_end - virt_start) / 0x1000ULL;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t v = virt_start + i * 0x1000;
        uint64_t phys = vmm_virt_to_phys(v);
        if (phys == 0)
            continue;

        uint64_t pml4_index = (v >> 39) & 0x1FF;
        uint64_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_WC | PTE_SHARED;
        if (g_pml4[pml4_index] & PTE_USER)
            flags |= PTE_USER;

        vmm_map_page(v, phys & ~0xFFFULL, flags);
    }
    vmm_flush_tlb_all();
}

void vmm_free_dma(const DMAAllocation &alloc)
{
    if (alloc.size == 0)
        return;
    size_t pages = (alloc.size + 4095) / 4096;
    for (size_t i = 0; i < pages; i++) {
        vmm_unmap_page_in(g_pml4, alloc.virt + i * 4096);
    }
    for (size_t i = 0; i < pages; i++) {
        pmm_free_frame(reinterpret_cast<void *>(alloc.phys + i * 4096));
    }
    dma_free_virt_range(alloc.virt, alloc.size);
    vmm_flush_tlb_all();
}

bool vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code)
{
    const bool present_fault = (error_code & 0x1) != 0;
    const bool write_fault = (error_code & 0x2) != 0;

    Process *curr = process_get_current();
    if (!curr || !curr->vma_list || !curr->page_table)
        return false;

    uint64_t sl_flags = spinlock_acquire_irqsave(curr->vma_lock_ptr);

    if (present_fault && !write_fault) {
        spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
        return false;
    }
    if (fault_addr >= 0x0000800000000000ULL) {
        spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
        return false;
    }

    VMA *vma = vma_find(curr->vma_list, fault_addr);
    if (!vma) {
        spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
        return false;
    }

    uint64_t vma_flags = vma->flags;
    VMAType vma_type = vma->type;
    uint64_t page_vaddr = fault_addr & ~0xFFFULL;
    uint64_t map_flags = vma_flags | PTE_PRESENT;
    if (page_vaddr < 0x0000800000000000ULL)
        map_flags |= PTE_USER;

    if (write_fault && !(vma_flags & PTE_WRITABLE)) {
        spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
        return false;
    }

    uint64_t phys = vmm_virt_to_phys_in(curr->page_table, page_vaddr);

    if (phys == 0) {
        void *new_frame = pmm_alloc_frame();
        if (!new_frame) {
            spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
            return false;
        }

        kstring::zero_memory(reinterpret_cast<void *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame))), 4096);

        if (!vmm_map_page_in(curr->page_table, page_vaddr, reinterpret_cast<uint64_t>(new_frame), map_flags).ok()) {
            pmm_free_frame(new_frame);
            spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
            return false;
        }
        vmm_invalidate_tlb(page_vaddr);
        spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
        return true;
    }

    if (vma_type == VMAType::Shared) {
        vmm_set_page_flags(page_vaddr, map_flags);
        spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
        return true;
    }

    uint16_t refcount = pmm_get_refcount(reinterpret_cast<void *>(phys));

    if (refcount > 1) {
        void *new_frame = pmm_alloc_frame();
        if (!new_frame) {
            spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
            return false;
        }

        uint8_t *src_virt = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(phys));
        uint8_t *dst_virt = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame)));
        kstring::memcpy(dst_virt, src_virt, 4096);

        if (!vmm_map_page_in(curr->page_table, page_vaddr, reinterpret_cast<uint64_t>(new_frame), map_flags).ok()) {
            pmm_free_frame(new_frame);
            spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
            return false;
        }
        pmm_refcount_dec(reinterpret_cast<void *>(phys));
    } else {
        if (!vmm_map_page_in(curr->page_table, page_vaddr, phys, map_flags).ok()) {
            spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
            return false;
        }
    }

    vmm_invalidate_tlb(page_vaddr);
    spinlock_release_irqrestore(curr->vma_lock_ptr, sl_flags);
    return true;
}
