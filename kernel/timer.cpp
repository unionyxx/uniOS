#include "timer.h"
#include "pic.h"

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static volatile uint64_t ticks = 0;
static uint32_t tick_frequency = 0;

void timer_init(uint32_t frequency) {
    tick_frequency = frequency;
    
    // PIT runs at 1193182 Hz
    // divisor = 1193182 / desired_frequency
    uint32_t divisor = 1193182 / frequency;
    
    // Command: Channel 0, lobyte/hibyte, rate generator
    outb(PIT_COMMAND, 0x36);
    
    // Send divisor
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));
    
    // Unmask IRQ0 (timer)
    pic_clear_mask(0);
}

uint64_t timer_get_ticks() {
    return ticks;
}

void timer_handler() {
    ticks++;
}

void sleep(uint32_t ms) {
    uint64_t ticks_to_wait = (ms * tick_frequency) / 1000;
    uint64_t end_tick = ticks + ticks_to_wait;
    
    while (ticks < end_tick) {
        asm("hlt");
    }
}
