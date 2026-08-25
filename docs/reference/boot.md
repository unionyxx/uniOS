# Boot Path

uniOS boots through Meridian, the in-tree x86-64 UEFI loader (`src/bootloader/`). Meridian loads the kernel and root filesystem image, selects a framebuffer mode, builds initial page tables, and hands off through the repo-owned `BootInfo` structure.

## Boot Sequence

1. UEFI firmware starts `EFI/BOOT/BOOTX64.EFI`.
2. Meridian opens the boot volume and reads `\KERNEL.ELF` and `\UNIFS.IMG` (1 MiB chunked reads).
3. Meridian selects a GOP framebuffer mode, guided by EDID when available.
4. Meridian parses the kernel ELF and allocates a zeroed image region at the link address.
5. Meridian locates the ACPI RSDP from the EFI configuration table.
6. Meridian builds the `BootInfo` payload and the initial page tables.
7. Meridian calls `ExitBootServices` (rebuilding the memory map and page tables on each retry, up to 4 attempts).
8. Meridian jumps to the kernel entry with the System V first argument (`rdi`) set to `BootInfo*`.

The loader logs only through the UEFI console (`ConOut`); it does not use the serial port. There is no boot command line: `BootModule.cmdline` is always null and the kernel has no cmdline consumer.

## Display Mode Selection

Meridian enumerates GOP modes and ranks them against an EDID hint:

- EDID is read from `EFI_EDID_ACTIVE_PROTOCOL` (falling back to `EFI_EDID_DISCOVERED_PROTOCOL`), validated by header magic and per-block checksums.
- Preferred timings come from base-block detailed timing descriptors, CTA-861 extension blocks (a fixed VIC table up to 3840x2160@120), and DisplayID blocks.
- Usable modes are nonzero-resolution modes with RGB8, BGR8, or bitmask pixel formats.
- Ranking: exact EDID hint match, then within-hint, then pixel count, aspect, width, height, current mode. There is no resolution cap; ultra-wide, 1440p, and 4K panels are allowed.

When the EDID hint matches the active mode exactly, Meridian publishes a `BootDisplayTiming` configuration table (36 bytes of timing data under a fixed GUID) that the kernel picks up via `boot_display_timing_init()`.

## Page Tables Built by the Loader

| Region | Mapping |
| --- | --- |
| All reported RAM below 128 TiB | Identity map **and** higher-half direct map at `hhdm_offset + phys` |
| Framebuffer | Same dual mapping |
| Kernel image | 4 KiB pages at its link address (`0xffffffff80000000` + offset), writable |

Large pages: 2 MiB pages are used only when the cursor is 2 MiB-aligned and at least 2 MiB remains; otherwise 4 KiB pages avoid over-mapping adjacent regions. No NX bits are set by the loader; the kernel applies protections later (`vmm_protect_kernel`).

The HHDM offset is `0xFFFF800000000000`. Page table sizing is estimated up front (unique PML4/PDPT/PT index counting) plus a 32-page margin.

## Kernel ELF Loading

Meridian validates the ELF header (ELF64, little-endian, `EM_X86_64`), bounds-checks the program header table with overflow-checked arithmetic, computes the page-aligned span of all `PT_LOAD` segments, allocates zeroed pages, copies file-backed bytes, and zeroes each BSS tail. The kernel is non-PIE (`mcmodel=kernel`) and runs at its link address — there are no runtime relocations.

## BootInfo ABI

`BootInfo` (`include/boot/boot_info.h`) is the bootloader-to-kernel ABI. It is repo-owned: change it on both sides in the same change. The kernel validates `magic`, `revision`, and the framebuffer pointer and halts otherwise.

- `BOOT_INFO_MAGIC` = `0x554E49424F4F5431` (the ASCII bytes `1TOOBINU`, i.e. `UNIBOOT1` reversed)
- `BOOT_INFO_REVISION` = 1

