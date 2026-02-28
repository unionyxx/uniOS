#pragma once
#include <stdint.h>

struct Process;

void scheduler_init();
void scheduler_create_task(void (*entry)(), const char* name);
void scheduler_schedule();
void scheduler_yield();

[[nodiscard]] Process* scheduler_get_process_list();

void scheduler_sleep(uint64_t ticks);
void scheduler_sleep_ms(uint64_t ms);
