#include <stdint.h>
#include <kernel/debug.h>

void cpu_enable_sse() {
    uint32_t eax, ebx, ecx, edx;
    
    // Get vendor string
    char vendor[13];
    asm volatile("cpuid" : "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = '\0';
    
    DEBUG_INFO("CPU Vendor: %s", vendor);

    // Get features
    asm volatile("cpuid" : "=c"(ecx), "=d"(edx) : "a"(1));
    bool has_sse = edx & (1 << 25);
    bool has_sse2 = edx & (1 << 26);
    bool has_avx = ecx & (1 << 28);
    
    DEBUG_INFO("CPU Features:%s%s%s", 
               has_sse ? " SSE" : "", 
               has_sse2 ? " SSE2" : "",
               has_avx ? " AVX" : "");

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (has_sse) cr4 |= (1 << 9);   // OSFXSR
    cr4 |= (1 << 10);  // OSXMMEXCPT
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);  // Clear EM
    cr0 |=  (1 << 1);  // Set MP
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}
