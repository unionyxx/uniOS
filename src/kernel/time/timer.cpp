#include <kernel/time/timer.h>
#include <kernel/arch/x86_64/pic.h>
#include <kernel/arch/x86_64/io.h>
#include <kernel/scheduler.h>
#include <drivers/video/framebuffer.h>

static volatile uint64_t ticks = 0;
static uint32_t tick_frequency = 0;

// Heartbeat state for visual system health indicator
static uint64_t last_heartbeat_tick = 0;
static bool heartbeat_on = false;

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

uint32_t timer_get_frequency() {
    return tick_frequency;
}

void timer_handler() {
    ticks++;
    
    // Heartbeat: toggle pixel every 500ms (tick_frequency/2 ticks at 1000Hz)
    // This provides visual confirmation that interrupts are still working
    // SAFETY: Only start after 3 seconds to ensure graphics is fully initialized
    if (tick_frequency > 0 && ticks > 3000 && 
        (ticks - last_heartbeat_tick) >= tick_frequency / 2) {
        last_heartbeat_tick = ticks;
        heartbeat_on = !heartbeat_on;
        
        // Draw directly to buffer - safer than calling gfx_put_pixel in IRQ context
        uint32_t* buf = gfx_get_buffer();
        uint64_t w = gfx_get_width();
        if (buf && w > 10) {
            // Small 4x4 square for visibility in top-right corner
            uint32_t color = heartbeat_on ? 0x00FF00 : 0x002200;  // Bright/dim green
            // Approximate pitch as width (works for most framebuffers)
            for (int y = 4; y < 8; y++) {
                for (int x = 0; x < 4; x++) {
                    buf[y * w + (w - 8 + x)] = color;
                }
            }
        }
    }
}

void sleep(uint32_t ms) {
    uint64_t ticks_to_wait = (ms * tick_frequency) / 1000;
    uint64_t end_tick = ticks + ticks_to_wait;
    
    while (ticks < end_tick) {
        scheduler_yield();  // Yield CPU to other tasks instead of just halting
    }
}
