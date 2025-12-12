#include "idt.h"

__attribute__((aligned(0x10)))
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_descriptor idtr;

extern "C" void* isr_stub_table[];
extern "C" void* irq_stub_table[];

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    struct idt_entry* descriptor = &idt[vector];

    descriptor->isr_low    = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs  = 0x08; // Kernel code segment offset
    descriptor->ist        = 0;
    descriptor->attributes = flags;
    descriptor->isr_mid    = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high   = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved   = 0;
}

extern "C" void load_idt(struct idt_descriptor* idtr);
extern "C" void isr128();

void idt_init() {
    idtr.size = sizeof(idt) - 1;
    idtr.offset = (uint64_t)&idt;

    // CPU exceptions (0-31)
    for (uint8_t vector = 0; vector < 32; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
    }

    // IRQs (32-47)
    for (uint8_t vector = 0; vector < 16; vector++) {
        idt_set_descriptor(vector + 32, irq_stub_table[vector], 0x8E);
    }

    // Syscall (int 0x80) - Ring 3 callable
    idt_set_descriptor(0x80, (void*)isr128, 0xEE); // 0xEE = Present, Ring3, Interrupt

    load_idt(&idtr);
}
