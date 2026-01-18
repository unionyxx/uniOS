#include <kernel/arch/x86_64/gdt.h>

__attribute__((aligned(0x1000)))
static struct gdt_entry gdt[7]; // Null, Kernel Code, Kernel Data, User Code, User Data, TSS (Low), TSS (High)
static struct gdt_descriptor gdtr;

__attribute__((aligned(16)))
static struct tss_entry tss;

// Stack for the TSS (Privilege level 0 stack)
__attribute__((aligned(16)))
static uint8_t tss_stack[4096];

// Dedicated stack for Double Fault handler (IST1)
// This ensures the CPU can handle double faults even if the kernel stack overflows
__attribute__((aligned(16)))
static uint8_t double_fault_stack[4096];

extern "C" void load_gdt(struct gdt_descriptor* gdtr);
extern "C" void load_tss(void);

void gdt_init() {
    // Zero the TSS first
    for (unsigned i = 0; i < sizeof(tss); i++) {
        ((uint8_t*)&tss)[i] = 0;
    }
    
    // Setup TSS
    tss.rsp0 = (uint64_t)&tss_stack[sizeof(tss_stack)];
    tss.iomap_base = sizeof(tss);
    
    // Setup IST1 for Double Fault handler (vector 8)
    // This provides a known-good stack even if the kernel stack is corrupted
    tss.ist1 = (uint64_t)&double_fault_stack[sizeof(double_fault_stack)];

    // Null descriptor (0x00)
    gdt[0] = {0, 0, 0, 0, 0, 0};

    // Kernel Code (64-bit) - Selector 0x08
    gdt[1] = {
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = 0x9A,      // Present, Ring0, Code, Readable
        .granularity = 0xAF, // 64-bit
        .base_high = 0
    };

    // Kernel Data (64-bit) - Selector 0x10
    gdt[2] = {
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = 0x92,      // Present, Ring0, Data, Writable
        .granularity = 0xCF,
        .base_high = 0
    };

    // User Code (64-bit) - Selector 0x18 | 3 = 0x1B
    gdt[3] = {
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = 0xFA,      // Present, Ring3, Code, Readable
        .granularity = 0xAF, // 64-bit
        .base_high = 0
    };

    // User Data (64-bit) - Selector 0x20 | 3 = 0x23
    gdt[4] = {
        .limit_low = 0xFFFF,
        .base_low = 0,
        .base_middle = 0,
        .access = 0xF2,      // Present, Ring3, Data, Writable
        .granularity = 0xCF,
        .base_high = 0
    };

    // TSS Descriptor - Selector 0x28
    uint64_t tss_base = (uint64_t)&tss;
    uint64_t tss_limit = sizeof(tss) - 1;

    gdt[5] = {
        .limit_low = (uint16_t)(tss_limit & 0xFFFF),
        .base_low = (uint16_t)(tss_base & 0xFFFF),
        .base_middle = (uint8_t)((tss_base >> 16) & 0xFF),
        .access = 0x89,
        .granularity = (uint8_t)(((tss_limit >> 16) & 0x0F)),
        .base_high = (uint8_t)((tss_base >> 24) & 0xFF)
    };

    uint64_t* tss_high = (uint64_t*)&gdt[6];
    *tss_high = (tss_base >> 32) & 0xFFFFFFFF;

    gdtr.size = sizeof(gdt) - 1;
    gdtr.offset = (uint64_t)&gdt;

    load_gdt(&gdtr);
    load_tss();
}

// Update TSS rsp0 for context switching
// Must be called before switching to a new task to ensure Ring 3 -> Ring 0
// transitions use the correct kernel stack
void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}