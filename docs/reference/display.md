# Display

The display path is built on the UEFI GOP framebuffer handed off by Meridian. There is no native GPU modesetting: the only backend is `FirmwareFramebuffer`, and the runtime resolution is fixed to the boot GOP mode. EDID-derived mode lists are reported through the display API, but a mode change succeeds only when it matches the active mode. The scanout refresh the firmware programmed is likewise fixed; EDID detection reports it, it cannot change it.

## Refresh Rate Detection

The kernel re-parses the EDID blob handed off in `BootFramebuffer` (`display_detect_refresh_millihz_from_edid`, `display_detect_modes_from_edid` in `src/drivers/video/display.cpp`):

- Timing sources: base-block detailed timing descriptors, standard timings, established timings I-III, CTA-861 video data block VICs (fixed table up to 3840x2160@120), HDMI 1.4 VSDB 4K2K VICs, CTA detailed descriptors, and DisplayID extensions (detailed types I/VII with the interlaced option bit, formula blocks). Monitor range limits are read including the EDID 1.4 +255 Hz offset flags.
- Tolerance: only the header magic is fatal. Blocks with corrupt checksums and blobs truncated below the declared extension count are still parsed (real-world high-refresh EDIDs frequently fail checksums), and duplicate timings from weaker sources are upgraded in place when a detailed descriptor later supplies exact blanking/sync geometry.
- Decision order for the active resolution: exact timing match (closest to the hint when one is supplied, otherwise the highest refresh, progressive preferred), then monitor range limits (30-510 Hz), then the firmware boot handoff, then a 60 Hz fallback.
- Mode lists (up to 32 per connector) emit detailed descriptors first so exact-geometry modes survive the cap; `DISPLAY_MODE_FLAG_PREFERRED` marks the highest-resolution/highest-refresh entry, `DISPLAY_MODE_FLAG_CURRENT` the entry matching the active mode, `DISPLAY_MODE_FLAG_EXACT_TIMING` entries with full blanking/sync data.

## Kernel 2D Engine

`src/drivers/video/framebuffer.cpp`:

- Double buffering: a RAM backbuffer holds drawing; `gfx_swap_buffers` copies dirty content to VRAM.
- Dirty tracking: up to 128 rectangles, merged when overlapping or touching; overflow sets a full-redraw flag.
- Copies use non-temporal stores where appropriate with store fences after VRAM writes.
- The boot splash and the kernel log console render through this path in debug builds.

## Display Abstraction

`src/drivers/video/display.cpp` models heads with backend ops (caps, status, present, present-buffer, wait, set mode, atomic commit, compose). `display_init` installs the boot framebuffer as buffer 0; `display_late_init` finds the PCI display device but keeps the firmware backend.

## Present API

The KMS-style syscall surface (`include/uapi/display.h`, syscalls 247-262; see [System calls](syscalls.md)):

- **Caps/status/connectors/modes**: geometry, pixel format, refresh (from EDID timing detection), capability flags (compositor present, copy path, `SYNCHRONOUS_PRESENT` — the firmware backend copies inline and finishes before the present call returns, so userspace may render directly into the presented buffer), connector and mode enumeration with full timing data. Status carries present-sequence tracking plus present telemetry: `last_present_pixels`, `last_vram_copy_ticks`, `total_present_pixels`, `total_vram_copy_ticks` (VRAM copy duration in timer ticks and pixels pushed, for write-bandwidth measurement).
- **Buffers**: create/destroy kernel DMA-backed buffers, map them into the caller at `0x340000000` (up to 16 objects), grant WM access.
- **Present**: copy-path present with caller-supplied damage rects (up to 128), `sfence` semantics, present-sequence tracking.
- **Compose**: the compositor submits up to 32 layers with up to 32 damage rects; the kernel composites into DMA compose buffers (up to 3) and presents. Layers carry blend/scale/cursor parameters. Partial CPU composes seed the new buffer from what is already on screen: they prefer the most recent compose slot that still mirrors the display (plain RAM) and only fall back to reading the VRAM front buffer before any compose has landed, avoiding a PCIe readback stall on every partial frame.
- **Events**: vblank, flip complete, hotplug — waited on with `SYS_DISPLAY_EVENT_WAIT`, carrying the frame sequence and timestamps. The wait argument selects the mode: `0` polls, `UINT64_MAX` blocks indefinitely, any other value is a timeout in milliseconds. Frame pacing falls back to a deadline timer when no vblank source exists; the wait coarse-sleeps to one tick before the deadline and then spins on the tick counter so the frame lands on the deadline rather than a full scheduler round late.
- **Atomic commit / set mode**: accepted but effectively no-ops unless the requested mode matches the active one.

Legacy framebuffer syscalls (208-214) expose the raw framebuffer: info, WC mapping at `0x200000000`, flush, full and rect blits.

## Who Uses What

- The window manager is the only compositor client; it uses compose submit with damage rectangles, a hardware cursor when the backend supports page flips, and the copy path otherwise. See [Window manager](wm.md).
- Desktop applications never touch the display API directly — they draw into memfd-backed window buffers that the WM composites.
