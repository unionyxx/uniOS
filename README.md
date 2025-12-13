# uniOS

> **A scratch-built x86-64 operating system kernel written in C++20.**

![License](https://img.shields.io/badge/license-MIT-blue.svg?style=for-the-badge&logo=none)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg?style=for-the-badge&logo=intel)
![Language](https://img.shields.io/badge/language-C%2B%2B20-orange.svg?style=for-the-badge&logo=c%2B%2B)

![uniOS Screenshot](docs/screenshot.png)

**uniOS** is a hobby operating system built from scratch in C++20. It features a working shell with command piping, TCP/IP networking, USB support, and runs on real x86-64 hardware.

Current Version: **v0.5.2**

---

## Features

*   **Modern Core Architecture**
    Written in **C++20** with minimal assembly stubs. The kernel leverages modern language features for cleaner, safer, and more expressive code.

*   **Networking Stack**
    A complete, scratch-built TCP/IP stack supporting **Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, and DNS**.
    *   *Drivers*: Intel e1000, I217, I218, I219, I225, and Realtek RTL8139.

*   **USB Subsystem**
    Native **xHCI (USB 3.0)** driver implementation with full HID support for keyboards and mice.

*   **Memory Management**
    Robust **PMM** (Bitmap), **VMM** (4-level Paging), and a custom **Kernel Heap** (Bucket Allocator) for efficient memory usage.

*   **Filesystem (uniFS)**
    A custom, lightweight flat filesystem supporting dynamic file creation, inspection, and file type detection.

*   **Interactive Shell**
    A feature-rich command-line interface with **tab completion**, command history, line editing, and visual text selection.

*   **Visuals**
    Direct framebuffer access with custom font rendering, supporting a sleek dark theme and smooth scrolling.

## Development & Testing

uniOS is designed to run on **real x86_64 hardware** and is primarily tested on **QEMU/KVM**. QEMU enables rapid development with full debugging support (GDB, snapshots, device inspection).

> **Note**: While targeting most machines, hardware support is provided on a best-effort basis. Issues on specific hardware may not be fixable due to the scope of one-person development.

## Getting Started

### Prerequisites
*   `gcc` (cross-compiler for `x86_64-elf`)
*   `nasm`
*   `xorriso`
*   `qemu-system-x86_64`
*   `python3` (for uniFS image generation)

### Build & Run

```bash
# Clone the repository
git clone https://github.com/unionyxx/uniOS.git
cd uniOS

# Build Limine (one-time setup)
git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
make -C limine

# Build and run (release mode - optimized, no debug output)
make run

# Or build debug mode (full logging)
make debug && make run
```

### Make Targets

| Target | Description |
|--------|-------------|
| `make` / `make release` | Build optimized release version |
| `make debug` | Build with debug logging enabled |
| `make run` | Run in QEMU |
| `make run-net` | Run with e1000 networking |
| `make run-serial` | Run with serial output to stdio |
| `make run-gdb` | Run with GDB stub on localhost:1234 |
| `make clean` | Remove build artifacts |
| `make help` | Show all available targets |

## Shell Commands

| Category | Command | Description |
|----------|---------|-------------|
| **Files** | `ls` | List files with type and size |
| | `cat <file>` | Display text file contents |
| | `stat <file>` | Show file information |
| | `hexdump <file>` | Hex dump of file (first 256 bytes) |
| | `touch <file>` | Create empty file |
| | `rm <file>` | Delete file |
| | `write <file> <text>` | Write text to file |
| | `append <file> <text>` | Append text to file |
| | `df` | Show filesystem stats |
| **Text** | `wc [file]` | Count lines, words, characters |
| | `head [n] [file]` | Show first n lines (default 10) |
| | `tail [n] [file]` | Show last n lines (default 10) |
| | `grep <pattern> [file]` | Search for pattern (case-insensitive) |
| | `sort [file]` | Sort lines alphabetically |
| | `uniq [file]` | Remove consecutive duplicate lines |
| | `rev [file]` | Reverse characters in each line |
| | `tac [file]` | Print lines in reverse order |
| | `nl [file]` | Number lines |
| | `tr <a> <b>` | Translate character a to b |
| | `echo <text>` | Print text |
| **System** | `mem` | Show memory usage |
| | `date` | Show current date/time |
| | `uptime` | Show system uptime |
| | `version` | Show kernel version |
| | `cpuinfo` | CPU information |
| | `lspci` | List PCI devices |
| **Network** | `ifconfig` | Show network configuration |
| | `dhcp` | Request IP via DHCP |
| | `ping <host>` | Ping an IP or hostname |
| **Scripting** | `run <file>` | Execute script file |
| | `set NAME=value` | Set variable (or list all) |
| | `unset NAME` | Remove variable |
| | `if/else/endif` | Conditional blocks |
| | `while/end` | Loop blocks |
| **Other** | `clear` | Clear screen |
| | `gui` | Start GUI mode |
| | `reboot` | Reboot system |
| | `poweroff` | Shutdown (ACPI) |

> **Tip:** Commands can be piped: `ls | grep elf | wc`
> 
> **Scripting:** Use `\n` for newlines: `write test.sh "set X=1\necho $X"`

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Tab` | Command completion |
| `Ctrl+A` | Move to start of line |
| `Ctrl+E` | Move to end of line |
| `Ctrl+U` | Cut text before cursor |
| `Ctrl+K` | Cut text after cursor |
| `Ctrl+W` | Delete word before cursor |
| `Ctrl+Y` | Paste from clipboard |
| `Ctrl+C` | Copy selection / cancel line |
| `Ctrl+L` | Clear screen |
| `Shift+←/→` | Select text |
| `↑/↓` | Navigate command history |
| `←/→` | Move cursor in line |

## Structure

| Directory | Description |
|-----------|-------------|
| `kernel/core` | Core kernel logic (kmain, debug, scheduler). |
| `kernel/arch` | Architecture-specific code (GDT, IDT, interrupts). |
| `kernel/mem` | Memory management (PMM, VMM, Heap). |
| `kernel/drivers` | Device drivers (PCI, Timer, Graphics, Input, USB). |
| `kernel/net` | Network stack implementation. |
| `kernel/fs` | Filesystem support (uniFS). |
| `kernel/shell` | Kernel shell implementation. |
| `tools/` | Build tools (mkunifs.py). |
| `rootfs/` | Files included in the uniFS image. |

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
