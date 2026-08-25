# uniOS Architecture

## Boot Sequence

uniOS boots using the in-tree Meridian UEFI bootloader.

1.  Firmware starts `EFI/BOOT/BOOTX64.EFI`.
2.  Meridian loads `kernel.elf` and `unifs.img` from the EFI system partition.
3.  Meridian selects a GOP framebuffer mode.
4.  Meridian builds the uniOS `BootInfo` structure.
5.  Meridian creates initial higher-half page tables.
6.  Meridian exits boot services and jumps to the kernel entry point.

## Boot Media

- **`boot.img`**: The primary system disk image. It includes an EFI system partition and a dedicated, writable `UNI_DATA` FAT32 partition for persistent storage.
- **`uniOS.iso`**: A UEFI-bootable ISO image for cross-platform compatibility and testing.

Both images support persistence by automatically mounting any discovered FAT32 volume labeled `UNI_DATA` to `/data`.

## Kernel Startup

Upon entry, the kernel initializes core subsystems in order:

- CPU features, GDT, and IDT.
- Interrupt controllers (APIC/ACPI).
- Memory management (PMM, VMM, and Heap).
- Scheduler and threading.
- Device drivers and VFS.
- `unifs.img` mount (system root).
- `/data` mount (persistent storage from FAT32 volume).
- Kernel tests (debug builds only).
- SMP bring-up of application processors.
- Deferred services (networking, etc.) and userspace `init` launch.

Debug builds retain framebuffer and serial boot logs and run registered kernel tests. Release builds boot directly to the desktop session.

## Memory Management

### Physical Memory Manager (PMM)
Uses a bitmap-backed frame allocator initialized from the UEFI memory map. It tracks frame references for address space accounting and DMA safety.

### Virtual Memory Manager (VMM)
Implements x86-64 four-level paging with higher-half kernel mappings. User processes occupy isolated lower-half address spaces. Supports MMIO, DMA, and page-level protection. Address-space invalidations across cores use a TLB-shootdown IPI with monotonic sequence acknowledgements, retry, and a bounded timeout.

### Kernel Heap
Provides bucketed allocation for small objects and page-backed ranges for large buffers, protected by kernel spinlocks.

## Processes and Syscalls

Userspace programs are ELF binaries loaded from the VFS. The syscall interface provides:

- Process lifecycle (fork, exec, exit, wait) and threads.
- File and directory operations (open, read, write, stat), pipes, and memfd.
- Display and GUI composition.
- Shared memory, futexes, epoll, and inter-process signals.
- TCP/UDP networking and DNS.
- System power control (reboot, poweroff).

## Desktop Session

`/bin/init.elf` initializes the graphical environment:

- **Window Manager**: Desktop composition, window metadata, input focus, damage tracking, wallpaper loading, and Alt+Tab focus cycling.
- **System Services**: Menubar, Dock, and Desktop launcher.
- **Applications**: Terminal, Files, Preferences, Latitude, Clock, Calendar, Calculator, and About.

## Display & Graphics

The kernel display path handles framebuffer abstraction, dirty-region tracking, and compositor buffer submission. It supports framebuffer metadata from boot and provides display syscalls for presentation and event waiting.

## Input Subsystem

Supports PS/2 keyboard/mouse (including IntelliMouse scroll) and USB HID keyboard/mouse via xHCI. Kernel-level event conversion provides unified input to the window manager.

## Storage and Filesystems

- **VFS**: Virtual filesystem switch abstraction.
- **uniFS**: Read-only system filesystem for `unifs.img`.
- **FAT32**: Persistent data storage with cluster-hinting and FSInfo optimization.
- **Drivers**: ATA, AHCI, and USB Mass Storage (MSC).

## Networking

Full-stack networking including Ethernet, ARP, IPv4, ICMP, UDP, TCP, DHCP, and DNS. Native drivers for e1000 and RTL8139 are included.

TCP uses a windowed send path with multiple segments in flight and RFC 5681-style congestion control: slow start, congestion avoidance, fast retransmit/recovery on duplicate ACKs, and retransmission timeouts with exponential backoff. Round-trip time follows a Jacobson/Karels estimate with Karn's rule. The effective send window is the tightest of the congestion window, the peer's advertised window, and the socket's send buffer.

## SMP

uniOS supports multi-core x86-64 operation; see [SMP](smp.md) for AP bring-up, scheduling, shutdown, and validation details.

## Audio

Supports AC97 and High Definition Audio (HDA) controllers with a native playback path in userspace.

## Runtime Assets

The environment uses specialized binary formats to avoid heavy runtime parsing:

- `.uoic`: Pre-rendered icon packages.
- `.uocu`: Cursor packages with hotspot metadata.
- `.uof`: Processed bitmap font data.
- `.uowp`: Multi-variant wallpaper containers.
