#include <boot/boot_info.h>
#include <drivers/acpi/acpi.h>
#include <drivers/apic/ioapic.h>
#include <kernel/arch/x86_64/gdt.h>
#include <kernel/arch/x86_64/idt.h>
#include <kernel/arch/x86_64/pat.h>
#include <kernel/cpu.h>
#include <kernel/debug.h>
#include <kernel/irq.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/smp.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

// Trampoline page layout (offsets within the copied page).
namespace {
constexpr uint32_t SMP_TRAMPOLINE_VECTOR_LIMIT = 0x100; // SIPI vector: phys page < 1 MiB
constexpr uint64_t SMP_LOW_MEM_CEILING = 0x90000;       // stay below EBDA/legacy regions
constexpr uint32_t SMP_INIT_DELAY_MS = 10;
constexpr uint32_t SMP_ONLINE_TIMEOUT_MS = 200;

// Trampoline CR3 layout: identity map of the low 2 MiB plus a copy of the
// kernel PML4's upper half, so both the trampoline page and higher-half
// kernel .text/.data are reachable before the real CR3 is loaded.
constexpr uint64_t TPTE_PRESENT = 1ULL << 0;
constexpr uint64_t TPTE_WRITABLE = 1ULL << 1;
constexpr uint64_t TPTE_HUGE = 1ULL << 7;

struct alignas(16) TrampParams
{
    uint32_t tpml4_phys;
    uint32_t pad;
    uint64_t stack_top;
    uint64_t kcr3;
    uint64_t entry;
    uint64_t percpu;
};

// Fixed trampoline page layout — MUST match smp_trampoline.asm.
constexpr size_t TR_OFF_TPML4 = 0x180;
constexpr size_t TR_OFF_STACK = 0x188;
constexpr size_t TR_OFF_KCR3 = 0x190;
constexpr size_t TR_OFF_ENTRY = 0x198;
constexpr size_t TR_OFF_PERCPU = 0x1A0;
constexpr size_t TR_OFF_GDT = 0x200;
constexpr size_t TR_OFF_GDT_DESC = 0x218;

} // namespace

extern "C" const void *smp_trampoline_start;
extern "C" const void *smp_trampoline_end;
extern "C" const void *smp_trampoline_farjmp;

namespace {

// Offset of the 64-bit continuation within the trampoline page; the patched
// far jump targets trampoline_phys + this value.
constexpr size_t TR_OFF_LM_CONT = 0x80;

inline size_t blob_offset(const void *sym)
{
    return reinterpret_cast<uintptr_t>(sym) - reinterpret_cast<uintptr_t>(&smp_trampoline_start);
}
} // namespace

// Per-core idle body: sleeps until a RESCHED IPI or timer tick, then pulls
// runnable work from the global runqueue via scheduler_yield().
extern "C" [[gnu::target("no-sse")]] void smp_idle_entry()
{
    for (;;) {
        asm volatile("sti\n"
                     "hlt\n"
                     "cli\n");
        scheduler_yield();
    }
}

// Naked handoff from the low-memory trampoline. Runs at a higher-half address
// still mapped under the TRAMPOLINE CR3; switching CR3 here keeps RIP valid.
// ap_main never returns.
extern "C" [[gnu::naked, gnu::target("no-sse")]] void ap_long_mode_entry()
{
    asm volatile("mov %rcx, %cr3\n"
                 "xor %ebp, %ebp\n"
                 "call ap_main\n"
                 "1:\n"
                 "cli\n"
                 "hlt\n"
                 "jmp 1b\n");
}

extern "C" [[gnu::target("no-sse")]] void ap_main(PerCpu *cpu)
{
    if (!cpu || cpu->cpu_id >= CONFIG_SMP_MAX_CPUS) {
        asm volatile("1:\ncli\nhlt\njmp 1b\n");
    }

    // Control registers, FPU, per-core syscall MSRs, GS -> this PerCpu.
    cpu_core_setup(cpu->cpu_id, cpu->apic_id);

    // IA32_PAT is per-core; without this an AP keeps the reset-default PAT
    // and interprets PTE_WC as UC.
    pat_init();

    // Load the per-core GDT/TSS (and with it the IST stacks) BEFORE the IDT:
    // an NMI/#DF/#PF arriving in between would otherwise resolve IST against
    // the trampoline GDT, which has no TSS.
    gdt_init_cpu(cpu->cpu_id);
    idt_load(); // shared interrupt table

    apic_enable_this_core();
    apic_timer_start_this_core(apic_timer_bsp_initcnt());

    // Adopt the per-core idle context and start pulling work from the global
    // runqueue. Never returns; the online flag and count are published inside
    // the scheduler handoff (under the sched lock) so this core is only
    // visible to IPI/shootdown senders once its LAPIC is enabled.
    scheduler_enter_idle(cpu->idle);
}