| Field | Meaning |
| --- | --- |
| `magic`, `revision`, `size` | Validation |
| `hhdm_offset` | Higher-half direct map offset |
| `kernel_physical_base`, `kernel_virtual_base` | Kernel placement (informational) |
| `framebuffer`, `framebuffer_count` | GOP framebuffer: address, width, height, pitch, bpp, masks, EDID blob, mode list |
| `firmware_type` | `BOOT_FIRMWARE_UEFI32` / `BOOT_FIRMWARE_UEFI64` |
| `rsdp_address` | Physical RSDP address |
| `efi_system_table_address` | Used to find the display-timing config table |
| `bootloader_name`, `bootloader_version` | `Meridian`, `0.1.0` |
| `modules`, `module_count` | One module: `unifs.img` at its HHDM address |
| `memory_map`, `memory_map_count` | Translated UEFI memory map (max 1024 entries) |

Memory types: `USABLE`, `RESERVED`, `ACPI_RECLAIMABLE`, `ACPI_NVS`, `BAD`, `BOOTLOADER_RECLAIMABLE` (loader code/data and boot-services regions), `KERNEL_AND_MODULES`, `FRAMEBUFFER`. The loader overlays the kernel image, module, and framebuffer ranges onto the translated map.

## Handoff

`handoff.asm` converts the Windows x64 ABI arguments to System V, loads the new CR3 and stack, clears `rbp`, aligns `rsp` to a call-site shape, and jumps to the entry point. Interrupts stay disabled; the kernel starts at CPL0 with the loader's page tables.

## Kernel Startup

The kernel entry point `_start` is a C++ function (`src/kernel/core/kmain.cpp`) — Meridian supplies the stack, CR3, and a fully placed image, so there is no BSP assembly startup. Initialization order:

1. `serial_init()` (COM1, 115200 8N1) — first statement, so early logs go somewhere.
2. CPU setup: CPUID feature detection, CR0/CR4/EFER, XSAVE setup, syscall MSRs (`STAR`, `LSTAR`, `SFMASK`), GS base to the BSP `PerCpu`.
3. Stack canary init (`__stack_chk_guard`), BootInfo validation.
4. GDT + TSS (per-core segment area with dedicated rsp0 and IST stacks for #DF, NMI, #PF), 256-entry IDT (vector `0x80` is a ring-3 syscall gate).
5. Framebuffer console and boot splash.
6. Memory: `vmm_early_init` (HHDM), `pmm_init` (bitmap + refcounts from the memory map), `vmm_init` (reuses loader page tables, pre-creates the MMIO window, registers the TLB-shootdown vector), `heap_init`.
7. C++ global constructors, PAT setup, kernel page protections (`.text` RX, `.rodata` RO+NX, `.data` RW+NX).
8. ACPI init (RSDP, FADT, MADT), PIC remap + mask, APIC init (LAPIC + IOAPIC, timer calibration).
9. Event queues and futex init, framebuffer remapped as write-combining, double buffering enabled, display init, scheduler init, BSP idle task.
10. Drivers: PS/2 keyboard/mouse, PIT fallback timer, PCI, display late init, RTC, USB + USB HID, USB storage settle, AHCI, ATA, partition scan.
11. Filesystems: VFS init, uniFS mounted as `/`, `/data` and `/vol` ensured, persistent `UNI_DATA` volume mounted (3 phases, see [Filesystems](filesystems.md)).
12. Debug builds: `ktest_run_all()`.
13. `smp_init()` — application processors are brought up (see [SMP](smp.md)).
14. Deferred tasks: `DeferredInit` (networking, sound, removable volume mounts) and `InitLaunch` (`kernel_exec("/bin/init.elf")`).
15. The BSP idle loop polls input and network between `hlt`s.

## Logging

Kernel logs go through `klog()`: a 16 KiB ring buffer, serial always, and the framebuffer console when enabled. Format: `[seconds.mmm] [MODULE] [level] message`.

- Debug builds enable framebuffer logging and verbose boot logs after `debug_init`.
- Release builds are quiet: only Warn and above pass.
- `dmesg` in the shell reads the kernel log buffer; `SYS_SET_QUIET` toggles framebuffer logging at runtime (the window manager silences it after the first frame).
