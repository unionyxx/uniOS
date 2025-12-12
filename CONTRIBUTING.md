# Contributing to uniOS

Thank you for your interest in contributing to uniOS!

## Getting Started

1. Fork the repository
2. Clone your fork: `git clone https://github.com/YOUR_USERNAME/uniOS.git`
3. Set up the build environment (see README.md)
4. Create a feature branch: `git checkout -b feature/my-feature`

## Build Requirements

- GCC cross-compiler for `x86_64-elf`
- NASM assembler
- xorriso (for ISO creation)
- QEMU (for testing)
- Python 3 (for uniFS generation)

## Code Style

- **C++20** with kernel restrictions (no exceptions, no RTTI)
- Use `kstring::` utilities for string operations
- Use `DEBUG_INFO/WARN/ERROR/LOG` macros for logging (debug builds only)
- Keep functions focused and documented
- Use named constants instead of magic numbers

## Testing

**Testing is done in QEMU.** The OS targets real hardware, but hardware-specific issues may not be supportable.

```bash
make clean && make debug  # Build with logging
make run                  # Test in QEMU
make run-gdb              # Debug with GDB on localhost:1234
```

Verify:
- Kernel boots without errors
- Your feature works as expected
- Existing features still work (regression testing)

## Versioning

When your changes warrant a version bump (see `kernel/core/version.h` for rules):

1. Update `UNIOS_VERSION_*` macros in `kernel/core/version.h`
2. Update `UNIOS_VERSION_STRING` and `UNIOS_VERSION_FULL`
3. Update `Current Version:` line in `README.md`

**Don't bump for**: docs, refactoring, build changes, or minor cleanup.

## Pull Request Process

1. Ensure your code builds cleanly with both `make release` and `make debug`
2. Test thoroughly in QEMU
3. Update documentation if needed
4. Submit a PR with a clear description of changes

## Areas for Contribution

- **Network**: Protocol implementations (TCP improvements, new protocols)
- **Shell**: New commands and shell features
- **Filesystem**: uniFS enhancements
- **Documentation**: Improve docs and comments
- **Drivers**: QEMU-compatible device support

## Questions?

Open an issue for questions or feature discussions.

