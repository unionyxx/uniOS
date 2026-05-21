#pragma once

#include <stdint.h>

typedef void (*IrqVectorHandler)(uint8_t vector, void *ctx);

bool irq_register_vector_handler(uint8_t vector, IrqVectorHandler handler, void *ctx);
void irq_unregister_vector_handler(uint8_t vector);
bool irq_register_isa_handler(uint8_t irq, IrqVectorHandler handler, void *ctx);
void irq_unregister_isa_handler(uint8_t irq);
uint8_t irq_isa_to_vector(uint8_t irq);

void apic_init();
bool apic_is_enabled();
uint32_t apic_get_current_id();
void apic_send_eoi();
void apic_send_ipi_all_excluding_self(uint8_t vector);
