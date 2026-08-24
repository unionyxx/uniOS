#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Maximum number of CPUs the kernel will bring up, including the BSP.
constexpr uint32_t CONFIG_SMP_MAX_CPUS = 8;

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

struct Process;

// Per-core kernel data. Once a core is configured, its IA32_GS_BASE and
// IA32_KERNEL_GS_BASE both point at its own instance.
//
// ABI contract with src/arch/x86_64/boot/usermode.asm: [gs:0] = kernel_stack,
// [gs:8] = user_stack. These two fields must stay first.
struct alignas(64) PerCpu
{
    uint64_t kernel_stack;
    uint64_t user_stack;
    Process *current;
    Process *idle;
    uint32_t cpu_id;
    uint32_t apic_id;
    volatile bool online;
    volatile bool stop_requested;
};

static_assert(offsetof(PerCpu, kernel_stack) == 0, "usermode.asm ABI: [gs:0]");
static_assert(offsetof(PerCpu, user_stack) == 8, "usermode.asm ABI: [gs:8]");

// Index 0 is the BSP. All entries are page-aligned instances; unused slots stay zeroed.
extern PerCpu g_cpus[CONFIG_SMP_MAX_CPUS];
extern "C" volatile int g_cpu_online_count;
extern "C" volatile int g_cpu_stopped_count;

static inline PerCpu *cpu_get_local()
{
    uint32_t lo = 0, hi = 0;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    return reinterpret_cast<PerCpu *>((static_cast<uint64_t>(hi) << 32) | lo);
}

// Returns nullptr for cores not seen in the MADT/started yet.
PerCpu *cpu_by_apic_id(uint32_t apic_id);

void cpu_init();

// Full per-core bring-up: control registers from BSP-detected features, FPU,
// syscall MSRs, and GS_BASE -> g_cpus[cpu_id]. BSP calls it via cpu_init(),
// each AP after trampoline handoff.
[[gnu::target("no-sse")]] void cpu_core_setup(uint32_t cpu_id, uint32_t apic_id);

uint32_t cpu_bsp_apic_id();
