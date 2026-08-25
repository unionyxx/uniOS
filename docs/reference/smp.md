# SMP

uniOS supports x86-64 symmetric multiprocessing with up to 8 cores (`CONFIG_SMP_MAX_CPUS`), a single global runqueue, and per-core execution state.

## AP Bring-Up

The BSP parses enabled processor-local APIC entries from the ACPI MADT and starts each AP individually with INIT followed by SIPI.

Trampoline: a 4 KiB page below 1 MiB (SIPI vector = page >> 12). The loader reserves `0x8F000` for this when possible; the kernel otherwise scans for a usable low page (floor `0x10000`, ceiling `0x90000`). If no page is available, or an AP fails its startup timeout, uniOS keeps running with the successfully started CPU set.

The trampoline page is position-independent:

| Offset | Contents |
| --- | --- |
| `0x000` | 16-bit entry: lgdt, PAE, CR3, EFER.LME+NXE, CR0.PG+PE, far jump to the 64-bit continuation |
| `0x080` | 64-bit continuation: loads parameters, jumps to the higher-half kernel entry **before** switching CR3 |
| `0x180` | Parameter block: trampoline PML4, stack top, kernel CR3, entry point, per-CPU pointer |
| `0x200` | Minimal GDT (64-bit code/data) |

The trampoline CR3 identity-maps the first 2 MiB with one large page and shares the kernel PML4's upper-half entries.

Startup sequence per AP: INIT IPI, 10 ms delay, INIT deassert, SIPI, bounded wait for the online flag, second SIPI if needed. The online timeout is 200 ticks; a straggler is re-parked with INIT+deassert (no further SIPI) so it cannot consume the next AP's parameter block. ICR sends use bounded delivery-status polling.

## Per-AP Setup

Each AP gets, in order:

1. Core setup: CR0/CR4/EFER, XSAVE, syscall MSRs (`STAR`/`LSTAR`/`SFMASK`), GS base pointing at its own `PerCpu`.
2. PAT programming (index 2 = write-combining) — mandatory per core, or `PTE_WC` mappings behave as UC.
3. Its own GDT + TSS with dedicated rsp0 and IST stacks (#DF, NMI, #PF), then the shared IDT.
4. LAPIC enable and LAPIC timer start from the BSP calibration multiplied by the AP divisor (APs tick at 100 Hz, one tenth of the BSP's 1 kHz clock, to keep idle cores off the scheduler lock).
5. Entry into its private idle task, which publishes the core online only after the LAPIC is enabled — so IPI senders that observe the online flag can actually reach the core.

Each AP also gets a separate bootstrap stack that is abandoned after handoff; it must not share storage with the idle task's stack.

## Scheduling

The runqueue and wait queues are protected by `g_sched_lock`. Each CPU has a private idle task that is never inserted into the global runqueue. A RESCHED IPI wakes idle CPUs after work becomes ready. New executable tasks are fully initialized before publication to the runqueue; this ordering is required for cross-core task pickup.

Address-space changes use the TLB-shootdown IPI protocol described in [Memory management](memory.md). The BSP is the source of the global scheduler clock; AP LAPIC timers run at 100 Hz and service local scheduling interrupts only (each accounting for 10 jiffies), never incrementing the global tick counter.

## Shutdown

Panic and reset paths send a lock-free STOP IPI to the other online CPUs (All-Excluding-Self broadcast on a dynamically allocated vector). The receiving CPU acknowledges once, disables interrupts, and halts — it never re-enters the scheduler or takes locks. Shutdown waits for acknowledgements after filesystem synchronization, with bounded polling so a faulty LAPIC cannot turn reset into an infinite spin.

## Validation

```sh
meson test -C build/debug --suite smoke        # single core
meson test -C build/debug --suite smoke-smp    # 2 cores
meson test -C build/debug --suite smoke-smp4   # 4 cores (also requires the 4-CPU scheduler marker)
meson compile -C build/debug smp-soak          # repeated 4-core boots, held briefly
```

The SMP suites require the debug ktest marker and the desktop-frame marker. Default run/smoke targets remain single-core; `run-smp`, `run-smp4`, and their serial/headless variants are explicit multi-core paths.

Real hardware validation should additionally cover non-contiguous LAPIC IDs, AP startup timeouts, x2APIC-disabled mode, USB/network IRQ load, reboot, poweroff, and panic while another CPU is in a syscall or page fault.
