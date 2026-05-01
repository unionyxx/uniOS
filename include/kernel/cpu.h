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

extern CpuLocal g_bsp_cpu_local;

void cpu_init();