namespace {

// Preference for low page sources:
//  1. BOOTLOADER_RECLAIMABLE — the loader asked the firmware for a page below
//     1 MiB, so this is firmware-blessed RAM.
//  2. USABLE — conventional low RAM (QEMU/OVMF exposes some).
//  3. RESERVED/ACPI_RECLAIMABLE — real UEFI firmware rarely reports any
//     usable RAM below 1 MiB; conventional low RAM usually shows up as
//     reserved. The ceiling keeps clear of VGA/option-ROM space, and the
//     trampoline's own scratch stack lives in this same region already.
int low_page_rank(uint64_t type)
{
    switch (type) {
        case BOOT_MEM_BOOTLOADER_RECLAIMABLE:
            return 3;
        case BOOT_MEM_USABLE:
            return 2;
        case BOOT_MEM_RESERVED:
        case BOOT_MEM_ACPI_RECLAIMABLE:
            return 1;
        default:
            return 0;
    }
}

bool find_low_trampoline_page(uint64_t &out_phys)
{
    const BootInfo *bi = boot_get_info();
    if (!bi || !bi->memory_map)
        return false;

    uint64_t best = 0;
    int best_rank = 0;
    for (uint64_t i = 0; i < bi->memory_map_count; i++) {
        const BootMemoryMapEntry *e = &bi->memory_map[i];
        const int rank = low_page_rank(e->type);
        if (rank == 0)
            continue;

        uint64_t start = (e->base + 0xFFFULL) & ~0xFFFULL;
        const uint64_t end = e->base + e->length;
        // Keep clear of the IVT/BDA and the trampoline's own scratch stack
        // region below 0x8000.
        if (start < 0x10000)
            start = 0x10000;
        if (end <= start)
            continue;

        const uint64_t ceiling = end < SMP_LOW_MEM_CEILING ? end : SMP_LOW_MEM_CEILING;
        if (ceiling <= start)
            continue;

        const uint64_t candidate = ((ceiling - 1) & ~0xFFFULL); // last full page
        if (candidate < start || (candidate >> 12) >= SMP_TRAMPOLINE_VECTOR_LIMIT)
            continue;
        if (rank > best_rank || (rank == best_rank && candidate > best)) {
            best = candidate;
            best_rank = rank;
        }
    }

    if (best == 0)
        return false;

    out_phys = best;
    return true;
}

bool build_trampoline_page_tables(uint8_t *tramp_page, uint32_t &tpml4_phys_out)
{
    void *pml4 = pmm_alloc_frame();
    void *pdpt = pmm_alloc_frame();
    void *pd = pmm_alloc_frame();
    if (!pml4 || !pdpt || !pd)
        return false;

    auto *pml4v = reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(pml4) + vmm_get_hhdm_offset());
    auto *pdptv = reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(pdpt) + vmm_get_hhdm_offset());
    auto *pdv = reinterpret_cast<uint64_t *>(reinterpret_cast<uintptr_t>(pd) + vmm_get_hhdm_offset());

    // Identity-map the first 2 MiB (covers any SIPI vector page).
    pdv[0] = 0 | TPTE_PRESENT | TPTE_WRITABLE | TPTE_HUGE;

    pdptv[0] = reinterpret_cast<uint64_t>(pd) | TPTE_PRESENT | TPTE_WRITABLE;

    for (int i = 0; i < 512; i++)
        pml4v[i] = 0;
    pml4v[0] = reinterpret_cast<uint64_t>(pdpt) | TPTE_PRESENT | TPTE_WRITABLE;

    // Share the kernel's upper half so higher-half addresses resolve under the
    // trampoline CR3 too. The subtrees are shared live structures, not copies.
    const uint64_t *kpml4 = vmm_get_kernel_pml4();
    if (!kpml4)
        return false;
    for (int i = 256; i < 512; i++)
        pml4v[i] = kpml4[i];

    tpml4_phys_out = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pml4));

    // Params live inside the trampoline page itself.
    auto *params = reinterpret_cast<TrampParams *>(tramp_page + TR_OFF_TPML4);
    params->tpml4_phys = tpml4_phys_out;
    params->pad = 0;
    params->stack_top = 0; // filled per-AP
    params->kcr3 = reinterpret_cast<uint64_t>(vmm_get_kernel_pml4()) - vmm_get_hhdm_offset();
    params->entry = reinterpret_cast<uint64_t>(&ap_long_mode_entry);
    params->percpu = 0; // filled per-AP

    return true;
}

