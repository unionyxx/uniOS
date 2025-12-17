#include "pat.h"
#include "debug.h"

// Read MSR
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Write MSR
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Check CPUID for PAT support
bool pat_is_supported() {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    // PAT is bit 16 of EDX
    return (edx & (1 << 16)) != 0;
}

void pat_init() {
    if (!pat_is_supported()) {
        DEBUG_WARN("PAT not supported by CPU");
        return;
    }
    
    // Read current PAT MSR
    uint64_t pat = rdmsr(IA32_PAT_MSR);
    
    // Default PAT layout (at reset):
    // PAT0 = WB (0x06)  - index 0 (PCD=0, PWT=0, PAT=0)
    // PAT1 = WT (0x04)  - index 1 (PCD=0, PWT=1, PAT=0)
    // PAT2 = UC- (0x07) - index 2 (PCD=1, PWT=0, PAT=0)
    // PAT3 = UC (0x00)  - index 3 (PCD=1, PWT=1, PAT=0)
    // PAT4 = WB (0x06)  - index 4 (PCD=0, PWT=0, PAT=1)
    // PAT5 = WT (0x04)  - index 5 (PCD=0, PWT=1, PAT=1)
    // PAT6 = UC- (0x07) - index 6 (PCD=1, PWT=0, PAT=1)
    // PAT7 = UC (0x00)  - index 7 (PCD=1, PWT=1, PAT=1)
    
    // We want to set PAT2 (index 2) to Write-Combining (0x01)
    // This is accessed with PCD=1, PWT=0, PAT=0
    // Clear PAT2 (bits 16-23) and set to WC
    pat &= ~(0xFFULL << 16);  // Clear PAT2
    pat |= ((uint64_t)PAT_WC << 16);  // Set PAT2 = WC
    
    // Write updated PAT MSR
    wrmsr(IA32_PAT_MSR, pat);
    
    DEBUG_INFO("PAT configured: entry 2 = Write-Combining");
}
