# uniOS

![uniOS Screenshot](docs/screenshot.png)

> **A minimal, 64-bit operating system kernel built from scratch.**

![License](https://img.shields.io/badge/license-MIT-blue.svg?style=flat-square)
![Platform](https://img.shields.io/badge/platform-x86__64-lightgrey.svg?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B20-orange.svg?style=flat-square)

**uniOS** is a Minimalist x86 OS in C++20. It serves as a clean, modern, and hackable educational resource for understanding operating system internals, featuring a custom kernel, native drivers, and a transparent design philosophy.

Current Version: **v0.2.1**

---

## Features

*   **Modern Core**: Custom C++20 kernel with minimal assembly stubs.
*   **Memory Management**: Robust PMM (Bitmap), VMM (Page Tables), and Kernel Heap (Bucket Allocator).
*   **USB Stack**: Native xHCI driver with support for HID devices (Keyboard & Mouse) and DMA-safe memory.
*   **Boot Protocol**: Powered by **Limine** (v8.x) for a seamless, quiet boot experience.
*   **Visuals**: Direct framebuffer access with custom font rendering and a sleek dark theme.
*   **Architecture**: Full GDT, IDT, and interrupt handling setup.

## Known Issues (Real Hardware)

While uniOS runs perfectly in emulators, real hardware can be unpredictable:
*   **USB Mouse**: Support is improved but may still vary by controller. *Fallback: PS/2 mouse support is active.*
*   **ACPI Poweroff**: Shutdown logic is robust but depends on ACPI table correctness. *Fallback: Manual power off may be required.*
*   **Scroll Performance**: Scrolling may be slower on high-resolution displays (4K+) due to unoptimized framebuffer movement.

## Getting Started

### Prerequisites
*   `gcc` (cross-compiler for `x86_64-elf`)
*   `nasm`
*   `xorriso`
*   `qemu-system-x86_64`

### Build & Run

```bash
# Clone the repository
git clone https://github.com/unionyxx/uniOS.git
cd uniOS

# Build Limine (one-time setup)
git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
make -C limine

# Compile and emulate
make run
```

## Structure

| Directory | Description |
|-----------|-------------|
| `kernel/` | Core kernel source code (C++). |
| `boot/` | Bootloader configuration files. |
| `libc/` | Custom C standard library implementation. |
| `userspace/` | User-mode applications and shells. |

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
