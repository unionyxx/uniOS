# Boot Images

The build produces three nested images: a FAT32 EFI system partition image (`efi.img`), a raw disk image (`boot.img`), and a UEFI-bootable ISO (`uniOS.iso`). All of them carry the same three payloads: `BOOTX64.EFI`, `KERNEL.ELF`, and `UNIFS.IMG`.

## efi.img — EFI System Partition

`tools/build_efi_image.py --mode fat` writes a FAT32 volume with a pure-Python writer.

- Contents: `EFI/BOOT/BOOTX64.EFI`, `KERNEL.ELF`, `UNIFS.IMG`.
- Size: `max(64 MiB, files + 4 MiB headroom)` rounded up to 1 MiB.
- Geometry: 512-byte sectors, 32 reserved sectors, FSInfo at sector 1, backup boot sector at 6, 2 FATs, root cluster 2, media descriptor `0xF8`.
- Label `UNI_OS`, volume serial `0x554E694F`, OEM name `uniOSEFI`.
- Cluster size is the first of 1/2/4/8/16/32/64 sectors that yields at least 65525 clusters.

## boot.img — Primary Disk Image

`--mode disk` wraps `efi.img` in an MBR-partitioned raw disk:

| Partition | Type | Contents |
| --- | --- | --- |
| 1 (LBA 2048, bootable) | `0x0C` FAT32 LBA | The ESP image; the BPB hidden-sectors field is patched to 2048 |
| 2 (aligned to 2048) | `0x0C` FAT32 LBA | Writable data volume labeled `UNI_DATA`, default 128 MiB |

The data partition is preserved byte-for-byte across rebuilds when an existing `boot.img` already contains a `UNI_DATA` partition of the same sector count, so settings and user files survive `meson compile`. `--no-data-partition` disables it.

For real hardware, write `boot.img` to a USB drive as a raw disk image. The kernel mounts the first suitable FAT32 volume at `/data` (see [Filesystems](filesystems.md)).

## uniOS.iso

`tools/create_iso.py` writes ISO9660 with 2048-byte sectors and a fixed layout (system area, PVD at sector 16, descriptors, path tables, boot catalog at 21, root directory, then file extents).

- El Torito boot catalog with platform ID `0xEF` (EFI); the boot image is the entire `efi.img`, which firmware mounts as the ESP.
- Visible tree: `/EFI/BOOT/BOOTX64.EFI;1`, `/KERNEL.ELF;1`, `/UNIFS.IMG;1`.
- Timestamps honor `SOURCE_DATE_EPOCH` for reproducible builds.
- The ISO has no `UNI_DATA` partition; persistence requires a reachable `UNI_DATA` volume on another disk.

## unifs.img — Root Filesystem

`tools/mkunifs.py` packs the staged build rootfs into the uniFS image format:

- 16-byte header: magic `UNIFS v1` + entry count.
- Up to 256 entries of 80 bytes: NUL-terminated path (max 63 bytes), absolute data offset, size. Directories are entries whose path ends in `/`.
- Entries are sorted for deterministic images; file data follows the table.

The staged rootfs is the authored `rootfs/` plus build overlays: every app ELF as `/bin/<name>.elf` and the generated `/usr/share/wallpapers/default.uowp`. The image is mounted read-only at `/`; writes are shadowed into a volatile RAM overlay. See [Filesystems](filesystems.md).

## ESP Boot Alone

`efi.img` can be booted directly from emulated USB media (`run-esp-usb` targets), which is useful for testing the removable-boot paths without a partition table.