uint32_t g_madt_ap_ids[CONFIG_SMP_MAX_CPUS];

uint32_t enumerate_madt_aps()
{
    const auto *madt = reinterpret_cast<const AcpiMadtHeader *>(acpi_find_table("APIC"));
    if (!madt)
        return 0;

    uint32_t count = 0;
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(madt) + sizeof(AcpiMadtHeader);
    const uint8_t *end = reinterpret_cast<const uint8_t *>(madt) + madt->header.length;

    while (ptr + sizeof(AcpiMadtRecord) <= end) {
        const auto *record = reinterpret_cast<const AcpiMadtRecord *>(ptr);
        if (record->length < sizeof(AcpiMadtRecord) || ptr + record->length > end)
            break;

        if (record->type == 0 && record->length >= sizeof(AcpiMadtLapic)) {
            const auto *lapic = reinterpret_cast<const AcpiMadtLapic *>(ptr);
            constexpr uint32_t LAPIC_ENABLED = 1u << 0;
            if ((lapic->flags & LAPIC_ENABLED) && lapic->apic_id != cpu_bsp_apic_id()) {
                if (count < CONFIG_SMP_MAX_CPUS - 1) {
                    g_madt_ap_ids[count++] = lapic->apic_id;
                }
            }
        }

        ptr += record->length;
    }
    return count;
}

bool start_ap(uint32_t slot, uint32_t apic_id, uint8_t sipi_vector, uint8_t *tramp_page)
{
    // Per-CPU block + private idle task (which owns the AP's kernel stack).
    PerCpu *cpu = &g_cpus[slot];
    cpu->current = nullptr;

    Process *idle = scheduler_create_idle_task(smp_idle_entry, "Idle");
    if (!idle) {
        BOOT_ERROR("SMP: no idle task for AP %u", apic_id);
        return false;
    }
    cpu->idle = idle;

    // Dedicated bootstrap stack for the AP's pre-scheduler C code. It must be
    // SEPARATE from the idle task's stack: scheduler_enter_idle() switches
    // onto idle->sp, and enter_idle's own live frames would clobber the
    // prepared bootstrap context if both lived on the same buffer (the AP
    // then jumps into the weeds still holding g_sched_lock, freezing the
    // machine). The bootstrap stack is abandoned after the handoff.
    void *boot_stack = pmm_alloc_frames(KERNEL_STACK_SIZE / 4096);
    if (!boot_stack) {
        BOOT_ERROR("SMP: no bootstrap stack for AP %u", apic_id);
        return false;
    }
    const uint64_t stack_top = reinterpret_cast<uint64_t>(boot_stack) + vmm_get_hhdm_offset() + KERNEL_STACK_SIZE;

    cpu->cpu_id = slot;
    cpu->apic_id = apic_id;
    cpu->kernel_stack = stack_top;
    cpu->user_stack = 0;
    __atomic_store_n(&cpu->online, false, __ATOMIC_RELEASE);

    auto *params = reinterpret_cast<TrampParams *>(tramp_page + TR_OFF_TPML4);
    params->stack_top = stack_top;
    params->percpu = reinterpret_cast<uint64_t>(cpu);

    // Quiet bus during the INIT/SIPI handshake.
    const uint64_t flags = interrupts_save_disable();

    // Full INIT-SIPI protocol: under OVMF the APs are parked inside firmware
    // (long-mode) loops, not in classic wait-for-SIPI, so the INIT must put
    // them back into real mode before the SIPI can take effect. Real chipsets
    // occasionally lose the first SIPI (SMM interference, posted IPIs), so
    // the sequence is INIT, deassert, SIPI, short wait, second SIPI.
    BOOT_LOG("SMP: sending INIT to AP %u", apic_id);
    apic_send_init_ipi(static_cast<uint8_t>(apic_id));
    timer_poll_wait_ms(SMP_INIT_DELAY_MS);
    apic_send_init_deassert_ipi(static_cast<uint8_t>(apic_id));

    BOOT_LOG("SMP: sending SIPI vector 0x%x to AP %u", sipi_vector, apic_id);
    apic_send_sipi(static_cast<uint8_t>(apic_id), sipi_vector);

    // Brief poll (~200 us or more) before the second SIPI; it is ignored by
    // an AP that already left wait-for-SIPI state.
    for (uint32_t i = 0; i < 1000000; i++) {
        if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE))
            break;
        asm volatile("pause");
    }
    if (!__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) {
        BOOT_LOG("SMP: sending second SIPI to AP %u", apic_id);
        apic_send_sipi(static_cast<uint8_t>(apic_id), sipi_vector);
    }
    interrupts_restore(flags);
    BOOT_LOG("SMP: waiting for AP %u online flag", apic_id);

    // Ticks must advance for the timeout below, but the boot thread must not
    // be switched away from (the scheduler would land in an idle task and
    // never return here). Block preemption rather than masking interrupts.
    preempt_disable();
    asm volatile("sti");

    // Wait for the AP to consume the params and mark itself online. Params
    // are rewritten only after this succeeds, so sequential starts are safe.
    const uint64_t deadline = timer_get_ticks() + SMP_ONLINE_TIMEOUT_MS;
    bool online = false;
    while (timer_get_ticks() < deadline) {
        if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE)) {
            online = true;
            break;
        }
        asm volatile("pause");
    }

    asm volatile("cli");
    preempt_enable();

    if (!online) {
        // Last chance: the AP may have crossed the finish line just past the
        // deadline. Checking again matters because the recovery below INITs
        // the core, which must never hit an already-running AP.
        if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE))
            return true;

        // Re-park the late AP before this function returns: the trampoline
        // param block is rewritten for the NEXT AP, and a straggler waking up
        // afterwards would otherwise consume another core's stack/PerCpu.
        // INIT puts it back into wait-for-SIPI (no further SIPI is sent to
        // it), so it stays parked.
        BOOT_WARN("SMP: AP %u did not come online in time, re-parking", apic_id);
        const uint64_t park_flags = interrupts_save_disable();
        apic_send_init_ipi(static_cast<uint8_t>(apic_id));
        timer_poll_wait_ms(1);
        apic_send_init_deassert_ipi(static_cast<uint8_t>(apic_id));
        interrupts_restore(park_flags);
        return false;
    }
    return true;
}

} // namespace

