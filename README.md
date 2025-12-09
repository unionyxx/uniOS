# uniOS

![uniOS Screenshot](docs/screenshot.png)

A minimal, 64-bit operating system kernel built from scratch.

This project is an exploration of low-level systems programming, focusing on clean architecture and modern C++ implementation. It is not intended to replace your daily driver, but to serve as a transparent, hackable educational resource.

Current Version: **v0.1**

## Overview

uniOS targets the x86-64 architecture, utilizing the Limine bootloader for a modern boot protocol.

**Core Features:**
*   **Kernel:** Custom C++ kernel with minimal assembly stubs.
*   **Boot:** Limine bootloader (v8.x) with quiet boot and splash screen.
*   **USB Stack:** Full xHCI driver with HID support (Keyboard & Mouse).
*   **Graphics:** Direct framebuffer access with custom font rendering and black theme.
*   **Interrupts:** Full GDT and IDT setup.

## Getting Started

### Prerequisites
*   `gcc` (cross-compiler recommended for x86_64-elf)
*   `nasm`
*   `xorriso`
*   `qemu-system-x86_64`

### Build & Run
```bash
# Clone the repository
git clone https://github.com/YOUR_USERNAME/uniOS.git
cd uniOS

# Build Limine (one-time)
git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
make -C limine

# Compile and emulate
make run
```

## Structure

*   `kernel/` - Core kernel source (C++).
*   `boot/` - Boot configuration.
*   `libc/` - Custom C standard library implementation.
*   `userspace/` - User-mode applications.

## License

MIT
