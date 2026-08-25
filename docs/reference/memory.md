# Memory Management

Memory management lives in `src/mm/` and `src/arch/x86_64/mm/`. The kernel runs in the higher half; user processes occupy isolated lower-half address spaces.

## Address Space Layout

| Region | Address | Purpose |
| --- | --- | --- |
| Kernel image | `0xffffffff80000000` | Link base (`linker.ld`) |
| HHDM direct map | `0xFFFF800000000000 + phys` | All physical memory below 128 TiB |
| MMIO/DMA window | `0xFFFFFE0000000000 .. 0xFFFFFF800000000000` | Bump-allocated device mappings |
| Kernel stack top | `0xFFFFFF8000000000` | Kernel stacks grow down from here |
| User/kernel split | `0x0000800000000000` | User space is the canonical lower half |
| Anonymous mmap base | `0x100000000` | First-fit search for `SYS_MMAP` |
| Framebuffer mapping | `0x200000000` | Fixed WC mapping (`SYS_FB_MMAP`) |
| Shared memory region | `0x300000000` | 64 slots x 16 MiB (`SYS_SHM_MAP`) |
| Display buffers | `0x340000000` | Up to 16 buffer objects |
| User stack top | `0x0000700000000000` | 8 pages (32 KiB) per process |

## Physical Memory Manager (PMM)

`src/mm/pmm.cpp` is a bitmap allocator with per-frame reference counts, both allocated from the first fitting `USABLE` region of the boot memory map. One global spinlock (irqsave) serializes everything.

- 4 KiB frames, 1 bit per frame (set = used), plus a `uint16_t` refcount per frame.
- All frames start reserved; `USABLE` ranges are freed, then the bitmap/refcount frames and frame 0 are re-reserved.
- `pmm_alloc_frame()` returns a **zeroed** physical frame (zeroing happens through the HHDM mapping).
- `pmm_alloc_frames(count)` allocates physically contiguous frames from the top of memory downward, keeping low RAM free for DMA-sensitive uses.
- Refcounts power copy-on-write and shared mappings: `pmm_refcount_inc` panics on a free frame; `pmm_refcount_dec` frees at zero; invalid frees are reported, not silent.
- Stats: `pmm_get_free_memory()`, `pmm_get_total_memory()` feed the `mem` shell command and `SYS_GETMEMINFO`.

## Virtual Memory Manager (VMM)

`src/mm/vmm.cpp` implements classic 4-level paging (PML4 → PDPT → PD → PT). Intermediate tables are allocated with atomic compare-and-swap; a racing loser adopts the winner's frame. Encountered 1 GiB/2 MiB leaves are split on demand.

PTE flags: `PRESENT`, `WRITABLE`, `USER`, `PWT`, `PCD`, `NX`, and the software bit `PTE_SHARED` (bit 52: shared page, no copy-on-write). Cache types:

- `PTE_MMIO` = present, writable, PCD+PWT, NX (strong UC under the default PAT).
- `PTE_WC` = present, writable, PCD, NX with PAT index 2 programmed to WC (per-core PAT setup; APs must run `pat_init` or WC reads back as UC).

Key operations:

- `vmm_create_address_space`: fresh PML4 with kernel entries 256-511 copied.
- `vmm_clone_address_space` (fork): kernel half shared, user half deep-copied with leaf refcount bumps; writable non-shared pages are write-protected in **both** spaces (copy-on-write).
- `vmm_map_mmio` / `vmm_alloc_dma`: mappings from the MMIO/DMA window with batched invalidation. `vmm_free_dma` performs a full TLB shootdown **before** frames return to the pool, so no remote core can keep DMAing into a reused frame.
- `vmm_remap_framebuffer`: replaces the loader's WB mappings in place with WC+shared.
- `vmm_protect_kernel`: `.text` read+execute, `.rodata`/`.requests` read-only+NX, `.data` through `__kernel_end` writable+NX.
- `vmm_handle_page_fault`: demand paging for VMA-covered addresses, copy-on-write resolution (copy when refcount > 1, in-place permission upgrade otherwise), shared-page flag fixup.

There is no PCID/INVPCID; local flushes are `invlpg` loops (> 32 pages: CR3 reload).

## TLB Shootdowns

Cross-core invalidation uses an IPI with monotonic sequence acknowledgements (`src/mm/vmm.cpp`):

- The sender takes the shootdown lock, bumps a global sequence, enqueues `{address, pages, seq}` into a per-CPU FIFO ring (16 entries) on every other online CPU, sends one IPI per target, and releases the lock before waiting.
- Targets drain their ring in order, flush locally, and publish `tlb_ack_sequence` per request with release semantics.
- The sender polls acknowledgements for up to 40 rounds of bounded pause-loops, re-IPI-ing lagging CPUs between rounds, and panics on timeout. Enqueue waits on a full ring are also budgeted and panic rather than spin forever.
- Fresh APs copy the current sequence before publishing themselves online, so they never owe acknowledgements for earlier invalidations.
- Requests are never overwritten before acknowledgement, so late IPIs cannot be misread.

## Kernel Heap

`src/mm/heap.cpp` is a bucketed allocator protected by one global spinlock:

- 8 buckets: 32, 64, 128, 256, 512, 1024, 2048, 4096 bytes. Free blocks live in per-bucket lists; slab pages are tracked in an open-addressed table (65536 slots). When a page's blocks are all free, the page returns to the PMM.
- Allocations carry a header with size and magic (`0xC0FFEE1234567890`); `free` and `realloc` validate it.
- Requests larger than one page go straight to contiguous PMM frames at their HHDM address.
- `heap_init` only zeroes metadata; the heap grows on demand. Global `operator new`/`delete` forward to it.
- `heap_dump_stats()` walks the free lists and tracked-page table for debugging.
