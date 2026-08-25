# Testing

Kernel tests use the `KTEST()` macro (`include/kernel/ktest.h`); system-level validation boots real images in QEMU and watches the serial log.

## ktest Framework

- `KTEST(name)` defines a test function and registers a `KTestCase{name, func}` in the `.ktests` linker section, bracketed by `__ktests_start`/`__ktests_end`.
- `ktest_run_all()` walks the section, runs each test, records failures with condition/file/line, and prints a pass/fail summary.
- Assertions: `KTEST_EXPECT` (returns from the test on failure) and `KTEST_EXPECT_EQ`.
- Tests run only in debug builds, after filesystem mounts and before SMP bring-up. Release builds boot straight to the desktop.
- Test files live next to the code they cover as `*_tests.cpp`: PMM/VMM/heap tests in `src/mm/tests/`, scheduler/SMP/syscall tests in `src/kernel/tests/`, congestion-control and DNS tests in `src/net/`.

Notable coverage: zeroed-frame and double-free guards, HHDM round trips, heap realloc patterns and calloc overflow, ready-queue state guards, exactly-once enqueue across cores, per-CPU sanity, a threaded stress mix (heap churn, irqsave spinlocks, mutex handoff), TCP congestion policy, and hostile DNS input.

## Smoke Suites

```sh
meson test -C build/debug --suite smoke --print-errorlogs
```

The harness (`tools/qemu_smoke.py`) boots `boot.img` headless with serial on stdio and enforces:

- Success markers: `first desktop frame submitted` (always) and `ktest suite passed` (debug builds).
- Failure markers: `ktest suite failed`, `KERNEL PANIC`.

SMP suites are opt-in and heavier:

```sh
meson test -C build/debug --suite smoke-smp     # 2 cores
meson test -C build/debug --suite smoke-smp4    # 4 cores; also requires "SMP scheduler ready on 4 CPUs"
meson compile -C build/debug smp-soak           # repeated 4-core boots, sessions held briefly
```

Timeouts scale with the machine: Linux without KVM access runs under TCG with much larger budgets (CI grants the runner KVM access and falls back to TCG when `/dev/kvm` is unusable).

## What to Run When

- **Boot / kernel start / display / init changes**: must boot in QEMU (serial + graphical) and pass the smoke suite.
- **Storage / `/data` changes**: exercise a path that mounts the FAT32 `UNI_DATA` volume (the default `boot.img` run does this).
- **Scheduler / SMP changes**: the SMP suites, plus `smp-soak` for scheduling work.
- **Anything touching docs build**: `meson compile -C build/debug wiki` (strict link checking fails on broken references).

## CI

`.github/workflows/ci.yml` builds debug and release, runs the smoke suite on the debug image, then lint (cppcheck) and a format check (`git diff --exit-code` after `format`). Lint and format are continue-on-error; keep the tree format-clean locally.
