#pragma once
#include <stdbool.h>
#include <stdint.h>

struct CPUFeatures
{
    bool has_sse;
    bool has_sse2;
    bool has_sse3;
    bool has_sse41;
    bool has_sse42;
    bool has_avx;
    bool has_erms;
    bool has_xsave;
    bool has_smep;
    bool has_smap;
    bool has_rdrand;
    bool has_fsgsbase;
    bool has_nx;
};

extern CPUFeatures g_cpu_features;
extern "C" uint8_t g_use_xsave;
extern "C" uint32_t g_xsave_mask_lo;
extern "C" uint32_t g_xsave_mask_hi;

struct CpuLocal
{
    uint64_t kernel_stack;
    uint64_t user_stack;
};

static inline CpuLocal *cpu_get_local()
{
    uint32_t lo = 0, hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    return reinterpret_cast<CpuLocal *>((static_cast<uint64_t>(hi) << 32) | lo);
}

extern CpuLocal g_bsp_cpu_local;
extern "C" volatile int g_cpu_online_count;

void cpu_init();
