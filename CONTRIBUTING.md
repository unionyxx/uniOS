# Contributing to uniOS

uniOS is a hobby project. I welcome PRs, but keep in mind this is a learning OS, not a production kernel. If you're interested in kernel development, this is a good codebase to experiment with.

## Development Environment

I develop on **Ubuntu/WSL** and test primarily in **QEMU**. If it breaks on your specific hardware, I probably can't fix it without serial logs.

### Build Requirements

- GCC cross-compiler for `x86_64-elf`
- NASM assembler
- xorriso (for ISO creation)
- QEMU (for testing)
- Python 3 (for uniFS generation)

## Code Style

This is a **freestanding C++20 kernel**. That means:

- **No exceptions** (`-fno-exceptions`) — We can't unwind the stack in a kernel.
- **No RTTI** (`-fno-rtti`) — `dynamic_cast` and `typeid` don't exist.
- **No `std::`** — We use `kstring::` for string operations to avoid libc.
- **No `new`/`delete`** — Use `malloc()`/`free()` from the kernel heap.

### Logging

Use the `DEBUG_*` macros in `debug.h`:

```cpp
DEBUG_INFO("Initialized device %s\n", name);
DEBUG_WARN("Timeout after %d ms\n", timeout);
DEBUG_ERROR("Failed to allocate buffer\n");
```

These only print in debug builds (`make debug`). Release builds strip them.

### Constants

Use named constants. Magic numbers make debugging painful:

```cpp
// Bad
if (status & 0x10) { ... }

// Good
#define STATUS_READY (1 << 4)
if (status & STATUS_READY) { ... }
```

## Testing

```bash
make clean && make debug  # Build with logging
make run                  # Test in QEMU
make run-gdb              # Debug with GDB on localhost:1234
```

Before submitting a PR:
1. Verify it builds with both `make` and `make debug`
2. Test your feature in QEMU
3. Check that existing commands still work

> [!WARNING]
> I can't test on real hardware for every PR. If your change requires specific hardware, document how to test it.

## Versioning

Version is defined in `include/kernel/version.h`. Bump it if you add a feature. Don't bump for:
- Documentation changes
- Refactoring
- Build system tweaks

## Areas for Contribution

| Area | Notes |
|------|-------|
| **Network** | TCP improvements, new protocols |
| **Shell** | New commands (must use existing uniFS/kernel APIs) |
| **Drivers** | QEMU-compatible devices preferred |
| **Docs** | Fix errors, add examples |

## Out of Scope

Please do not open PRs for:

- **GUI window managers** — The shell is the focus
- **Alternative filesystems** (FAT32, ext4) — uniFS is intentional
- **32-bit (i386) support** — x86-64 only
- **UEFI runtime services** — We boot via Limine and don't use UEFI after
- **SMP (multicore)** — Single-core design for simplicity

## Questions?

Open an issue. I read them regularly.
