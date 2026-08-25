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
bool apic_send_ipi_to(uint8_t dest_apic_id, uint8_t vector);
bool apic_send_init_ipi(uint8_t dest_apic_id);
bool apic_send_init_deassert_ipi(uint8_t dest_apic_id);
bool apic_send_sipi(uint8_t dest_apic_id, uint8_t vector);
// Broadcasts the RESCHED vector to all other online cores.
void apic_send_resched_ipi_to_others();
void apic_stop_other_cpus();

// LAPIC timer: the BSP calibrates once (shared PIT channel 2); every core
// then programs itself with the BSP's count.
void apic_timer_start_this_core(uint32_t initcnt);
[[nodiscard]] uint32_t apic_timer_bsp_initcnt();
void apic_enable_this_core();
