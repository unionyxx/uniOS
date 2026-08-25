# uniOS Architecture

uniOS is a freestanding x86-64 operating system in C++20: a kernel with no `std`, no exceptions, and no RTTI, plus native ELF userspace. This page maps the system; each topic links to a dedicated page.

## Boot Sequence

1. Firmware starts `EFI/BOOT/BOOTX64.EFI` (Meridian).
2. Meridian loads `KERNEL.ELF` and `UNIFS.IMG` from the EFI system partition.
3. Meridian selects a GOP framebuffer mode (EDID-guided).
4. Meridian builds the `BootInfo` structure and initial higher-half page tables.
5. Meridian exits boot services and jumps to the kernel entry.
6. The kernel initializes subsystems, mounts filesystems, runs tests (debug), brings up APs, then launches `/bin/init.elf`.

Details: [Boot path](boot.md), [Boot images](images.md).

## Subsystem Map

| Subsystem | Location | Page |
| --- | --- | --- |
| Physical/virtual memory, heap | `src/mm/` | [Memory management](memory.md) |
| Scheduler, time | `src/kernel/sched/`, `src/kernel/time/` | [Scheduling](scheduling.md) |
| Multi-core bring-up | `src/kernel/smp/` | [SMP](smp.md) |
| Processes, syscalls | `src/kernel/core/` | [Processes](processes.md), [System calls](syscalls.md) |
| VFS, pipes, memfd | `src/fs/` | [VFS](vfs.md) |
| uniFS, FAT32 | `src/fs/unifs/`, `src/fs/fat32/` | [Filesystems](filesystems.md) |
| PCI, ACPI, APIC | `src/drivers/bus/pci/`, `src/drivers/acpi/`, `src/drivers/apic/` | [Drivers](drivers.md) |
| ATA, AHCI, USB MSC | `src/drivers/storage/` | [Storage drivers](storage-drivers.md) |
| USB xHCI/HID | `src/drivers/bus/usb/` | [USB](usb.md) |
| Input | `src/drivers/class/hid/`, `src/kernel/core/input*.cpp` | [Input](input.md) |
| Display | `src/drivers/video/` | [Display](display.md) |
| Audio | `src/drivers/sound/` | [Audio](audio.md) |
| Network stack | `src/net/` | [Networking](networking.md), [TCP](tcp.md) |
| Userspace runtime | `src/usr/` | [Userspace runtime](userspace.md) |
| Window manager | `src/usr/wm/` | [Window manager](wm.md) |
| Shell | `src/usr/shell/` | [Shell](shell.md), [Scripting](scripting.md) |

## Memory Management

The PMM is a bitmap allocator with per-frame reference counts, initialized from the UEFI memory map; frames are allocated zeroed. The VMM implements 4-level paging with a higher-half kernel (link base `0xffffffff80000000`, HHDM at `0xFFFF800000000000`) and isolated lower-half user spaces. Copy-on-write, demand paging, an MMIO/DMA window, and sequence-acknowledged TLB shootdowns are built in. The kernel heap is bucketed (32-4096 bytes) with page-granular large allocations. See [Memory management](memory.md).

## Processes and Syscalls

Userspace programs are ELF binaries loaded from the VFS. Syscalls use the `syscall` instruction with a System V-style register convention. The surface covers process lifecycle (fork, exec, exit, wait), threads, files, pipes, memfd, anonymous and shared memory, futexes, epoll, signals, display/composition, shared-memory IPC, networking, and power. See [System calls](syscalls.md).

## Desktop Session

`/bin/init.elf` starts the window manager, menubar, dock, and optionally a terminal, and supervises them. The WM is a userspace compositor: window metadata lives in a shared-memory registry, client buffers are memfds transferred to the WM, and frames are submitted through the kernel display API. See [Window manager](wm.md) and [Desktop services and apps](apps.md).

## Display and Graphics

The kernel display path abstracts the firmware framebuffer with double buffering and dirty-region tracking, and exposes a KMS-style syscall surface (caps, modes, buffers, present, compose, events). The only backend is the GOP framebuffer; EDID provides mode/timing metadata. See [Display](display.md).

## Input

PS/2 keyboard/mouse (scan code set 1, IntelliMouse scroll) and USB HID devices feed a merged kernel input layer. Graphical input is queued to the window manager's event queue; text input is polled through stdin reads. See [Input](input.md).

## Storage and Filesystems

Boot content comes from `unifs.img` (read-only with a volatile RAM overlay); persistent data lives on a FAT32 volume labeled `UNI_DATA`, mounted at `/data` with storage-guard gating. Block devices come from ATA PIO, AHCI DMA, and USB mass storage; partitions are MBR/GPT. See [Filesystems](filesystems.md) and [Storage drivers](storage-drivers.md).

## Networking

A polled IPv4 stack: Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS over e1000 or RTL8139. TCP implements the full state machine with windowed send, RFC 5681-style congestion control (slow start, congestion avoidance, fast retransmit/recovery), Jacobson/Karels RTT estimation with Karn's rule, and RTO backoff. The effective send window is the tightest of the congestion window, the peer's advertised window, and the socket's send buffer. See [Networking](networking.md) and [TCP](tcp.md).

## SMP

Multi-core operation with MADT-driven AP bring-up, per-core GDT/TSS/PAT/syscall state, RESCHED IPIs, STOP IPI shutdown, and up to 8 cores. See [SMP](smp.md).

## Audio

AC97 and HDA controllers with polled playback; HDA also records. Kernel-side WAV parsing, userspace PCM feeds. See [Audio](audio.md).

## Runtime Assets

Specialized binary formats avoid runtime parsing:

- `.uoic`: pre-rendered icon packages.
- `.uocu`: cursor packages with hotspot metadata.
- `.uof`: processed bitmap font data.
- `.uowp`: multi-variant wallpaper containers.

Binaries under `rootfs/usr/share/` are committed and regenerated from sources by the tools in `tools/`. See [Asset formats](formats/README.md).
