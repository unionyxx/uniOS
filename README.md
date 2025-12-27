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

Current Version: **v0.6.3**

---

## Features

- **C++20 Kernel** — Built with `-fno-exceptions` and `-fno-rtti`. Uses `kstring::` utilities instead of `std::` to avoid libc dependencies.

- **Bitmap PMM & 4-Level Paging** — Physical memory tracked via bitmap allocator. Currently capped at 16GB to prevent bitmap overflow. Recursive 4-level paging for virtual memory.

- **Preemptive Multitasking** — 1000Hz timer-based scheduling. 16KB kernel stacks per process (sized for deep networking call chains). FPU/SSE context saved via `fxsave`/`fxrstor`.

- **Scratch-built TCP/IP Stack** — Not a port of lwIP. Hand-written Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, and DNS. Tested with `ping` and basic TCP handshakes.

- **Native xHCI Driver** — USB 3.0 host controller support. HID keyboards and mice work via interrupt transfers. No hub support.

- **uniFS** — Simple flat filesystem. Boot files loaded from Limine module (read-only), runtime files stored in RAM (lost on reboot).

- **Shell** — Command-line interface with tab completion, history, piping (`ls | grep elf | wc`), and scripting support.

- **AC97 Audio** — Basic sound card driver. Play WAV/PCM files from the shell. Supports 16-bit stereo audio.

## Known Limitations

> [!WARNING]
> These are architectural constraints, not bugs.

| Limitation | Details |
|------------|---------|
| **16GB RAM cap** | PMM bitmap is statically sized. Memory above 16GB is ignored. |
| **Experimental user-mode** | Basic syscall interface (exit, read, write). No memory protection between processes yet. |
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
| `make run-usb` | Run with xHCI USB (keyboard/mouse) |
| `make run-sound` | Run with AC97 sound card |
| `make run-serial` | Run with serial output to stdio |
| `make run-gdb` | Run with GDB stub on `localhost:1234` |
| `make clean` | Remove build artifacts |

> [!TIP]
> Use `make run-gdb` to attach a debugger to QEMU on localhost:1234.

## Shell Commands

| Category | Command | Description |
|----------|---------|-------------|
| **Files** | `ls` | List files with type and size |
| | `cat <file>` | Display text file contents |
| | `stat <file>` | Show file information |
| | `hexdump <file>` | Hex dump of file |
| | `touch <file>` | Create empty file |
| | `rm <file>` | Delete file |
| | `write <file> <text>` | Write text to file |
| | `append <file> <text>` | Append text to file |
| | `df` | Show filesystem usage |
| **Text** | `grep <pattern> [file]` | Search for pattern |
| | `wc [file]` | Count lines, words, characters |
| | `head [n] [file]` | Show first N lines (default 10) |
| | `tail [n] [file]` | Show last N lines (default 10) |
| | `sort [file]` | Sort lines alphabetically |
| | `uniq [file]` | Remove consecutive duplicates |
| | `rev [file]` | Reverse each line |
| | `tac [file]` | Reverse line order |
| | `nl [file]` | Number lines |
| | `tr <from> <to>` | Translate characters |
| | `echo <text>` | Print text |
| **System** | `mem` | Show memory usage |
| | `uptime` | Show system uptime |
| | `date` | Show current date/time |
| | `cpuinfo` | Show CPU information |
| | `lspci` | List PCI devices |
| | `version` | Show kernel version |
| | `uname` | Show system name |
| | `clear` | Clear screen |
| | `reboot` | Restart system |
| | `poweroff` | Shutdown system |
| **Network** | `ifconfig` | Show network configuration |
| | `dhcp` | Request IP via DHCP |
| | `ping <host>` | Ping an IP or hostname |
| **Audio** | `audio status` | Show audio device status |
| | `audio play <file>` | Play WAV file |
| | `audio pause` | Pause playback |
| | `audio resume` | Resume playback |
| | `audio stop` | Stop playback |
| | `audio volume [0-100]` | Get/set volume |
| **Scripting** | `run <file>` | Execute script file |
| | `source <file>` | Execute in current context |
| | `set NAME=value` | Set variable |
| | `unset NAME` | Unset variable |
| | `env` | List all variables |
| | `test <expr>` | Evaluate expression |
| | `expr <math>` | Arithmetic expression |
| | `read <var>` | Read user input |
| | `sleep <ms>` | Sleep milliseconds |
| | `time <cmd>` | Time command execution |
| | `true` / `false` | Exit status 0 / 1 |

> [!NOTE]
> Commands can be piped: `ls | grep elf | wc`
> 
> The shell parser splits by spaces. Pipes use a fixed 4KB buffer.

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| <kbd>Tab</kbd> | Command/filename completion |
| <kbd>Ctrl</kbd>+<kbd>A</kbd> | Move to start of line |
| <kbd>Ctrl</kbd>+<kbd>E</kbd> | Move to end of line |
| <kbd>Ctrl</kbd>+<kbd>U</kbd> | Cut text before cursor |
| <kbd>Ctrl</kbd>+<kbd>K</kbd> | Cut text after cursor |
| <kbd>Ctrl</kbd>+<kbd>W</kbd> | Delete word before cursor |
| <kbd>Ctrl</kbd>+<kbd>Y</kbd> | Paste (yank) |
| <kbd>Ctrl</kbd>+<kbd>C</kbd> | Copy selection / cancel line |
| <kbd>Ctrl</kbd>+<kbd>L</kbd> | Clear screen |
| <kbd>↑</kbd>/<kbd>↓</kbd> | Navigate command history |
| <kbd>←</kbd>/<kbd>→</kbd> | Move cursor |
| <kbd>Home</kbd>/<kbd>End</kbd> | Jump to start/end |
| <kbd>Delete</kbd> | Delete character at cursor |
| <kbd>Shift</kbd>+<kbd>←</kbd>/<kbd>→</kbd> | Select text |

## Project Structure

```text
kernel/
├── core/       # Entry point, scheduler, terminal, debug
├── arch/       # GDT, IDT, interrupts, I/O
├── mem/        # PMM, VMM, heap
├── drivers/    # Hardware drivers
│   ├── net/    # e1000, RTL8139
│   ├── usb/    # xHCI, HID
│   └── sound/  # AC97
├── net/        # TCP/IP stack
├── fs/         # uniFS filesystem
└── shell/      # Command interpreter
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

