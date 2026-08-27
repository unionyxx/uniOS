# Filesystems

uniOS mounts boot content from `unifs.img` at `/` and persistent data from a FAT32 volume at `/data`. Removable volumes mount under `/vol`.

## uniFS — System Root

`unifs.img` is a flat read-only image (see [Boot images](images.md) for the layout). The kernel validates the header and every entry range against the image size at mount time and refuses corrupt images; `\` is normalized to `/` in entry names. Reads are direct `memcpy`s from the image at its module address.

### RAM Overlay

The boot image is never written. Writes are shadowed into a volatile RAM overlay:

- Up to 64 RAM files, 1 MiB each, names up to 63 bytes.
- Writing or truncating a boot file copies it to RAM first (copy-on-write from image data).
- Delete removes only the RAM copy — a boot file cannot be removed, only shadowed by a RAM entry of the same name.
- `mkdir` creates a RAM directory entry; directories are implicit path prefixes in both tables.
- Readdir merges boot entries (minus shadowed ones) with RAM files.

Nothing in the overlay survives reboot. Persistent state belongs on `/data`.

## FAT32 — Persistent Data

`src/fs/fat32/fat32.cpp` implements FAT32 with long filenames:

- Boot sector parsing accepts 512 or 4096-byte sectors, power-of-two cluster sizes up to 128 sectors, and validates all geometry in 64-bit arithmetic so crafted values cannot wrap.
- FSInfo is validated by its three signatures and provides the free-cluster count and next-free-cluster hint; allocation starts at the hint and wraps, frees update it, and the in-memory state is marked dirty. The FSInfo sector itself is rewritten lazily by the filesystem sync op (`vfs_sync()`), not on every allocation/free.
- A per-filesystem spinlock serializes every FAT/directory mutation and the read-modify-write walks behind it.
- All cluster-chain walks (lookup, readdir, read, free) are bounded by the cluster count, so cyclic chains on corrupt media terminate. Out-of-range FAT entries are treated as EOF.
- LFN: up to 20 slots per name, checksum verified before use, UTF-16 to UTF-8 conversion rejecting surrogates, fallback to the 8.3 short name. Creation generates `~1`..`~999` short-name suffixes when needed.
- Writes bypass read-modify-write for aligned full sectors and coalesce physically contiguous cluster runs into one multi-sector transfer; only head/tail partial sectors take a read-modify-write. Clusters are allocated as needed. Truncate supports size 0 only. All file I/O flows through the VFS page cache.

## /data Mount Discovery

Labels: the boot ESP is `UNI_OS`; the data volume is `UNI_DATA`. After mounting `/`, the kernel ensures `/data` and `/vol` exist (in the RAM overlay) and mounts the persistent volume in three phases, each scanning partitions first and then whole disks:

1. **Phase 0**: FAT32 volume labeled exactly `UNI_DATA`.
2. **Phase 1**: any FAT32 volume except the boot label `UNI_OS`.
3. **Phase 2**: even the boot FAT32 volume (logged as a fallback).

If the first attempt fails, late USB storage is settled (poll bursts + partition rescan) and the mount retries; the deferred boot task retries once more. With nothing mounted, settings become session-only (writes land in the volatile RAM overlay).

The `/data` mount is storage-guarded (see below) and registered with the `SYSTEM_DATA` volume flag. Removable non-data volumes mount read-write at `/vol/<label>` with collision suffixes.

## Storage Guard

A global storage access mode gates guarded mounts (`src/fs/storage_guard.cpp`):

| Mode | Effect |
| --- | --- |
| `OFF` (0) | Guarded devices hidden from the volume list |
| `READ_ONLY` (1) | Reads allowed, writes refused |
| `WRITABLE` (2, default) | Full access |

Guarded opens re-check the mode on every read/write/readdir through descriptor flags. `SYS_STORAGE_SET_MODE` is restricted to the window manager; the Preferences app requests changes through the shared registry, and the WM applies them (prompting the user when storage was disabled).

## User Database Persistence

`/etc/passwd` and `/etc/shadow` are parsed in-kernel; changes are synced to `/data/etc/` and restored from there at boot when present.
