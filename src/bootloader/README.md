# Meridian Bootloader

Meridian is the in-tree x86-64 UEFI bootloader for uniOS.

## Boot Path

1.  **Entry**: Firmware loads `EFI/BOOT/BOOTX64.EFI` from the EFI system partition.
2.  **Loading**: Meridian loads `kernel.elf` and `unifs.img` into memory.
3.  **Graphics**: Selects a GOP framebuffer mode, prioritizing EDID preferred timings if available.
4.  **Memory Map**: Retrieves the final UEFI memory map.
5.  **Paging**: Sets up initial higher-half identity and kernel page tables.
6.  **Handoff**: Builds the uniOS `BootInfo` structure.
7.  **Exit**: Exits UEFI boot services and jumps to the kernel entry point.

## Filesystem Layout

The EFI system partition must contain:

- `EFI/BOOT/BOOTX64.EFI`: This loader binary.
- `kernel.elf`: The uniOS kernel.
- `unifs.img`: The initial system root filesystem.

## Handoff Model

The kernel receives a single `BootInfo *` pointer. This structure is repository-owned and defines the contract between the bootloader and the kernel.

`BootInfo` includes:
- Final memory map.
- Framebuffer address and resolution.
- Loaded modules (e.g., `unifs.img`).
- Firmware vendor and version metadata.

## Display Mode Selection

Meridian implements a robust GOP mode selection algorithm:
- Preferred timings from **EDID** blocks.
- **CTA** and **DisplayID** timing extension data.
- Fallback scoring based on resolution, bit-depth, and pixel format.
