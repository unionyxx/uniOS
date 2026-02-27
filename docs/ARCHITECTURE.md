# uniOS Architecture

Technical overview for contributors and developers.

## Overview

**uniOS** is a scratch-built x86-64 operating system kernel written in C++20. It boots via **Limine** and provides a shell interface with networking, USB support, and a RAM-based filesystem.

```mermaid
graph TD
    User[User Shell / GUI] --> VFS[uniFS / FAT32]
    User --> Net[Network Stack]
    User --> Audio[Audio Stack]
    User --> USB[USB Stack]
    
    subgraph Kernel Core
    Sched[Scheduler]
    Mem[PMM / VMM]
    GUI[Window Manager]
    end
    
    Net --> IPv4
    IPv4 --> Ethernet
    Ethernet --> Driver[e1000/RTL8139 Driver]
    
    Audio --> HDA[HDA Driver]
    Audio --> AC97[AC97 Driver]
    
    USB --> xHCI[xHCI Controller]
    xHCI --> HID[HID Keyboard/Mouse]
    xHCI --> HUB[Hub Detection (Initial)]
    
    Sched --> Context[Context Switch]
    Context --> TSS[TSS rsp0 Update]
```

## Memory Layout

| Virtual Address | Usage |
|-----------------|:------|
| `0x0000_0000_0000_0000` | User space (reserved, unused) |
| `0xFFFF_8000_0000_0000` | Higher Half Direct Map (HHDM) |
| `0xFFFF_FF80_0000_0000` | Fixed kernel stack per process |
| `0xFFFF_FFFF_9000_0000` | MMIO virtual base (`mmio_next_virt`) |

### Why These Addresses?

- **HHDM** is set by Limine. All physical memory is accessible at `phys + hhdm_offset`.
- **Kernel stack** is at a fixed virtual address so `fork()` doesn't corrupt RBP pointers. Each process has stacks at the same vaddr mapped to different physical pages.
- **MMIO** starts at a high address to avoid collisions with heap or HHDM.

## GUI & Graphics

### Windowing System

uniOS features a custom window manager in `src/kernel/core/gui.cpp`. Features include:
- Draggable windows with title bars and close buttons.
- Desktop environment with icons and taskbar.
- Real-time clock with periodic updates.
- Cursor management with background restoration (no flickering).

### SSE-Accelerated Renderer

The renderer in `src/drivers/video/framebuffer.cpp` is heavily optimized:
- **SSE2 Primitives**: Fills, copies, and gradients use 128-bit SIMD instructions.
- **Dirty Rectangle Tracking**: Only the portions of the screen that changed are copied to VRAM.
- **Double Buffering**: Renders to a RAM backbuffer before swapping to VRAM to eliminate tearing.
- **Bulk Transfers**: Full-width row copies are optimized to bypass row-by-row overhead.

## Memory Management

### PMM (Physical Memory Manager)

Bitmap-based allocator in `pmm.cpp`. The bitmap is dynamically sized at boot time to cover all usable physical memory reported by Limine.

> [!NOTE]
> The PMM tracks frame reference counts to support future copy-on-write functionality and shared memory.

### VMM (Virtual Memory Manager)

4-level paging (PML4 → PDPT → PD → PT).

Key functions:
- `vmm_map_page()` — Map in active PML4
- `vmm_map_page_in()` — Map in a passive PML4 (for `fork`)
- `vmm_clone_address_space()` — Deep copy user pages, share kernel pages
- `vmm_free_address_space()` — Free user pages on process exit

### Heap

Bucket allocator in `heap.cpp`. Fixed bucket sizes (32, 64, 128, ... bytes). Large allocations fall back to direct PMM pages.

Features **Page Coalescing**: Fully freed pages are automatically returned to the PMM instead of being held in bucket free-lists indefinitely, reducing memory waste.

Spinlock-protected for thread safety.

## Scheduler

Preemptive, timer-based at **1000Hz** (1ms granularity). Features **O(1) task insertion** using a tail pointer for the process list, improving performance when creating many tasks.

### Why 16KB Stacks?

```cpp
#define KERNEL_STACK_SIZE 16384
```

Deep call chains in networking (TCP → IP → ARP → driver → interrupt) can use 4-8KB. 16KB gives headroom. 4KB stacks caused overflows in practice.

### Context Switching

1. Save current task's callee-saved registers
2. Save FPU/SSE state via `fxsave`
3. Update `tss.rsp0` for next task
4. Switch CR3 if next task has different page table
5. Restore next task's state

### Process Isolation

`fork()` creates a new address space:
- Clones page tables (deep copy of user pages)
- Allocates separate physical stack pages
- Maps stack to same virtual address (`KERNEL_STACK_TOP`)
- Rebases RBP pointers when forking from HHDM-based kernel tasks

## Drivers

