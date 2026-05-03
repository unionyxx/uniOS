#include <kernel/arch/x86_64/idt.h>

__attribute__((aligned(0x10))) static struct idt_entry idt[IDT_ENTRIES];
static struct idt_descriptor idtr;
static bool g_vector_used[IDT_ENTRIES] = {false};

extern "C" void *isr_stub_table[];
extern "C" void *irq_stub_table[];

void idt_set_descriptor(uint8_t vector, const void *isr, uint8_t flags)
{
    struct idt_entry *descriptor = &idt[vector];

    descriptor->isr_low = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs = 0x08; // Kernel code segment offset
    descriptor->ist = 0;
    descriptor->attributes = flags;
    descriptor->isr_mid = (uint16_t)(((uint64_t)isr >> 16) & 0xFFFF);
    descriptor->isr_high = (uint32_t)(((uint64_t)isr >> 32) & 0xFFFFFFFF);
    descriptor->reserved = 0;
}

// Set IDT descriptor with IST (Interrupt Stack Table) entry
// IST 1-7 selects a stack from TSS; 0 means use current stack
void idt_set_descriptor_with_ist(uint8_t vector, const void *isr, uint8_t flags, uint8_t ist)
{
    struct idt_entry *descriptor = &idt[vector];

    descriptor->isr_low = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs = 0x08;
    descriptor->ist = ist & 0x7; // IST is 3 bits (0-7)
    descriptor->attributes = flags;
    descriptor->isr_mid = (uint16_t)(((uint64_t)isr >> 16) & 0xFFFF);
    descriptor->isr_high = (uint32_t)(((uint64_t)isr >> 32) & 0xFFFFFFFF);
    descriptor->reserved = 0;
}

extern "C" void load_idt(struct idt_descriptor *idtr);
extern "C" void isr128();

void idt_init()
{
    idtr.size = sizeof(idt) - 1;
    idtr.offset = (uint64_t)&idt;

    // CPU exceptions (0-31)
    for (uint8_t vector = 0; vector < 32; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
    }

    // Override exceptions to use ISTs
    idt_set_descriptor_with_ist(2, isr_stub_table[2], 0x8E, 2);   // NMI
    idt_set_descriptor_with_ist(8, isr_stub_table[8], 0x8E, 1);   // Double Fault
    idt_set_descriptor_with_ist(14, isr_stub_table[14], 0x8E, 3); // Page Fault

    // Hardware interrupt vectors (32-255, excluding userspace syscall 0x80)
    for (uint16_t vector = 32; vector < 256; vector++) {
        if (vector == 0x80)
            continue;
        idt_set_descriptor(static_cast<uint8_t>(vector), irq_stub_table[vector - 32], 0x8E);
    }

    // Syscall (int 0x80) - Ring 3 callable
    idt_set_descriptor(0x80, reinterpret_cast<void *>(isr128), 0xEE); // 0xEE = Present, Ring3, Interrupt
    g_vector_used[0x80] = true;

    load_idt(&idtr);
}

uint8_t idt_allocate_free_vector()
{
    // Start searching from 48, past exceptions and ISA IRQs.
    for (int i = 48; i < 255; i++) {
        if (!g_vector_used[i]) {
            g_vector_used[i] = true;
            return static_cast<uint8_t>(i);
        }
    }
    return 0;
}
