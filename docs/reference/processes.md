# Processes

Userspace programs are ELF binaries loaded from the VFS. Process state lives in `struct Process` (`include/kernel/process.h`), whose first fields have assembly-fixed offsets (static-asserted against the context-switch and usermode assembly).

## Process Model

- Kernel stack: 64 KiB of contiguous frames; the first eight qwords are `0xDEADBEEFDEADBEEF` canaries checked on every schedule.
- FPU state: a 4 KiB xsave area embedded in the struct.
- Per-process: a refcounted `FdTable` (128 slots, shared live by threads, deep-copied by fork), VMA list guarded by a VMA lock, cwd (256 bytes), signal state, 128-slot event queue, children/sibling/global linkage.
- States: Ready, Running, Blocked, Sleeping, Zombie, Waiting.
- Threads (`SYS_THREAD_CREATE`) are processes with their own pids; see [Threads](#threads).

## Fork

`SYS_FORK` clones the current process:

1. FPU state and fd table are copied (vnode refcounts bumped under the fd lock).
2. The address space is cloned under the parent's VMA lock: the kernel half is shared, the user half is deep-copied with frame refcount bumps, and writable non-shared pages are write-protected in **both** parent and child (copy-on-write). Shared pages keep their write permission.
3. A full-range TLB shootdown publishes the CoW downgrade.
4. VMAs are cloned, the child gets a fresh kernel stack seeded with a `SyscallFrame` that returns 0 in `rax`.

The child's pid is captured before it is published to the scheduler — a fast exit on another core could otherwise free the struct before fork returns.

## Exec

`SYS_EXEC` replaces the process image:

- A threaded process tears its thread group down first, mirroring exit-group: every live member is SIGKILLed through the locked signal path, the deaths are awaited (bounded at 1 s, yielding so members run to their deaths), and the joinable zombies are reaped so the deferred-free paths own their stacks and structs. A member that wedges past the deadline makes the exec fail — the address space must stay untouched while a sibling still runs on it.
- The ELF is validated (`\x7FELF`, ELF64, little-endian, `ET_EXEC`/`ET_DYN`, `EM_X86_64`, sane program headers). Entry must be covered by a segment, and overlapping `PT_LOAD` segments are rejected (they could merge writable and executable ranges).
- Segments map with `USER` plus `WRITABLE` when `PF_W` and `NX` unless `PF_X`; the 32 KiB user stack maps below `0x0000700000000000`. Each page's BSS tail (`[filesz, memsz)`) is zeroed with the range clamped to the page — the lower bound is `max(page start, vaddr+filesz)`, because on pages deep in a multi-page BSS the file end lies below the page and an unclamped subtraction would wrap the zero-fill into unrelated physical memory below the segment.
- The old address space and VMAs are freed only after the new ones are installed; CR3 is switched on return to user.

There is no argv/envp or aux vector: `crt0` calls `main()` with no arguments.

## Threads

`sys_thread_create` (`src/kernel/sched/scheduler.cpp`) allocates a `Process` that shares the creator's address space — page table, VMA list, and VMA lock (the leader's embedded lock through `vma_lock_ptr`) — and the fd table **live** (`fd_table_share` bumps the table's refcount; open/close in one thread is immediately visible to siblings, while fork deep-copies). It gets a private kernel stack, its own pid, and `leader_pid` = the creator's leader pid; plain processes lead their own group.

- The entry frame is cloned from the caller's live syscall frame, so the thread starts with the caller's user segments and rflags; `arg` reaches the entry in `rdi` through the `thread_ret` trampoline.
- `SYS_GETPID` in a thread returns the thread's own pid. A joinable thread is a child of its **creator** only: `SYS_WAIT4` reaps it from the creating thread, not from the group.
- Detached threads (the `THREAD_DETACHED` create flag or `SYS_THREAD_DETACH`) are orphaned to pid 0 — the kernel-zombie reaper collects them with no waitpid, and a later waitpid on the tid fails. `SYS_THREAD_DETACH` refuses forked children (`-10` ECHILD): they lead their own group, and orphaning them would discard their exit status.
- The fd table outlives any single thread: each exit drops one reference (`process_release_private_fds`), and the table — with its vnodes — is freed only at the last release.

## Exit and Wait

`process_exit(status)` — the `SYS_EXIT` and fatal-signal path — first kills the thread group: every other live process with the same `leader_pid` is sent SIGKILL under the scheduler lock (blocked waiters are woken; zombies are skipped). Then:

1. Releases private fds (detached under the fd lock, vnode refs dropped outside it) and shared-memory references.
2. Under the scheduler lock: becomes a Zombie with an exit status; orphans are reparented to pid 1 (or to the pid-0 kernel task when init is absent); waiters are woken.
3. Switches to the kernel address space **before** scheduling, so the exiting core never runs on the dying task's mappings.

`sys_thread_exit(status)` (`SYS_THREAD_EXIT`) runs the same common path minus the group kill, after unmapping the thread's recorded stack range strictly while on the kernel stack (a refused unmap leaks the mapping and logs at error level). The exit status travels through the `int32_t` channel — fatal-signal deaths carry `-signum`, and `pthread_join` collects only values that fit.

`SYS_WAIT4` supports specific pids, any-child (`-1`), and `WNOHANG`. Reaping frees the kernel stack, address space, and VMAs — except when the page table, VMA list, or VMA lock is still shared with live threads; such zombies are parked on a deferred-free list and retried on every reap pass. A waitpid that finds the child in its children list but not in the global process list refuses the reap (the entry was already detached) instead of freeing a possibly deferred or dangling struct. Kernel-parented zombies are reaped automatically.

## Signals

Defined signals: SIGHUP 1, SIGINT 2, SIGQUIT 3, SIGILL 4, SIGTRAP 5, SIGABRT 6, SIGBUS 7, SIGFPE 8, SIGKILL 9, SIGUSR1 10, SIGSEGV 11, SIGUSR2 12, SIGPIPE 13, SIGALRM 14, SIGTERM 15. `SIG_DFL = 0`, `SIG_IGN = 1`.

Delivery:

- `SYS_KILL` finds the target under the scheduler lock (lookup-while-reap safe) and checks uid (root, self, or same uid). Signal 0 is an existence check.
- A pending bit is OR'd into the target's signal mask and blocking states are interrupted.
- Signals are checked on every return to ring 3 — after syscalls and after interrupts that interrupted user mode.
- Default-fatal signals (SIGINT/SIGTERM/SIGQUIT/SIGKILL/SIGSEGV) exit the whole thread group with a negative signal-number status.
- Custom handlers run on the user stack with a saved context (interrupt frame + FPU state + magic). `SYS_SIGRETURN` validates the magic, forces user CS/SS, sanitizes RFLAGS, restores FPU state with masked MXCSR/xstate bits, and returns via `iretq`.
- `SYS_SIGACTION` rejects SIGKILL and non-canonical handler/restorer addresses. libc installs the `__sigret` trampoline as the restorer.
- Blocking kernel paths (stdin, pipes, futex waits, event waits) return `-EINTR` when a default-fatal signal is pending.

## UID Model

`SYS_GETUID`/`SYS_SETUID` manage a per-process uid (root can set). The kernel parses `/etc/passwd` and `/etc/shadow` (`src/kernel/core/kuser.cpp`) and persists them to `/data/etc/` when they change. VFS permission checks are uid-only (owner bits, else other bits; uid 0 bypasses).
