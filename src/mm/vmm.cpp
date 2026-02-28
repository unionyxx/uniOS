#include <kernel/mm/vmm.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vma.h>
#include <kernel/process.h>
#include <kernel/debug.h>
#include <kernel/panic.h>
#include <boot/limine.h>
#include <libk/kstring.h>

using kstring::memcpy;

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
    .response = nullptr
};

static uint64_t* g_pml4 = nullptr;
static uint64_t g_hhdm_offset = 0;

[[nodiscard]] static bool split_huge_page(uint64_t* pd, uint64_t index) {
    uint64_t huge_entry = pd[index];
    if (!(huge_entry & PTE_PAT)) return false;

    void* pt_frame = pmm_alloc_frame();
    if (!pt_frame) return false;

    uint64_t pt_phys = reinterpret_cast<uint64_t>(pt_frame);
    uint64_t* pt_virt = reinterpret_cast<uint64_t*>(pt_phys + g_hhdm_offset);

    uint64_t base_phys = huge_entry & 0x000FFFFFFFE00000ULL;
    uint64_t flags = huge_entry & 0xFFF;
    flags &= ~PTE_PAT;

    for (int i = 0; i < 512; i++) {
        pt_virt[i] = (base_phys + (i * 0x1000)) | flags;
    }

    pd[index] = pt_phys | (flags & ~PTE_PAT);
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    return true;
}

[[nodiscard]] static uint64_t* get_next_level(uint64_t* current_level, uint64_t index, bool alloc) {
    if (current_level[index] & PTE_PRESENT) {
        if (current_level[index] & PTE_PAT) {
            if (!split_huge_page(current_level, index)) return nullptr;
        }
        uint64_t phys = current_level[index] & 0x000FFFFFFFFFF000ULL;
        return reinterpret_cast<uint64_t*>(phys + g_hhdm_offset);
    }

    if (!alloc) return nullptr;

    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;

    uint64_t phys = reinterpret_cast<uint64_t>(frame);
    current_level[index] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    
    uint64_t* next_level = reinterpret_cast<uint64_t*>(phys + g_hhdm_offset);
    kstring::zero_memory(next_level, 512 * sizeof(uint64_t));
    return next_level;
}

void vmm_init() {
    if (!hhdm_request.response) return;
    g_hhdm_offset = hhdm_request.response->offset;

    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    g_pml4 = reinterpret_cast<uint64_t*>(cr3 + g_hhdm_offset);
}

uint64_t vmm_phys_to_virt(uint64_t phys) {
    return phys + g_hhdm_offset;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt >= KERNEL_STACK_TOP && (flags & PTE_USER)) panic("vmm: Kernel address with PTE_USER");
    if (virt < 0x0000800000000000ULL && !(flags & PTE_USER)) panic("vmm: Userspace address without PTE_USER");

    uint64_t* pdpt = get_next_level(g_pml4, (virt >> 39) & 0x1FF, true);
    if (!pdpt) return;

    uint64_t* pd = get_next_level(pdpt, (virt >> 30) & 0x1FF, true);
    if (!pd) return;

    uint64_t* pt = get_next_level(pd, (virt >> 21) & 0x1FF, true);
    if (!pt) return;

    pt[(virt >> 12) & 0x1FF] = phys | flags;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

static void vmm_map_page_no_flush(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt >= KERNEL_STACK_TOP && (flags & PTE_USER)) panic("vmm: Kernel address with PTE_USER");
    if (virt < 0x0000800000000000ULL && !(flags & PTE_USER)) panic("vmm: Userspace address without PTE_USER");

    uint64_t* pdpt = get_next_level(g_pml4, (virt >> 39) & 0x1FF, true);
    if (!pdpt) return;

    uint64_t* pd = get_next_level(pdpt, (virt >> 30) & 0x1FF, true);
    if (!pd) return;

    uint64_t* pt = get_next_level(pd, (virt >> 21) & 0x1FF, true);
    if (!pt) return;

    pt[(virt >> 12) & 0x1FF] = phys | flags;
}

