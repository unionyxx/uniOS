# Testing

Kernel tests use the `KTEST()` macro (`include/kernel/ktest.h`); system-level validation boots real images in QEMU and watches the serial log.

## ktest Framework

- `KTEST(name)` defines a test function and registers a `KTestCase{name, func}` in the `.ktests` linker section, bracketed by `__ktests_start`/`__ktests_end`.
- `ktest_run_all()` walks the section, runs each test, records failures with condition/file/line, and prints a pass/fail summary.
- Assertions: `KTEST_EXPECT` (returns from the test on failure) and `KTEST_EXPECT_EQ`.
- Tests run only in debug builds, after filesystem mounts and before SMP bring-up. Release builds boot straight to the desktop.
- The suite runs with interrupts enabled (BSP-local, before SMP): timed waits need real timer ticks, and the tick counter only advances from the timer IRQ — parked waiters would hang without it. `kmain` restores IF=0 before SMP bring-up, which expects to enter that way.
- Test files live next to the code they cover as `*_tests.cpp`: PMM/VMM/heap tests in `src/mm/tests/`, scheduler/SMP/syscall tests in `src/kernel/tests/`, congestion-control and DNS tests in `src/net/`.
- Test-side raw accesses to user-mapped pages (fixtures reading/writing through user VAs) ride `KSTAC()`/`KCLAC()` from `include/kernel/user_ptr.h`, so the suite keeps passing the day CR4.SMAP is turned on.

Notable coverage: zeroed-frame and double-free guards, HHDM round trips, heap realloc patterns and calloc overflow, ready-queue state guards, exactly-once enqueue across cores, per-CPU sanity, a threaded stress mix (heap churn, irqsave spinlocks, mutex handoff), fd-table fork-copy/thread-share/refcount, thread-group exit (including a sibling blocked in a futex wait), thread exit unmap + detach routing, timed futex waits (word-matched wakes, registry scaling past the old fixed-table capacity, tick round-up, arm-before-queue races), timed-wait clear semantics, aligned-alloc metadata round-trips and stale-header rejection, PMM zone/DMA32 bounds and exhaustion, mmap OOM rollback ordering with fault injection, timekeeping monotonicity/clamp/tier reporting, kernel-stack guard-page mapping and recycling, timekeeping tier reporting, static TLS (block/TCB install layout, per-thread template clone, exit unmap, fork carry-over, and PT_TLS parse including the kernel-half `p_vaddr` rejection), TCP congestion policy, and hostile DNS input.

## Smoke Suites

```sh
meson test -C build/debug --suite smoke --print-errorlogs
```

The harness (`tools/qemu_smoke.py`) boots `boot.img` headless with serial on stdio and enforces:

- Success markers: `first desktop frame submitted` (always), `ktest suite passed` and `task teardown audit: PASS` (debug builds).
- Failure markers: `ktest suite failed`, `KERNEL PANIC`, and `task teardown audit: FAIL` (debug builds).

Debug builds also run a userspace thread self-test: the deferred boot-services task `kernel_exec`s `/bin/threadtest.elf` (plain C, `crt0 + libc`, no GUI) after the net self-test spawn. It exercises create (with cross-thread fd visibility), mutex, condvar (timedwait timeout cycles), join, detach, tls, and wavprobe scenarios, exits nonzero when any scenario fails, and prints one serial summary line:

`thread self-test summary: create=PASS mutex=PASS cond=PASS join=PASS detach=PASS tls=PASS wavprobe=PASS`

The tls scenario pins the per-thread contract end-to-end: four workers each run 5000 increments on their own `__thread` counter and return 5000 through the exit channel (a shared block would split the 20000 increments), the main thread's counter must stay zero, a nonzero-initialized `__thread` canary must read its initial value in every thread (a block shifted against the link-time TPOFF corrupts initialized variables), and the TCB at `fs:0` must self-identify and carry the `pthread_self` tid.

Debug boots also leak-check the idle loop: after each polled stage (1=input, 2=net, 3=sound, 4=scheduler yield) a probe verifies interrupts are still enabled, prints `idle: interrupts disabled by poll stage N (leak)` once per offending stage, and re-enables before the `hlt` so the boot stays alive with the leak visible in the serial log. The marker is a smoke failure token: a leaked irqsave on any poll path fails the suite loudly instead of hanging it. This probe localized the `tcp_receive()` irq-pairing leak behind issue #24.

