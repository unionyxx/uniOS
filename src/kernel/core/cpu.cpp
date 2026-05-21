#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <libk/kstring.h>
#include <stdint.h>

CPUFeatures g_cpu_features = {};

// Exported to assembly for runtime dispatch between xsave/fxsave
extern "C" uint8_t g_use_xsave;
uint8_t g_use_xsave = 0;
extern "C" uint32_t g_xsave_mask_lo;
extern "C" uint32_t g_xsave_mask_hi;
uint32_t g_xsave_mask_lo = 0;
uint32_t g_xsave_mask_hi = 0;

CpuLocal g_bsp_cpu_local;
volatile int g_cpu_online_count = 1;

extern "C" void syscall_entry();

namespace {
enum : uint32_t
{
    kMsrStar = 0xC0000081,
    kMsrLstar = 0xC0000082,
    kMsrSfmask = 0xC0000084,
    kMsrEfer = 0xC0000080,
    kMsrGsBase = 0xC0000101,
    kMsrKernelGsBase = 0xC0000102,
};

enum : uint64_t
{
    kCr0Mp = 1ULL << 1,
    kCr0Em = 1ULL << 2,
    kCr0Ts = 1ULL << 3,
    kCr0Ne = 1ULL << 5,
    kCr0Wp = 1ULL << 16,

    kCr4Fsgsbase = 1ULL << 16,
    kCr4Osxsave = 1ULL << 18,
    kCr4Osfxsr = 1ULL << 9,
    kCr4Osxmmexcpt = 1ULL << 10,

    kXcr0X87 = 1ULL << 0,
    kXcr0Sse = 1ULL << 1,
    kXcr0Avx = 1ULL << 2,

    kEferSce = 1ULL << 0,
    kEferNxe = 1ULL << 11,

    kRflagsTf = 1ULL << 8,
    kRflagsIf = 1ULL << 9,
    kRflagsDf = 1ULL << 10,
    kRflagsNt = 1ULL << 14,
    kRflagsRf = 1ULL << 16,
    kRflagsAc = 1ULL << 18,
};

[[gnu::target("no-sse")]] static inline void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t &eax, uint32_t &ebx,
                                                   uint32_t &ecx, uint32_t &edx)
{
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}

[[gnu::target("no-sse")]] static inline uint64_t rdmsr64(uint32_t msr)
{
    uint32_t lo = 0;
    uint32_t hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

[[gnu::target("no-sse")]] static inline void wrmsr64(uint32_t msr, uint64_t value)
{
    asm volatile("wrmsr" ::"a"(static_cast<uint32_t>(value)), "d"(static_cast<uint32_t>(value >> 32)), "c"(msr));
}
} // namespace

[[gnu::target("no-sse")]] void syscall_init()
{
    // Initialize per-CPU data for BSP
    g_bsp_cpu_local.kernel_stack = 0; // Will be set by scheduler
    g_bsp_cpu_local.user_stack = 0;

    const uint64_t gs_base = reinterpret_cast<uint64_t>(&g_bsp_cpu_local);
    wrmsr64(kMsrGsBase, gs_base);
    wrmsr64(kMsrKernelGsBase, gs_base);

    // IA32_STAR MSR.
    // AMD SYSRET quirk: program the user selector base with RPL=3 (0x13 instead of 0x10)
    // so SYSRET/interrupt return paths synthesize the correct privilege level selectors.
    const uint64_t star = (static_cast<uint64_t>(0x13) << 48) | (static_cast<uint64_t>(0x08) << 32);
    wrmsr64(kMsrStar, star);

    // IA32_LSTAR MSR: entry point for `syscall`.
    const uint64_t lstar = reinterpret_cast<uint64_t>(syscall_entry);
    wrmsr64(kMsrLstar, lstar);

    // IA32_FMASK/SFMASK: clear flags that should never leak from userspace into kernel mode.
    const uint64_t sfmask = kRflagsTf | kRflagsIf | kRflagsDf | kRflagsNt | kRflagsRf | kRflagsAc;
    wrmsr64(kMsrSfmask, sfmask);

    uint64_t efer = rdmsr64(kMsrEfer);
    efer |= kEferSce;
    wrmsr64(kMsrEfer, efer);
}

