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

void pat_init()
{
    if (!pat_is_supported()) {
        DEBUG_WARN("PAT not supported by CPU");
        return;
    }

    uint64_t old_pat = rdmsr(IA32_PAT_MSR);
    if (pat_entry(old_pat, 2) == PAT_WC) {
        DEBUG_INFO("PAT entry 2 already configured as Write-Combining");
        return;
    }

    uint64_t new_pat = pat_with_entry(old_pat, 2, PAT_WC);
    wrmsr(IA32_PAT_MSR, new_pat);

    uint64_t verify_pat = rdmsr(IA32_PAT_MSR);
    if (pat_entry(verify_pat, 2) != PAT_WC) {
        DEBUG_WARN("PAT configuration verify failed for entry 2");
        return;
    }

    DEBUG_INFO("PAT configured: entry 2 = Write-Combining");
}