void smp_init()
{
    const uint32_t ap_count = enumerate_madt_aps();
    BOOT_LOG("SMP: MADT lists %u AP(s)", ap_count);
    if (ap_count == 0) {
        return;
    }

    uint64_t tramp_phys = 0;
    if (!find_low_trampoline_page(tramp_phys) || !pmm_reserve_range(tramp_phys, 1)) {
        BOOT_WARN("SMP: no free low-memory page for trampoline, staying single-core");
        return;
    }
    BOOT_LOG("SMP: trampoline page at phys 0x%lx", tramp_phys);

    auto *tramp_page = reinterpret_cast<uint8_t *>(vmm_phys_to_virt(tramp_phys));
    const size_t blob_size =
        reinterpret_cast<uintptr_t>(&smp_trampoline_end) - reinterpret_cast<uintptr_t>(&smp_trampoline_start);
    kstring::memcpy(tramp_page, &smp_trampoline_start, blob_size);

    uint32_t tpml4_phys = 0;
    if (!build_trampoline_page_tables(tramp_page, tpml4_phys)) {
        BOOT_WARN("SMP: failed to build trampoline page tables, staying single-core");
        return;
    }

    // Patch the lgdt descriptor's linear base and the far-jump target to the
    // copied page's addresses. The asm label sits directly on the immediate.
    *reinterpret_cast<volatile uint32_t *>(tramp_page + TR_OFF_GDT_DESC + 2) =
        static_cast<uint32_t>(tramp_phys + TR_OFF_GDT);
    const size_t farjmp_imm_off = blob_offset(&smp_trampoline_farjmp);
    *reinterpret_cast<volatile uint32_t *>(tramp_page + farjmp_imm_off) =
        static_cast<uint32_t>(tramp_phys + TR_OFF_LM_CONT);

    const uint8_t sipi_vector = static_cast<uint8_t>(tramp_phys >> 12);
    BOOT_LOG("SMP: trampoline ready (vector=0x%x, tpml4=0x%x)", sipi_vector, tpml4_phys);

    uint32_t started = 0;
    uint32_t slot = 1;
    for (uint32_t i = 0; i < ap_count && slot < CONFIG_SMP_MAX_CPUS; i++) {
        BOOT_LOG("SMP: starting AP %u -> slot %u", g_madt_ap_ids[i], slot);
        if (start_ap(slot, g_madt_ap_ids[i], sipi_vector, tramp_page))
            started++;
        slot++;
    }

    if (started == 0) {
        BOOT_WARN("SMP: no APs started, continuing single-core");
        return;
    }

    BOOT_SUCCESS("SMP: %u of %u AP(s) online (%d CPUs total)", started, ap_count,
                 __atomic_load_n(&g_cpu_online_count, __ATOMIC_ACQUIRE));
    BOOT_SUCCESS("SMP scheduler ready on %d CPUs", __atomic_load_n(&g_cpu_online_count, __ATOMIC_ACQUIRE));
}

uint32_t smp_ap_count()
{
    return enumerate_madt_aps();
}
