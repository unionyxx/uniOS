# SMP

uniOS supports x86-64 symmetric multiprocessing with a single global runqueue
and per-core execution state.

## Boot

The BSP parses enabled processor-local APIC entries from the ACPI MADT. Each AP
is started individually with INIT followed by SIPI. The SIPI trampoline must
reside below 1 MiB, enters long mode using a temporary identity map, then jumps
to the higher-half kernel entry. APs receive private GS data, GDT/TSS/IST
state, kernel stacks, idle contexts, syscall MSRs, and LAPIC timer state.

If no usable low page is available, or an AP fails its startup timeout, uniOS
keeps running with the successfully started CPU set. The default QEMU targets
remain single-core; `run-smp`, `run-smp4`, `smoke-smp`, and `smoke-smp4` are
explicit multi-core validation paths.

## Scheduling

The runqueue and wait queues are protected by `g_sched_lock`. Each CPU has a
private idle task that is never inserted into the global runqueue. A RESCHED
IPI wakes idle CPUs after work becomes ready. New executable tasks are fully
initialized before publication to the runqueue; this ordering is required for
cross-core task pickup.

Address-space changes use the existing TLB-shootdown IPI protocol. The BSP is
the source of the global scheduler clock; AP LAPIC timers service local
interrupts but do not increment the global tick counter.

## Shutdown

Panic and reset paths send a lock-free STOP IPI to other online CPUs. The
receiving CPU acknowledges once, disables interrupts, and halts. Shutdown
waits for acknowledgements after filesystem synchronization, with bounded
IPI/status polling so a faulty LAPIC cannot turn reset into an infinite spin.

## Validation

```text
meson test -C build/debug --suite smoke
meson test -C build/debug --suite smoke-smp
meson test -C build/debug --suite smoke-smp4
meson compile -C build/debug smp-soak
```

The SMP suites require the debug ktest marker and the desktop-frame marker.
The soak target repeats four-CPU boots and holds each fully tested kernel
session briefly. The dedicated `smoke-smp4` suite additionally requires the
desktop-frame marker.
Real hardware validation should additionally cover LAPIC IDs that are not
contiguous, AP startup timeouts, x2APIC-disabled mode, USB/network IRQ load,
reboot, poweroff, and panic while another CPU is in a syscall or page fault.
