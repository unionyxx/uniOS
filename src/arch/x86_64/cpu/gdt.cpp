#include <kernel/arch/x86_64/gdt.h>
#include <kernel/cpu.h>

// Per-core segment state. Selector layout is identical on every core
// (0x08 kcode, 0x10 kdata, 0x1B udata, 0x23 ucode, 0x28 TSS) so shared
// entry assembly works everywhere; only TSS contents differ per core.
struct alignas(0x1000) CoreSegmentArea
{
    gdt_entry entries[7]; // Null, Kernel Code, Kernel Data, User Code, User Data, TSS (Low), TSS (High)
    tss_entry tss;
    uint8_t rsp0_stack[4096];
    uint8_t double_fault_stack[4096]; // IST1: double faults survive kernel stack overflow
    uint8_t nmi_stack[4096];          // IST2
    uint8_t pf_stack[4096];           // IST3
};

static CoreSegmentArea g_core_seg[CONFIG_SMP_MAX_CPUS] = {};

extern "C" void load_gdt(struct gdt_descriptor *gdtr);
extern "C" void load_tss(void);

[[gnu::target("no-sse")]] static void fill_gdt_entries(gdt_entry *gdt, const tss_entry *tss)
{
    const uint64_t tss_base = reinterpret_cast<uint64_t>(tss);
    const uint64_t tss_limit = sizeof(tss_entry) - 1;

    // Null descriptor (0x00)
    gdt[0] = {0, 0, 0, 0, 0, 0};

    // Kernel Code (64-bit) - Selector 0x08
    gdt[1] = {.limit_low = 0xFFFF,
              .base_low = 0,
              .base_middle = 0,
              .access = 0x9A,      // Present, Ring0, Code, Readable
              .granularity = 0xAF, // 64-bit
              .base_high = 0};

    // Kernel Data (64-bit) - Selector 0x10
    gdt[2] = {.limit_low = 0xFFFF,
              .base_low = 0,
              .base_middle = 0,
              .access = 0x92, // Present, Ring0, Data, Writable
              .granularity = 0xCF,
              .base_high = 0};

    // User Data (64-bit) - Selector 0x18 | 3 = 0x1B
    gdt[3] = {.limit_low = 0xFFFF,
              .base_low = 0,
              .base_middle = 0,
              .access = 0xF2, // Present, Ring3, Data, Writable
              .granularity = 0xCF,
              .base_high = 0};

    // User Code (64-bit) - Selector 0x20 | 3 = 0x23
    gdt[4] = {.limit_low = 0xFFFF,
              .base_low = 0,
              .base_middle = 0,
              .access = 0xFA,      // Present, Ring3, Code, Readable
              .granularity = 0xAF, // 64-bit
              .base_high = 0};

    // TSS Descriptor - Selector 0x28
    gdt[5] = {.limit_low = static_cast<uint16_t>(tss_limit & 0xFFFF),
              .base_low = static_cast<uint16_t>(tss_base & 0xFFFF),
              .base_middle = static_cast<uint8_t>((tss_base >> 16) & 0xFF),
              .access = 0x89,
              .granularity = static_cast<uint8_t>(((tss_limit >> 16) & 0x0F)),
              .base_high = static_cast<uint8_t>((tss_base >> 24) & 0xFF)};

    auto *tss_high = reinterpret_cast<uint64_t *>(&gdt[6]);
    *tss_high = (tss_base >> 32) & 0xFFFFFFFF;
}

[[gnu::target("no-sse")]] void gdt_init_cpu(unsigned cpu_id)
{
    if (cpu_id >= CONFIG_SMP_MAX_CPUS)
        return;

    CoreSegmentArea &area = g_core_seg[cpu_id];
    tss_entry &tss = area.tss;

    tss.rsp0 = reinterpret_cast<uint64_t>(&area.rsp0_stack[sizeof(area.rsp0_stack)]);
    tss.iomap_base = sizeof(tss_entry);

    tss.ist1 = reinterpret_cast<uint64_t>(&area.double_fault_stack[sizeof(area.double_fault_stack)]);
    tss.ist2 = reinterpret_cast<uint64_t>(&area.nmi_stack[sizeof(area.nmi_stack)]);
    tss.ist3 = reinterpret_cast<uint64_t>(&area.pf_stack[sizeof(area.pf_stack)]);

    fill_gdt_entries(area.entries, &tss);

    gdt_descriptor gdtr = {
        .size = sizeof(area.entries) - 1,
        .offset = reinterpret_cast<uint64_t>(area.entries),
    };
    load_gdt(&gdtr);
    load_tss();
}

void gdt_init()
{
    gdt_init_cpu(0);
}

// Must run on the core whose rsp0 is being updated; scheduler context switches
// are always local to the current core.
void tss_set_rsp0(uint64_t rsp0)
{
    const PerCpu *cpu = cpu_get_local();
    if (!cpu || cpu->cpu_id >= CONFIG_SMP_MAX_CPUS)
        return;
    g_core_seg[cpu->cpu_id].tss.rsp0 = rsp0;
}