static inline void vmm_flush_tlb_all() {
    asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

uint64_t vmm_virt_to_phys(uint64_t virt) {
    uint64_t pml4_index = (virt >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt >> 30) & 0x1FF;
    uint64_t pd_index   = (virt >> 21) & 0x1FF;
    uint64_t pt_index   = (virt >> 12) & 0x1FF;

    if (!(g_pml4[pml4_index] & PTE_PRESENT)) return 0;
    
    uint64_t* pdpt = reinterpret_cast<uint64_t*>((g_pml4[pml4_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    if (!(pdpt[pdpt_index] & PTE_PRESENT)) return 0;
    if (pdpt[pdpt_index] & PTE_PAT) {
        return (pdpt[pdpt_index] & 0x000FFFFFC0000000ULL) + (virt & 0x3FFFFFFFULL);
    }
    
    uint64_t* pd = reinterpret_cast<uint64_t*>((pdpt[pdpt_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    if (!(pd[pd_index] & PTE_PRESENT)) return 0;
    if (pd[pd_index] & PTE_PAT) {
        return (pd[pd_index] & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFFULL);
    }
    
    uint64_t* pt = reinterpret_cast<uint64_t*>((pd[pd_index] & 0x000FFFFFFFFFF000ULL) + g_hhdm_offset);
    if (!(pt[pt_index] & PTE_PRESENT)) return 0;
    
    return (pt[pt_index] & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFFULL);
}

uint64_t* vmm_get_kernel_pml4() {
    return g_pml4;
}

[[nodiscard]] static uint64_t* get_next_level_in(uint64_t* current_level, uint64_t index, bool alloc) {
    if (current_level[index] & PTE_PRESENT) {
        uint64_t phys = current_level[index] & 0x000FFFFFFFFFF000ULL;
        return reinterpret_cast<uint64_t*>(phys + g_hhdm_offset);
    }

    if (!alloc) return nullptr;

    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;

    uint64_t phys = reinterpret_cast<uint64_t>(frame);
    current_level[index] = phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    
    uint64_t* next_level = reinterpret_cast<uint64_t*>(phys + g_hhdm_offset);
    kstring::zero_memory(next_level, 512 * sizeof(uint64_t));
    return next_level;
}

void vmm_map_page_in(uint64_t* target_pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (virt >= KERNEL_STACK_TOP && (flags & PTE_USER)) panic("vmm: Kernel address with PTE_USER");
    if (virt < 0x0000800000000000ULL && !(flags & PTE_USER)) panic("vmm: Userspace address without PTE_USER");

    uint64_t* pdpt = get_next_level_in(target_pml4, (virt >> 39) & 0x1FF, true);
    if (!pdpt) return;

    uint64_t* pd = get_next_level_in(pdpt, (virt >> 30) & 0x1FF, true);
    if (!pd) return;

    uint64_t* pt = get_next_level_in(pd, (virt >> 21) & 0x1FF, true);
    if (!pt) return;

    pt[(virt >> 12) & 0x1FF] = phys | flags;
}

uint64_t* vmm_create_address_space() {
    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;
    
    uint64_t* new_pml4 = reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(frame) + g_hhdm_offset);
    kstring::zero_memory(new_pml4, 512 * sizeof(uint64_t));
    
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = g_pml4[i];
    }
    
    return new_pml4;
}

uint64_t vmm_get_hhdm_offset() {
    return g_hhdm_offset;
}

static void clone_page_table_level(uint64_t* src, uint64_t* dst, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(src[i] & PTE_PRESENT)) {
            dst[i] = 0;
            continue;
        }
        
        uint64_t src_phys = src[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t flags = src[i] & 0xFFF;
        
        if (level == 1) {
            pmm_refcount_inc(reinterpret_cast<void*>(src_phys));
            if (flags & PTE_WRITABLE) {
                flags &= ~PTE_WRITABLE;
                src[i] &= ~PTE_WRITABLE;
            }
            dst[i] = src_phys | flags;
        } else {
            void* new_table = pmm_alloc_frame();
            if (!new_table) {
                dst[i] = 0;
                continue;
            }
            
            uint64_t* new_table_virt = reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(new_table) + g_hhdm_offset);
            uint64_t* src_table = reinterpret_cast<uint64_t*>(src_phys + g_hhdm_offset);
            kstring::zero_memory(new_table_virt, 512 * sizeof(uint64_t));
            
            clone_page_table_level(src_table, new_table_virt, level - 1);
            dst[i] = reinterpret_cast<uint64_t>(new_table) | flags;
        }
    }
}

uint64_t* vmm_clone_address_space(uint64_t* src_pml4) {
    if (!src_pml4) return nullptr;
    
    void* frame = pmm_alloc_frame();
    if (!frame) return nullptr;
    
    uint64_t* new_pml4 = reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(frame) + g_hhdm_offset);
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
        uint64_t flags = src_pml4[i] & 0xFFF;
        
        void* new_pdpt = pmm_alloc_frame();
        if (!new_pdpt) {
            new_pml4[i] = 0;
            continue;
        }
        
        uint64_t* new_pdpt_virt = reinterpret_cast<uint64_t*>(reinterpret_cast<uint64_t>(new_pdpt) + g_hhdm_offset);
        uint64_t* src_pdpt = reinterpret_cast<uint64_t*>(src_phys + g_hhdm_offset);
        kstring::zero_memory(new_pdpt_virt, 512 * sizeof(uint64_t));
        
        clone_page_table_level(src_pdpt, new_pdpt_virt, 3);
        new_pml4[i] = reinterpret_cast<uint64_t>(new_pdpt) | flags;
    }
    
    return new_pml4;
}

static void free_page_table_level(uint64_t* table, int level) {
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT)) continue;
        
        uint64_t phys = table[i] & 0x000FFFFFFFFFF000ULL;
        if (level == 1) {
            pmm_free_frame(reinterpret_cast<void*>(phys));
        } else {
            uint64_t* sub_table = reinterpret_cast<uint64_t*>(phys + g_hhdm_offset);
            free_page_table_level(sub_table, level - 1);
            pmm_free_frame(reinterpret_cast<void*>(phys));
        }
    }
}

