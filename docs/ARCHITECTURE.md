# uniOS Architecture

> Technical overview for contributors and developers.

## Overview

**uniOS** is a scratch-built x86-64 operating system kernel written in C++20. It boots via Limine and provides a shell interface with networking, USB support, and a RAM-based filesystem.

Development is primarily done in **QEMU** for rapid iteration with debugging support. The OS targets **real x86_64 hardware** but hardware-specific issues may not be supportable due to project scope.

```
┌─────────────────────────────────────────┐
│              User Shell                 │
├─────────────────────────────────────────┤
│      Filesystem (uniFS)  │  Terminal    │
├──────────────────────────┴──────────────┤
│            Network Stack                │
│   (TCP/UDP/ICMP/IPv4/ARP/Ethernet)      │
├─────────────────────────────────────────┤
│              Drivers                    │
│   (e1000, xHCI, PS/2, Timer, ACPI)      │
├─────────────────────────────────────────┤
│              Core Kernel                │
│   (Scheduler, Interrupts, Memory)       │
├─────────────────────────────────────────┤
│         Hardware Abstraction            │
│       (GDT, IDT, PIC, I/O Ports)        │
└─────────────────────────────────────────┘
```

## Versioning

uniOS follows semantic versioning with a pre-1.0 adaptation:

| Phase | Format | Description |
|-------|--------|-------------|
| **Pre-1.0** | `0.MAJOR.MINOR` | Active development. MAJOR for new subsystems, MINOR for features/fixes. |
| **Post-1.0** | `MAJOR.MINOR.PATCH` | Stable. MAJOR for breaking changes, MINOR for features, PATCH for fixes. |

The canonical version is defined in `kernel/core/version.h`. See that file for detailed increment rules.

## Memory Layout

| Address Range | Usage |
|---------------|-------|
| `0x0000_0000_0000_0000` - `0x0000_7FFF_FFFF_FFFF` | User space (unused) |
| `0xFFFF_8000_0000_0000` + | Higher Half Direct Map (HHDM) |
| Dynamic | Kernel heap (bucket allocator) |

### Memory Management

- **PMM**: Bitmap-based physical frame allocator
- **VMM**: 4-level page tables (PML4)
- **Heap**: Bucket allocator for small objects, page-based for large

## Key Subsystems

### uniFS (Filesystem)

Simple flat filesystem with two parts:
- **Boot files**: Loaded from Limine module (read-only)
- **RAM files**: Created at runtime (read-write, lost on reboot)

```cpp
// Create a file
unifs_create("notes.txt");

// Write to it
unifs_write("notes.txt", "Hello", 5);

// Read it back
const UniFSFile* f = unifs_open("notes.txt");
```

### Network Stack

```
Application → TCP/UDP → IPv4 → ARP → Ethernet → e1000/RTL8139
```

- Full DHCP client
- DNS resolution
- ICMP ping

### USB (xHCI)

- USB 3.0 host controller driver
- HID support (keyboard, mouse)
- Interrupt-based polling

## Build System

```bash
make          # Release build (optimized, no debug)
make debug    # Debug build (full logging)
make run-gdb  # Run with GDB stub
```

## Directory Structure

```
kernel/
├── core/       # Entry point, version, debug, scheduler
├── arch/       # GDT, IDT, interrupts, I/O
├── mem/        # PMM, VMM, heap
├── drivers/    # Hardware drivers
│   ├── net/    # e1000, RTL8139
│   └── usb/    # xHCI, HID
├── net/        # TCP/IP stack
├── fs/         # uniFS filesystem
└── shell/      # Command interpreter
```

## Coding Conventions

- C++20 standard
- No exceptions (`-fno-exceptions`)
- No RTTI (`-fno-rtti`)
- Kernel runs in ring 0
- All memory is kernel-accessible

## Key Files

| File | Purpose |
|------|---------|
| `kernel/core/kmain.cpp` | Kernel entry point |
| `kernel/core/version.h` | Version info and policy |
| `kernel/core/kstring.h` | Shared string utilities |
| `kernel/mem/heap.cpp` | Dynamic memory allocator |
| `kernel/net/tcp.cpp` | TCP implementation |
| `kernel/drivers/usb/xhci.cpp` | USB 3.0 driver |

