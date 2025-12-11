#pragma once
#include <stdint.h>

#define IDT_ENTRIES 256

struct idt_entry {
    uint16_t isr_low;   // The lower 16 bits of the ISR's address
    uint16_t kernel_cs; // The GDT segment selector that the CPU will load into CS before calling the ISR
    uint8_t  ist;       // The IST in the TSS that the CPU will load into RSP; set to zero for now
    uint8_t  attributes;// Type and attributes; see the IDT page
    uint16_t isr_mid;   // The higher 16 bits of the lower 32 bits of the ISR's address
    uint32_t isr_high;  // The higher 32 bits of the ISR's address
    uint32_t reserved;  // Set to zero
} __attribute__((packed));

struct idt_descriptor {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed));

// Interrupt frame pushed by CPU
struct interrupt_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

void idt_init();
