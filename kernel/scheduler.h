#pragma once
#include <stdint.h>

void scheduler_init();
void scheduler_create_task(void (*entry)());
void scheduler_schedule();
void scheduler_yield();
uint64_t scheduler_get_pid();
