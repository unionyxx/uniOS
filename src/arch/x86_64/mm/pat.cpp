#include <kernel/arch/x86_64/pat.h>
#include <kernel/debug.h>

// Read MSR
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint8_t pat_entry(uint64_t pat, unsigned index)
{
    return (uint8_t)((pat >> (index * 8U)) & 0xFFU);
}

static inline uint64_t pat_with_entry(uint64_t pat, unsigned index, uint8_t type)
{
    uint64_t shift = (uint64_t)index * 8ULL;
    pat &= ~(0xFFULL << shift);
    pat |= ((uint64_t)type << shift);
    return pat;
}

// Check CPUID for PAT support
bool pat_is_supported()
{
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    // PAT is bit 16 of EDX
    return (edx & (1 << 16)) != 0;
}

// Must run on EVERY core: IA32_PAT is per-logical-processor, and an AP left
// with the reset-default PAT would interpret PTE_WC (PAT index 2) as UC
// instead of WC, silently wrecking framebuffer/rendering performance.
[[gnu::target("no-sse")]] void pat_init()
{
    if (!pat_is_supported()) {
        DEBUG_WARN("PAT not supported by CPU");
        return;
    }

    uint64_t old_pat = rdmsr(IA32_PAT_MSR);
    if (pat_entry(old_pat, 2) == PAT_WC)
        return;

    // SDM-mandated sequence for changing PAT: interrupts off, caches disabled
    // and flushed around the MSR write, then flush again before re-enabling.
    uint64_t flags;
    asm volatile("pushfq\npopq %0" : "=r"(flags));
    asm volatile("cli");

    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %0, %%cr0" ::"r"(cr0 | (1ULL << 30)) : "memory"); // CR0.CD
    asm volatile("wbinvd");

    wrmsr(IA32_PAT_MSR, pat_with_entry(old_pat, 2, PAT_WC));

    asm volatile("wbinvd");
    asm volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");

    if ((flags & (1ULL << 9)) != 0)
        asm volatile("sti");

    uint64_t verify_pat = rdmsr(IA32_PAT_MSR);
    if (pat_entry(verify_pat, 2) != PAT_WC)
        DEBUG_WARN("PAT configuration verify failed for entry 2");
}
