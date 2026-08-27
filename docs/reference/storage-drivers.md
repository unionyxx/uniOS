# Storage Drivers

Block devices register into a singly linked list (`src/fs/block_dev.cpp`) with name, model, display name, block size, total blocks, read/write callbacks, and an optional `flush` callback. Partitions are child devices named `<parent>p<index>` (e.g. `usb0p1`).

Write-back caching: drivers set `cache_dirty` when accepted data may still sit in the device's volatile write cache and no longer flush after every request. `block_dev_flush(dev)` issues the device flush command only when dirty; `block_dev_flush_all()` flushes every registered device and runs from `vfs_sync()` (so `SYS_SYNC`, reboot, and poweroff all commit caches before reset). Closing the last fd of a FAT32 file also flushes its backing device once the page cache purge completes.

Device names: `ata0`, `ahci0`..`ahci3`, `usb0`..`usb7`. Writes at every layer are gated by the storage guard (see [Filesystems](filesystems.md)).

## ATA (PIO)

`src/drivers/storage/ata.cpp` drives the primary channel (`0x1F0-0x1F7`) with 48-bit LBA PIO commands (READ/WRITE EXT, CACHE FLUSH EXT):

- IDENTIFY provides the model string and sector count (words 100-103, fallback 60-61).
- A global spinlock serializes the task file; floating buses (status `0xFF`) are detected and skipped.
- A 256-entry LRU sector cache with a 64-bucket hash and one-sector read-ahead reduces PIO traffic; `ata_cache_flush_all` runs on sync paths.

## AHCI (SATA DMA)

`src/drivers/storage/ahci.cpp` targets class 0x01 / subclass 0x06 / prog-if 0x01 and maps BAR5 (ABAR):

- Up to 4 controllers, 32 ports per controller, one command slot per port, polled mode (PCI interrupts deliberately disabled).
- Per-port command lists, FIS buffers, command tables, and a 64 KB bounce buffer come from `vmm_alloc_dma` (physically contiguous).
- Port bring-up performs COMRESET, waits for readiness, and filters signatures (ATAPI, SEMB, and port multipliers are skipped; the skip decision is exported for tests).
- Commands: IDENTIFY, READ DMA EXT, WRITE DMA EXT, FLUSH CACHE EXT; FIS Register H2D type `0x27`; 3-second command timeout with per-port recovery. FLUSH CACHE EXT runs through the block-device flush op (sync/shutdown/last close), not after every write.
- Firmware BIOS/OS handoff (BOH) is handled during controller init.

## USB Mass Storage

`src/drivers/storage/usb_msc.cpp` implements the Bulk-Only Transport over xHCI:

- CBW/CSW signatures `0x43425355` / `0x53425355`; class requests GET MAX LUN and Bulk-Only Mass Storage Reset.
- SCSI subset: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY (10), READ/WRITE (10), SYNCHRONIZE CACHE (10). SYNCHRONIZE CACHE is deferred to the block-device flush op instead of following every write.
- Up to 8 devices, transfers capped at 64 KB; block sizes accepted only when 512-4096 and power-of-two.
- After registration the partition table is rescanned; disconnects drain under a mutex.

## Partitions

`src/fs/partition.cpp` scans MBR and GPT (header CRC validated) on registration and creates child block devices. FAT32 volume labels are read into the device display name, which is what the `/data` discovery matches against (see [Filesystems](filesystems.md)).
