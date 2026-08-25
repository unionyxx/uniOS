# Scheduling

The scheduler (`src/kernel/sched/scheduler.cpp`) is a preemptive, priority-based scheduler with a single global runqueue model and per-core execution state.

## Runqueues and Priorities

- Three priority levels (0 = highest). Ready queues are singly-linked FIFOs through `Process::queue_next`; pops scan priority 0 first.
- Time slices: priority 0 gets 5 ticks, priority 1 gets 20, priority 2 gets 50 (tick = 1 ms at the 1000 Hz timer).
- A task that exhausts its slice is demoted one level (max 2) and its slice resets. Waking from sleep boosts priority one level.
- Interactive boost: the window manager and the focused window's process are held at priority 0.
- Big lock: `g_sched_lock` guards runqueues, the sleep queue, wait queues, and the process list. It is always taken with interrupts disabled. Leaf locks (PMM, heap, VMA, fd, pipe, futex, epoll) may nest under it but may only re-enter the scheduler through `scheduler_wait(queue, &leaf)`, which releases the leaf lock before switching. After a context switch, the scheduler lock is released on the target task's behalf.

## Preemption and Ticks

The timer interrupt fires on vector 32 at 1000 Hz (see Timers below). The handler calls `timer_handler()`, sends EOI, then either records a pending preemption (`preempt_count > 0`) or calls `scheduler_schedule()` directly. `preempt_disable/enable` bracket critical kernel sections; SMP bring-up uses them to wait with ticks advancing but no scheduling.

Every `scheduler_schedule()` checks the current task's kernel stack canaries (the first eight qwords must remain `0xDEADBEEFDEADBEEF`) and panics on overflow.

## Task Creation

`scheduler_create_task` allocates a 64-byte-aligned `Process`, a 16-page kernel stack from contiguous PMM frames, canary words, and an initial context. Creation and publication are split (`scheduler_create_task_deferred` + `scheduler_enqueue_task`) so a task is fully initialized before any core can pick it up — an ordering requirement for cross-core pickup. Boot uses this for the `DeferredInit` and `InitLaunch` tasks.

Each core has a private idle task (pid 0) that is never inserted into the runqueue. When nothing is runnable, the core parks in `sti; hlt`. A RESCHED IPI (`scheduler_notify_idle_cpus`) wakes idle cores whenever work becomes ready; it is sent on every enqueue, wake, and input notification.

## Sleep and Wait Queues

- The sleep queue is delta-encoded: each entry stores ticks relative to the previous entry, and each tick consumes elapsed time from the head.
- Wait queues are FIFOs. `scheduler_wait(queue, lock)` pushes the task, releases the given lock, schedules, and re-acquires the lock on return. `scheduler_wake_all` additionally nudges the global epoll wait queue; `scheduler_wake_one` wakes exactly the head waiter (wake-one + resleep-on-miss).
- Blocking paths (stdin reads, pipes, futex, event waits) return `-EINTR` when a fatal signal becomes pending.

## Context Switch

The switch path compares the next task's page table against the CR3 actually loaded on this core (`PerCpu.current_cr3_phys`) — not against the previous task's — because `process_exit()` switches to kernel page tables before scheduling while a runnable sibling thread may share the dying task's page table. It then updates the TSS rsp0 and calls the assembly switch (`src/arch/x86_64/boot/process.asm`), which saves callee-saved registers, the kernel stack pointer, and the FPU state (4 KiB xsave area at a fixed offset in `Process`).

## Timers and Time

- Primary tick: LAPIC timer in periodic mode at 1000 Hz on every core, calibrated once on the BSP against a PIT channel 2 one-shot 10 ms window (divide-by-16). APs reuse the BSP's calibrated count.
- Only the BSP increments the global tick counter; AP LAPIC timers service local interrupts only. The BSP is therefore the source of the scheduler clock.
- Fallback: without an APIC, PIT channel 0 at the requested rate via PIC IRQ0.
- `timer_poll_wait_ms` busy-waits using PIT channel 2 one-shots in <= 50 ms chunks (used by pid-0 contexts and SMP bring-up delays).
- Uptime syscalls derive from ticks; wall-clock time comes from the CMOS RTC (BCD/binary and 12/24-hour handling, double-read reconciliation, fallback date when no RTC is detected). See `src/drivers/rtc/rtc.cpp`.

## Shutdown

`system_reboot` / `system_poweroff` serialize on a single initiator, mark user processes as zombies, sync the filesystems, stop the other CPUs with the STOP IPI (see [SMP](smp.md)), then:

- Reboot: `wbinvd`, ACPI reset register, keyboard controller `0x64/0xFE`, PCI reset `0xCF9`, and finally a triple fault.
- Poweroff: ACPI PM1a/PM1b sleep with the parsed `_S5_` values, then a halt loop.