[[gnu::target("no-sse")]] void cpu_init()
{
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    char vendor[13] = {};

    g_cpu_features = {};
    g_use_xsave = 0;
    g_xsave_mask_lo = 0;
    g_xsave_mask_hi = 0;

    cpuid(0, 0, eax, ebx, ecx, edx);
    const uint32_t max_basic_leaf = eax;
    kstring::memcpy(&vendor[0], &ebx, sizeof(ebx));
    kstring::memcpy(&vendor[4], &edx, sizeof(edx));
    kstring::memcpy(&vendor[8], &ecx, sizeof(ecx));
    vendor[12] = '\0';
    DEBUG_INFO("CPU Vendor: %s (Max Leaf: 0x%x)", vendor, max_basic_leaf);

    cpuid(0x80000000U, 0, eax, ebx, ecx, edx);
    const uint32_t max_extended_leaf = eax;

    if (max_basic_leaf >= 1) {
        cpuid(1, 0, eax, ebx, ecx, edx);
        g_cpu_features.has_sse = (edx & (1U << 25)) != 0;
        g_cpu_features.has_sse2 = (edx & (1U << 26)) != 0;
        g_cpu_features.has_sse3 = (ecx & (1U << 0)) != 0;
        g_cpu_features.has_sse41 = (ecx & (1U << 19)) != 0;
        g_cpu_features.has_sse42 = (ecx & (1U << 20)) != 0;
        g_cpu_features.has_xsave = (ecx & (1U << 26)) != 0;
        g_cpu_features.has_avx = (ecx & (1U << 28)) != 0;
        g_cpu_features.has_rdrand = (ecx & (1U << 30)) != 0;
    }

    if (max_basic_leaf >= 7) {
        cpuid(7, 0, eax, ebx, ecx, edx);
        g_cpu_features.has_fsgsbase = (ebx & (1U << 0)) != 0;
        g_cpu_features.has_smep = (ebx & (1U << 7)) != 0;
        g_cpu_features.has_erms = (ebx & (1U << 9)) != 0;
        g_cpu_features.has_smap = (ebx & (1U << 20)) != 0;
    }

    if (max_extended_leaf >= 0x80000001U) {
        cpuid(0x80000001U, 0, eax, ebx, ecx, edx);
        g_cpu_features.has_nx = (edx & (1U << 20)) != 0;
    }

    const bool enable_xsave = g_cpu_features.has_xsave && max_basic_leaf >= 0x0DU;
    const bool enable_avx = enable_xsave && g_cpu_features.has_sse && g_cpu_features.has_avx;

    uint64_t cr4 = 0;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (g_cpu_features.has_sse) {
        cr4 |= kCr4Osfxsr;
        cr4 |= kCr4Osxmmexcpt;
    }
    // SMEP and SMAP remain disabled until all user mappings and entry paths are audited.
    if (enable_xsave)
        cr4 |= kCr4Osxsave;
    if (g_cpu_features.has_fsgsbase)
        cr4 |= kCr4Fsgsbase;
    asm volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");

    if (enable_xsave) {
        uint64_t xcr0 = kXcr0X87;
        if (g_cpu_features.has_sse)
            xcr0 |= kXcr0Sse;
        if (enable_avx)
            xcr0 |= kXcr0Avx;
        asm volatile("xsetbv" ::"a"(static_cast<uint32_t>(xcr0)), "d"(static_cast<uint32_t>(xcr0 >> 32)), "c"(0));
        g_xsave_mask_lo = static_cast<uint32_t>(xcr0);
        g_xsave_mask_hi = static_cast<uint32_t>(xcr0 >> 32);
        g_use_xsave = 1;
    } else {
        g_cpu_features.has_xsave = false;
        g_cpu_features.has_avx = false;
    }

    uint64_t cr0 = 0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~kCr0Em;
    cr0 &= ~kCr0Ts;
    cr0 |= kCr0Mp;
    cr0 |= kCr0Ne;
    cr0 |= kCr0Wp;
    asm volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");

    if (g_cpu_features.has_nx) {
        uint64_t efer = rdmsr64(kMsrEfer);
        efer |= kEferNxe;
        wrmsr64(kMsrEfer, efer);
    }

    asm volatile("fninit");

    syscall_init();

    BOOT_LOG("Features: SSE:%d SSE2:%d XSAVE:%d AVX:%d ERMS:%d SMEP:%d SMAP:%d NX:%d RDRAND:%d", g_cpu_features.has_sse,
             g_cpu_features.has_sse2, g_cpu_features.has_xsave, g_cpu_features.has_avx, g_cpu_features.has_erms,
             g_cpu_features.has_smep, g_cpu_features.has_smap, g_cpu_features.has_nx, g_cpu_features.has_rdrand);
}
