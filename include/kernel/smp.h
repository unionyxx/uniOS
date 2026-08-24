#pragma once
#include <stdint.h>

// SMP bring-up. smp_init() enumerates APs from the MADT, starts them via
// INIT-SIPI with the low-memory trampoline, and parks each in its private
// idle loop until the scheduler is taught to use them (resched IPIs).
//
// Everything degrades gracefully: if there is no usable low page for the
// trampoline, or the MADT lists no APs, or an AP fails to come online within
// its timeout, uniOS keeps booting single-core.

// Called once from kmain after scheduler/ktest init, before userspace starts.
void smp_init();

// Number of APs seen in MADT (may exceed started count if bring-up failed).
uint32_t smp_ap_count();
