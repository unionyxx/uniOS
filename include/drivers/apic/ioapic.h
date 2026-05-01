#pragma once
#include <drivers/acpi/acpi.h>
#include <stdint.h>

struct AcpiMadtHeader
{
    AcpiSdtHeader header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed));

struct AcpiMadtRecord
{
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct AcpiMadtLapic
{
    AcpiMadtRecord header;
    uint8_t acpi_processor_id;
    uint8_t apic_id;
    uint32_t flags;
} __attribute__((packed));

struct AcpiMadtIoApic
{
    AcpiMadtRecord header;
    uint8_t io_apic_id;
    uint8_t reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed));

struct AcpiMadtIso
{
    AcpiMadtRecord header;
    uint8_t bus;
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct AcpiMadtLapicAddressOverride
{
    AcpiMadtRecord header;
    uint16_t reserved;
    uint64_t local_apic_address;
} __attribute__((packed));

struct IoApic
{
    uint64_t base;
    uint32_t gsi_base;
    uint32_t max_interrupts;
};

struct Iso
{
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
};

void ioapic_init();
bool ioapic_is_ready();
void ioapic_set_entry(uint8_t irq, uint8_t vector);
uint32_t ioapic_irq_to_gsi(uint8_t irq);
uint32_t apic_get_current_id();
void apic_send_eoi();