Debug boots also run a kernel task teardown audit: the `TeardownAudit` task (created after `DeferredInit`/`InitLaunch`) waits for every kernel-mode task boot created to exit **and** be reaped, and for the deferred-free list to hold no kernel-mode zombie — the leak class where a reaped kernel task's null page table once matched every other kernel task's null and its struct plus kernel stack parked forever. Each of its `scheduler_yield()` calls pumps a reap pass, so waiting also drives the deferred retries. It prints `task teardown audit: PASS` with the deferred and free-memory accounting once settled, or `task teardown audit: FAIL` after 60 s without progress (a slow-but-alive task keeps the survivor set changing; only a wedged teardown is stuck long enough to trip it).

The wavprobe scenario packs WAV byte streams with explicit little-endian helpers (no struct casts) and checks `media_audio_probe` against them: a valid stereo stream's fields, truncated buffers, corrupt magic, non-PCM format codes, 8-bit samples, payloads running past `file_size`, and a header window smaller than the file (parsing reads stay in the window while the bounds check uses the file size). The output structure must be untouched on every rejection.

The suite's pass condition is not the full sentence: it greps the seven field tokens — debug-gated success markers `create=PASS`, `mutex=PASS`, `cond=PASS`, `join=PASS`, `detach=PASS`, `tls=PASS`, `wavprobe=PASS` (one per scenario, like the ktest marker) plus failure markers on the `=FAIL` spellings of the same fields. The tokens are substrings matched anywhere in the serial log, and they appear only in the summary line (the app's per-scenario log lines use spaces, never `=`), so they need no line anchoring. In a release tree the markers do not exist, so the suite reduces to the desktop-frame marker. The app is also runnable from the shell.

SMP suites are opt-in and heavier:

```sh
meson test -C build/debug --suite smoke-smp     # 2 cores
meson test -C build/debug --suite smoke-smp4    # 4 cores; also requires "SMP scheduler ready on 4 CPUs"
meson compile -C build/debug smp-soak           # repeated 4-core boots, sessions held briefly
```

The audio suite boots with the silent AC97 card attached so the ktest sound branch actually runs:

```sh
meson test -C build/debug --suite smoke-audio
```

The ktest `media_sound_stream_api` card-present branch (stream open/write/status, pause-latch during the pre-fill window, stop) prints `soundstream ktest: PASS`, which gates this suite; the plain smoke suite boots without a sound card, so the branch is skipped there and the marker is absent. In a release tree the suite reduces to the plain boot markers.

The network suite boots with slirp user networking and is a real E2E exercise of the stack:

```sh
meson test -C build/debug --suite smoke-net
```

`tools/smoke_net.py` starts a host-side `http.server` on `127.0.0.1:8931` (slirp maps the guest's `10.0.2.2` to host loopback), boots the debug image with an e1000 NIC, and asserts the debug net self-test summary line printed after `net_init()`: `arp=PASS` (slirp answers ARP for the gateway), `ping=PASS` (slirp answers ICMP echo), and `http=PASS` (the self-test downloads `/hello.txt` over TCP and checks the marker bytes). The DNS leg is informational — host-resolver dependent — and never gates. Net markers exist only in debug builds, so in a release tree the suite reduces to the desktop-frame marker.

Timeouts scale with the machine: Linux without KVM access runs under TCG with much larger budgets (CI grants the runner KVM access and falls back to TCG when `/dev/kvm` is unusable). Test boots pass QEMU `-snapshot`, so every suite's writes land in a throwaway overlay: suites can run concurrently in one `meson test` invocation without fighting over the boot image's write lock, and no test boot leaks state into `boot.img`.

## What to Run When

- **Boot / kernel start / display / init changes**: must boot in QEMU (serial + graphical) and pass the smoke suite.
- **Storage / `/data` changes**: exercise a path that mounts the FAT32 `UNI_DATA` volume (the default `boot.img` run does this).
- **Network stack changes**: `--suite smoke-net` (DHCP, ARP, ICMP, TCP download E2E).
- **Sound / streaming playback changes**: `--suite smoke-audio` (ktest stream lifecycle incl. the pause latch, on the AC97 card).
- **Scheduler / SMP changes**: the SMP suites, plus `smp-soak` for scheduling work.
- **Thread / pthread changes**: the plain smoke suite — the debug threadtest markers assert the pthread surface end-to-end.
- **Anything touching docs build**: `meson compile -C build/debug wiki` (strict link checking fails on broken references).

## CI

`.github/workflows/ci.yml` builds debug and release, runs the smoke suite and the smoke-net suite on the debug image, then lint (cppcheck) and a format check (`git diff --exit-code` after `format`). Lint and format are continue-on-error; keep the tree format-clean locally.
