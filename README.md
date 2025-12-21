<p align="center">
  <img src="docs/screenshot.png" alt="uniOS Screenshot" width="600">
</p>

<h1 align="center">uniOS</h1>

<p align="center">
  A scratch-built x86-64 operating system kernel written in C++20.
  <br>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-x86__64-lightgrey.svg?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/language-C%2B%2B20-orange.svg?style=flat-square" alt="Language">
</p>

---

**uniOS** is a hobby operating system built from scratch. It features a working shell with command piping, TCP/IP networking, USB support, and runs on real x86-64 hardware.

Current Version: **v0.6.1**

---

## Features

- **C++20 Kernel** — Built with `-fno-exceptions` and `-fno-rtti`. Uses `kstring::` utilities instead of `std::` to avoid libc dependencies.

- **Bitmap PMM & 4-Level Paging** — Physical memory tracked via bitmap allocator. Currently capped at 16GB to prevent bitmap overflow. Recursive 4-level paging for virtual memory.

- **Preemptive Multitasking** — 1000Hz timer-based scheduling. 16KB kernel stacks per process (sized for deep networking call chains). FPU/SSE context saved via `fxsave`/`fxrstor`.

- **Scratch-built TCP/IP Stack** — Not a port of lwIP. Hand-written Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, and DNS. Tested with `ping` and basic TCP handshakes.

- **Native xHCI Driver** — USB 3.0 host controller support. HID keyboards and mice work via interrupt transfers. No hub support.

- **uniFS** — Simple flat filesystem. Boot files loaded from Limine module (read-only), runtime files stored in RAM (lost on reboot).

- **Shell** — Command-line interface with tab completion, history, piping (`ls | grep elf | wc`), and scripting support.

## Known Limitations

> [!WARNING]
> These are architectural constraints, not bugs.

| Limitation | Details |
|------------|---------|
| **16GB RAM cap** | PMM bitmap is statically sized. Memory above 16GB is ignored. |
| **No user-mode** | All code runs in ring 0. No syscall interface yet. |
| **USB polling** | HID devices polled on timer, not via hardware interrupts. |
| **No USB hubs** | Only devices directly connected to root ports work. |
| **QEMU-first** | Tested primarily on QEMU. Real hardware may have driver issues. |

## Getting Started

### Prerequisites

- `gcc` (cross-compiler for `x86_64-elf`)
- `nasm`
- `xorriso`
- `qemu-system-x86_64`
- `python3` (for uniFS image generation)

### Build & Run

```bash
# Clone the repository
git clone https://github.com/unionyxx/uniOS.git
cd uniOS

# Build Limine (one-time setup)
git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
make -C limine

# Build and run
make run

# Or with networking
make run-net
```

### Make Targets

| Target | Description |
|--------|-------------|
| `make` | Build release (optimized, no debug output) |
| `make debug` | Build with `DEBUG_*` logging enabled |
| `make run` | Run in QEMU |
| `make run-net` | Run with e1000 networking |
| `make run-gdb` | Run with GDB stub on `localhost:1234` |
| `make clean` | Remove build artifacts |

> [!TIP]
> Use `make run-gdb` to attach a debugger to QEMU on localhost:1234.

## Shell Commands

| Category | Command | Description |
|----------|---------|-------------|
| **Files** | `ls` | List files with type and size |
| | `cat <file>` | Display text file contents |
| | `touch <file>` | Create empty file |
| | `rm <file>` | Delete file |
| | `write <file> <text>` | Write text to file |
| **Text** | `grep <pattern> [file]` | Search for pattern |
| | `wc [file]` | Count lines, words, characters |
| | `sort [file]` | Sort lines alphabetically |
| | `echo <text>` | Print text |
| **System** | `mem` | Show memory usage |
| | `uptime` | Show system uptime |
| | `lspci` | List PCI devices |
| **Network** | `ifconfig` | Show network configuration |
| | `dhcp` | Request IP via DHCP |
| | `ping <host>` | Ping an IP or hostname |
| **Scripting** | `run <file>` | Execute script file |
| | `set NAME=value` | Set variable |

> [!NOTE]
> Commands can be piped: `ls | grep elf | wc`
> 
> The shell parser splits by spaces. Pipes use a fixed 4KB buffer.

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| <kbd>Tab</kbd> | Command completion |
| <kbd>Ctrl</kbd>+<kbd>A</kbd> | Move to start of line |
| <kbd>Ctrl</kbd>+<kbd>E</kbd> | Move to end of line |
| <kbd>Ctrl</kbd>+<kbd>U</kbd> | Cut text before cursor |
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | Cut text after cursor |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Copy selection / cancel |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> | Clear screen |
| <kbd>↑</kbd>/<kbd>↓</kbd> | Navigate command history |

## Project Structure

```text
kernel/
├── core/       # Entry point, scheduler, debug
├── arch/       # GDT, IDT, interrupts, I/O
├── mem/        # PMM, VMM, heap
├── drivers/    # Hardware drivers
│   ├── net/    # e1000, RTL8139
│   └── usb/    # xHCI, HID
├── net/        # TCP/IP stack
├── fs/         # uniFS filesystem
└── shell/      # Command interpreter
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
