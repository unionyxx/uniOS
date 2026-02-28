#include <stdint.h>
#include <kernel/debug.h>

void cpu_enable_sse() {
    uint32_t ebx, ecx, edx;
    char vendor[13];
    asm volatile("cpuid" : "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *reinterpret_cast<uint32_t*>(&vendor[0]) = ebx;
    *reinterpret_cast<uint32_t*>(&vendor[4]) = edx;
    *reinterpret_cast<uint32_t*>(&vendor[8]) = ecx;
    vendor[12] = '\0';
    DEBUG_INFO("CPU Vendor: %s", vendor);

    asm volatile("cpuid" : "=c"(ecx), "=d"(edx) : "a"(1));
    const bool has_sse = edx & (1 << 25);
    DEBUG_INFO("CPU Features:%s%s%s", has_sse ? " SSE" : "", (edx & (1 << 26)) ? " SSE2" : "", (ecx & (1 << 28)) ? " AVX" : "");

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (has_sse) cr4 |= (1 << 9);
    cr4 |= (1 << 10);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 2);
    cr0 |=  (1 << 1);
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}
