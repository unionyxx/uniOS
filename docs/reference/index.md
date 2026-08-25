# uniOS Wiki

uniOS is a freestanding x86-64 operating system written in C++20. It boots through the in-tree Meridian UEFI bootloader, starts a preemptive multi-core kernel, and launches a native desktop userspace session with a window manager, dock, menubar, and applications.

No libc in the kernel, no exceptions, no RTTI. Memory management, PCI, ACPI, USB, networking, audio, and display code are all written from scratch.

## System at a Glance

| Area | Summary |
| --- | --- |
| Boot | Meridian UEFI loader, repo-owned `BootInfo` ABI, GOP framebuffer with EDID-driven mode selection |
| Kernel | Higher-half C++20 kernel, preemptive O(1)-style scheduler, SMP up to 8 cores, syscalls via `syscall` instruction |
| Memory | Bitmap PMM with frame refcounts, 4-level paging, HHDM direct map, bucketed kernel heap |
| Filesystems | VFS with page cache, read-only uniFS root with volatile RAM overlay, FAT32 for persistent `/data` |
| Drivers | PCI (ECAM + legacy), ACPI/APIC, ATA, AHCI, USB xHCI/HID/MSC, PS/2, e1000, RTL8139, AC97, HDA |
| Networking | Polling IPv4 stack: ARP, ICMP, UDP, TCP (RFC 5681-style congestion control), DHCP, DNS |
| Userspace | Native ELF programs, libc subset, immediate-mode GUI library, shell with scripting, desktop apps |
| Display | Firmware framebuffer backend, double buffering, dirty regions, KMS-style present/compose syscalls |

## Reading Guide

**Getting started**

- [Building and running](build.md) — toolchain requirements, build targets, QEMU run targets, test suites.
- [Boot images](images.md) — what `efi.img`, `boot.img`, and `uniOS.iso` contain, and how they are assembled.

**Kernel**

- [Boot path](boot.md) — Meridian, the `BootInfo` ABI, and kernel startup order.
- [Memory management](memory.md) — PMM, VMM, kernel heap, TLB shootdowns.
- [Scheduling](scheduling.md) — runqueues, priorities, sleep/wakeup, timers.
- [SMP](smp.md) — AP bring-up, per-core state, shutdown protocol.
- [Processes](processes.md) — fork/exec/exit lifecycle and signals.
- [System calls](syscalls.md) — the syscall ABI and the complete call table.
- [VFS](vfs.md) — vnodes, mounts, page cache, pipes, memfd.
- [Filesystems](filesystems.md) — uniFS, FAT32, and the `/data` volume.
- [Drivers](drivers.md) — PCI, ACPI, APIC, and the driver binding model.
- [Storage drivers](storage-drivers.md) — ATA, AHCI, USB mass storage, partitions.
- [USB](usb.md) — xHCI host controller, enumeration, HID.
- [Input](input.md) — PS/2 and USB input, event delivery.
- [Display](display.md) — framebuffer, present/compose API.
- [Audio](audio.md) — AC97 and HDA playback.
- [Networking](networking.md) — stack layout, DHCP, DNS, socket API.
- [TCP](tcp.md) — connection state machine, congestion control, limits.

**Userspace**

- [Userspace runtime](userspace.md) — crt0, libc, libgui, how apps are built.
- [Window manager](wm.md) — the shared-memory registry and window protocol.
- [Shell](shell.md) — builtins, pipelines, line editing.
- [Shell scripting](scripting.md) — variables, conditionals, loops, limits.
- [Desktop services and apps](apps.md) — init, menubar, dock, bundled apps.

**Reference**

- [Runtime configuration](config.md) — settings files and keys.
- [Asset formats](formats/README.md) — `.uoic`, `.uocu`, `.uof`, `.uowp`.
- [Testing](testing.md) — ktest, smoke suites, SMP validation.

## Repository Layout

| Path | Contents |
| --- | --- |
| `src/bootloader/` | Meridian UEFI bootloader |
| `src/kernel/`, `src/mm/`, `src/fs/`, `src/net/`, `src/drivers/` | Kernel and subsystem code |
| `src/usr/` | Userspace runtime, libc subset, GUI library, shell, window manager, desktop services, apps |
| `include/` | Kernel, driver, boot, and UAPI headers |
| `rootfs/` | Authored runtime files staged into `unifs.img` |
| `appicons/`, `assets/`, `cursors/` | Sources for generated runtime assets |
| `tools/` | Image, filesystem, rootfs staging, asset conversion, and QEMU helper scripts |
| `toolchains/` | Meson cross-file configuration |
| `docs/` | Project site and this reference documentation |

## Quick Start

```sh
meson setup build/debug --cross-file toolchains/llvm.ini --buildtype debug
meson compile -C build/debug boot-disk iso
meson test -C build/debug --suite smoke --print-errorlogs
```