void vmm_free_address_space(uint64_t* target_pml4) {
    if (!target_pml4 || target_pml4 == g_pml4) return;
    
    for (int i = 0; i < 256; i++) {
        if (!(target_pml4[i] & PTE_PRESENT)) continue;
        
        uint64_t phys = target_pml4[i] & 0x000FFFFFFFFFF000ULL;
        uint64_t* pdpt = reinterpret_cast<uint64_t*>(phys + g_hhdm_offset);
        free_page_table_level(pdpt, 3);
        pmm_free_frame(reinterpret_cast<void*>(phys));
    }
    
    pmm_free_frame(reinterpret_cast<void*>(reinterpret_cast<uint64_t>(target_pml4) - g_hhdm_offset));
}

void vmm_switch_address_space(uint64_t* new_pml4_phys) {
    asm volatile("mov %0, %%cr3" :: "r"(new_pml4_phys) : "memory");
}

static uint64_t g_mmio_next_virt = 0xFFFFFFFF90000000ULL;

uint64_t vmm_map_mmio(uint64_t phys_addr, uint64_t size) {
    if (size == 0) return 0;
    
    uint64_t phys_page = phys_addr & ~0xFFFULL;
    uint64_t offset = phys_addr & 0xFFF;
    uint64_t pages = (size + offset + 0xFFF) / 0x1000;
    
    uint64_t virt_base = g_mmio_next_virt;
    g_mmio_next_virt += pages * 0x1000;
    
    for (uint64_t i = 0; i < pages; i++) {
        vmm_map_page_no_flush(virt_base + i * 0x1000, phys_page + i * 0x1000, PTE_MMIO);
    }
    
    vmm_flush_tlb_all();
    return virt_base + offset;
}

DMAAllocation vmm_alloc_dma(size_t pages) {
    DMAAllocation alloc = {0, 0, 0};
    
    void* phys_ptr = pmm_alloc_frames(pages);
    if (!phys_ptr) return alloc;
    
    uint64_t phys = reinterpret_cast<uint64_t>(phys_ptr);
    uint64_t virt_base = g_mmio_next_virt;
    g_mmio_next_virt += pages * 0x1000;
    
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page_no_flush(virt_base + i * 0x1000, phys + i * 0x1000, PTE_MMIO);
    }
    
    vmm_flush_tlb_all();
    
    alloc.virt = virt_base;
    alloc.phys = phys;
    alloc.size = pages * 0x1000;
    
    return alloc;
}

void vmm_remap_framebuffer(uint64_t virt_addr, uint64_t size) {
    if (size == 0) return;
    
    uint64_t virt_start = virt_addr & ~0xFFFULL;
    uint64_t virt_end = (virt_addr + size + 0xFFF) & ~0xFFFULL;
    uint64_t pages = (virt_end - virt_start) / 0x1000;
    
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virt = virt_start + i * 0x1000;
        uint64_t phys = vmm_virt_to_phys(virt);
        if (phys == 0) continue;
        vmm_map_page(virt, phys & ~0xFFFULL, PTE_WC);
    }
}

void vmm_free_dma(DMAAllocation alloc) {
    if (alloc.size == 0) return;
    size_t pages = (alloc.size + 4095) / 4096;
    for (size_t i = 0; i < pages; i++) {
        pmm_free_frame(reinterpret_cast<void*>(alloc.phys + i * 4096));
    }
}

bool vmm_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    if (!(error_code & 2)) return false;
    
    Process* curr = process_get_current();
    if (!curr || !curr->vma_list) return false;
    
    VMA* vma = vma_find(curr->vma_list, fault_addr);
    if (!vma || !(vma->flags & PTE_WRITABLE)) return false;
    
    uint64_t page_vaddr = fault_addr & ~0xFFFULL;
    uint64_t phys = vmm_virt_to_phys(page_vaddr);
    
    if (phys == 0) return false;
    
    uint16_t refcount = pmm_get_refcount(reinterpret_cast<void*>(phys));
    
    if (refcount > 1) {
        void* new_frame = pmm_alloc_frame();
        if (!new_frame) panic("OOM during CoW page fault!");
        
        uint8_t* src_virt = reinterpret_cast<uint8_t*>(vmm_phys_to_virt(phys));
        uint8_t* dst_virt = reinterpret_cast<uint8_t*>(vmm_phys_to_virt(reinterpret_cast<uint64_t>(new_frame)));
        
        kstring::memcpy(dst_virt, src_virt, 4096);
        vmm_map_page(page_vaddr, reinterpret_cast<uint64_t>(new_frame), vma->flags | PTE_PRESENT | PTE_USER);
        pmm_refcount_dec(reinterpret_cast<void*>(phys));
        
        DEBUG_INFO("CoW: PID %d duplicated page at 0x%llx (frame 0x%llx -> 0x%llx)", curr->pid, page_vaddr, phys, reinterpret_cast<uint64_t>(new_frame));
    } else {
        vmm_map_page(page_vaddr, phys, vma->flags | PTE_PRESENT | PTE_USER);
        DEBUG_INFO("CoW: PID %d restored write permission on page 0x%llx", curr->pid, page_vaddr);
    }
    
    return true;
}
