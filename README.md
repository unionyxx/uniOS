# uniOS

![uniOS Screenshot](docs/screenshot.png)

> **A minimal, 64-bit operating system kernel built from scratch.**

![License](https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B20-orange.svg?style=flat-square)

**uniOS** is a Minimalist x86 OS in C++20. It serves as a clean, modern, and hackable educational resource for understanding operating system internals, featuring a custom kernel, native drivers, and a transparent design philosophy.

Current Version: **v0.3.1**

---

## Features

*   **Modern Core**: Custom C++20 kernel with minimal assembly stubs.
*   **Filesystem**: uniFS - simple flat filesystem with file type detection.
*   **Networking**: Full TCP/IP stack (Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS).
*   **Driver Support**:
    *   **Network**: Intel e1000/I217/I218/I219/I225, Realtek RTL8139.
    *   **USB**: Native xHCI driver with HID support (Keyboard & Mouse).
    *   **Input**: PS/2 Keyboard & Mouse.
*   **Memory Management**: Robust PMM (Bitmap), VMM (Page Tables), and Kernel Heap (Bucket Allocator).
*   **Boot Protocol**: Powered by **Limine** (v8.x) for a seamless, quiet boot experience.
*   **Visuals**: Direct framebuffer access with custom font rendering and a sleek dark theme.
*   **Interactive Shell**: Command history, line editing, file inspection, and network commands.

## Development & Testing

uniOS is written to be **portable across all x86_64 machines**, but is primarily tested on **QEMU/KVM**. This allows for rapid development with full debugging support (GDB, snapshots, device inspection).

> **Note**: Real hardware support is a future goal. The OS *may* work on physical machines, but stability is not guaranteed until dedicated hardware testing is implemented.

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
| **System** | `mem` | Show memory usage |
| | `date` | Show current date/time |
| | `uptime` | Show system uptime |
| | `version` | Show kernel version |
| | `cpuinfo` | CPU information |
| | `lspci` | List PCI devices |
| **Network** | `ifconfig` | Show network configuration |
| | `dhcp` | Request IP via DHCP |
| | `ping <host>` | Ping an IP or hostname |
| **Other** | `clear` | Clear screen |
| | `gui` | Start GUI mode |
| | `reboot` | Reboot system |
| | `poweroff` | Shutdown (ACPI) |

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
