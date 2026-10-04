# Testing

Kernel tests use the `KTEST()` macro (`include/kernel/ktest.h`); system-level validation boots real images in QEMU and watches the serial log.

## ktest Framework

- `KTEST(name)` defines a test function and registers a `KTestCase{name, func}` in the `.ktests` linker section, bracketed by `__ktests_start`/`__ktests_end`.
- `ktest_run_all()` walks the section, runs each test, records failures with condition/file/line, and prints a pass/fail summary.
- Assertions: `KTEST_EXPECT` (returns from the test on failure) and `KTEST_EXPECT_EQ`.
- Tests run only in debug builds, after filesystem mounts and before SMP bring-up. Release builds boot straight to the desktop.
- Test files live next to the code they cover as `*_tests.cpp`: PMM/VMM/heap tests in `src/mm/tests/`, scheduler/SMP/syscall tests in `src/kernel/tests/`, congestion-control and DNS tests in `src/net/`.
- Test-side raw accesses to user-mapped pages (fixtures reading/writing through user VAs) ride `KSTAC()`/`KCLAC()` from `include/kernel/user_ptr.h`, so the suite keeps passing the day CR4.SMAP is turned on.

Notable coverage: zeroed-frame and double-free guards, HHDM round trips, heap realloc patterns and calloc overflow, ready-queue state guards, exactly-once enqueue across cores, per-CPU sanity, a threaded stress mix (heap churn, irqsave spinlocks, mutex handoff), fd-table fork-copy/thread-share/refcount, thread-group exit (including a sibling blocked in a futex wait), thread exit unmap + detach routing, timed futex waits (word-matched wakes, the 16-slot table's ENOSPC, tick round-up), static TLS (block/TCB install layout, per-thread template clone, exit unmap, fork carry-over, and PT_TLS parse including the kernel-half `p_vaddr` rejection), TCP congestion policy, and hostile DNS input.

## Smoke Suites

```sh
meson test -C build/debug --suite smoke --print-errorlogs
```

The harness (`tools/qemu_smoke.py`) boots `boot.img` headless with serial on stdio and enforces:

- Success markers: `first desktop frame submitted` (always) and `ktest suite passed` (debug builds).
- Failure markers: `ktest suite failed`, `KERNEL PANIC`.

Debug builds also run a userspace thread self-test: the deferred boot-services task `kernel_exec`s `/bin/threadtest.elf` (plain C, `crt0 + libc`, no GUI) after the net self-test spawn. It exercises create (with cross-thread fd visibility), mutex, condvar (timedwait timeout cycles), join, detach, and tls scenarios, exits nonzero when any scenario fails, and prints one serial summary line:

`thread self-test summary: create=PASS mutex=PASS cond=PASS join=PASS detach=PASS tls=PASS`

The tls scenario pins the per-thread contract end-to-end: four workers each run 5000 increments on their own `__thread` counter and return 5000 through the exit channel (a shared block would split the 20000 increments), the main thread's counter must stay zero, a nonzero-initialized `__thread` canary must read its initial value in every thread (a block shifted against the link-time TPOFF corrupts initialized variables), and the TCB at `fs:0` must self-identify and carry the `pthread_self` tid.

The suite's pass condition is not the full sentence: it greps the six field tokens — debug-gated success markers `create=PASS`, `mutex=PASS`, `cond=PASS`, `join=PASS`, `detach=PASS`, `tls=PASS` (one per scenario, like the ktest marker) plus failure markers on the `=FAIL` spellings of the same fields. The tokens are substrings matched anywhere in the serial log, and they appear only in the summary line (the app's per-scenario log lines use spaces, never `=`), so they need no line anchoring. In a release tree the markers do not exist, so the suite reduces to the desktop-frame marker. The app is also runnable from the shell.

SMP suites are opt-in and heavier:

```sh
meson test -C build/debug --suite smoke-smp     # 2 cores
meson test -C build/debug --suite smoke-smp4    # 4 cores; also requires "SMP scheduler ready on 4 CPUs"
meson compile -C build/debug smp-soak           # repeated 4-core boots, sessions held briefly
```

The network suite boots with slirp user networking and is a real E2E exercise of the stack:

```sh
meson test -C build/debug --suite smoke-net
```

`tools/smoke_net.py` starts a host-side `http.server` on `127.0.0.1:8931` (slirp maps the guest's `10.0.2.2` to host loopback), boots the debug image with an e1000 NIC, and asserts the debug net self-test summary line printed after `net_init()`: `arp=PASS` (slirp answers ARP for the gateway), `ping=PASS` (slirp answers ICMP echo), and `http=PASS` (the self-test downloads `/hello.txt` over TCP and checks the marker bytes). The DNS leg is informational — host-resolver dependent — and never gates. Net markers exist only in debug builds, so in a release tree the suite reduces to the desktop-frame marker.

Timeouts scale with the machine: Linux without KVM access runs under TCG with much larger budgets (CI grants the runner KVM access and falls back to TCG when `/dev/kvm` is unusable).

## What to Run When

- **Boot / kernel start / display / init changes**: must boot in QEMU (serial + graphical) and pass the smoke suite.
- **Storage / `/data` changes**: exercise a path that mounts the FAT32 `UNI_DATA` volume (the default `boot.img` run does this).
- **Network stack changes**: `--suite smoke-net` (DHCP, ARP, ICMP, TCP download E2E).
- **Scheduler / SMP changes**: the SMP suites, plus `smp-soak` for scheduling work.
- **Thread / pthread changes**: the plain smoke suite — the debug threadtest markers assert the pthread surface end-to-end.
- **Anything touching docs build**: `meson compile -C build/debug wiki` (strict link checking fails on broken references).

## CI

`.github/workflows/ci.yml` builds debug and release, runs the smoke suite and the smoke-net suite on the debug image, then lint (cppcheck) and a format check (`git diff --exit-code` after `format`). Lint and format are continue-on-error; keep the tree format-clean locally.
