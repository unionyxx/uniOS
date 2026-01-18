#pragma once
#include <stdint.h>

struct Process;  // Forward declaration for getter

void scheduler_init();
void scheduler_create_task(void (*entry)(), const char* name);
void scheduler_schedule();
void scheduler_yield();

// Get process list head for inspection (e.g., ps command)
Process* scheduler_get_process_list();

// Sleep for a number of timer ticks (blocks the current process)
void scheduler_sleep(uint64_t ticks);

// Sleep for milliseconds (convenience wrapper)
void scheduler_sleep_ms(uint64_t ms);