### Network (e1000)

Interrupt-driven RX, synchronous TX. Ring buffer descriptors. DHCP and DNS work reliably in QEMU. Real hardware support is best-effort.

### Interrupts (MSI-X)

Support for **Message Signaled Interrupts (MSI-X)**. MSI-X allows devices to bypass the I/O APIC and write interrupt messages directly to local APICs, reducing latency and allowing for more interrupt vectors.

### USB (xHCI)

Polling-based HID. Why not interrupts? xHCI interrupt handling requires async TRB processing which adds complexity. Polling at 1000Hz is good enough for keyboards.

### Audio (HDA & AC97)

Dual driver support for audio. **Intel HD Audio (HDA)** uses DMA-based CORB/RIRB transfers for modern hardware compatibility. **AC97** is supported for legacy systems.

## Filesystems

uniOS supports multiple filesystem types via a block device abstraction:

| Source | Storage | Writable |
|--------|---------|:--------:|
| Boot files | Limine module (uniFS) | No |
| RAM files | Kernel heap (uniFS) | Yes |
| Disk files | Block device (**FAT32**) | Initial |

Files are accessed via common interfaces, with FAT32 support currently in the early infrastructure stage (parsing boot sectors).

## Build System

```bash
make          # Release (optimized, -O2)
make debug    # Debug (logs enabled, -O0 -g)
make run-gdb  # Attach GDB to localhost:1234
```

## Directory Structure

```text
├── include/      # Header files
│   ├── boot/     # Limine boot protocol
│   ├── kernel/   # Kernel core headers
│   ├── drivers/  # Driver interfaces
│   └── libk/     # Kernel library
├── src/          # Source files
│   ├── kernel/   # Core logic
│   │   ├── core/ # kmain, cpu, irq, gui, syscalls
│   │   ├── sched/# Scheduler
│   │   ├── shell/# Shell
│   │   └── time/ # Timer
│   ├── arch/     # CPU/Arch specific code
│   ├── mm/       # Memory management (PMM, VMM, Heap, VMA)
│   ├── drivers/  # Hardware drivers (Net, USB, Audio, Video, Bus)
│   ├── net/      # Network stack
│   ├── fs/       # Filesystems (uniFS, FAT32)
```

## Coding Conventions

| Rule | Reason |
|------|--------|
| `-fno-exceptions` | Can't unwind stack in kernel |
| `-fno-rtti` | No `dynamic_cast` or `typeid` |
| `kstring::` not `std::` | Avoid libc dependencies |
| Named constants | Magic numbers are debugging nightmares |

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/core/kmain.cpp` | Kernel entry and initialization |
| `src/kernel/core/cpu.cpp` | CPU feature detection and setup (SSE, etc) |
| `src/kernel/core/irq.cpp` | Interrupt and exception routing |
| `src/kernel/core/gui.cpp` | Window manager and desktop |
| `src/kernel/sched/scheduler.cpp` | Process management, context switch |
| `src/mm/vmm.cpp` | Page table manipulation |
| `src/fs/fat32/fat32.cpp` | FAT32 filesystem driver |
| `src/drivers/video/framebuffer.cpp` | SSE-optimized renderer |
| `src/drivers/bus/usb/xhci.cpp` | USB 3.0 driver |

## Userland Development

uniOS supports running user-mode applications. While assembly examples exist in `usr/userspace/`, C++ development is the preferred way to build complex tools.

### System Calls

System calls are triggered via `int 0x80` on x86_64. Arguments are passed in registers:
- `RAX`: Syscall number (defined in `include/kernel/syscall.h`)
- `RDI, RSI, RDX`: Arguments 1, 2, and 3
- `RCX`: Argument 4 (if needed)

### Minimal C++ Example

```cpp
#include <stdint.h>

extern "C" void _start() {
    const char* msg = "Hello from Userland!\n";
    
    // sys_write(STDOUT, msg, len)
    asm volatile(
        "mov $1, %%rax\n"   // SYS_WRITE
        "mov $1, %%rdi\n"   // STDOUT_FD
        "mov %0, %%rsi\n"   // buffer
        "mov $21, %%rdx\n"  // length
        "int $0x80"
        : : "r"(msg) : "rax", "rdi", "rsi", "rdx"
    );

    // sys_exit(0)
    asm volatile(
        "mov $60, %%rax\n"  // SYS_EXIT
        "xor %%rdi, %%rdi\n"
        "int $0x80"
        : : : "rax", "rdi"
    );
}
```

### Compiling Userland

To compile for uniOS, use `x86_64-elf-gcc` with the following flags:
- `-ffreestanding`: No standard library
- `-fno-exceptions -fno-rtti`: No C++ runtime support
- `-nostdlib`: Don't link standard startup files
- `-T usr/userspace/user.ld`: Use the user-mode linker script
