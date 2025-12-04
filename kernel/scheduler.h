#pragma once

void scheduler_init();
void scheduler_create_task(void (*entry)());
void scheduler_schedule();
void scheduler_yield();
