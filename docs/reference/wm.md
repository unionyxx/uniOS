# Window Manager

The window manager (`/bin/wm.elf`, `src/usr/wm/`) is a userspace compositor process. The kernel provides present/compose syscalls and event plumbing; the WM owns window metadata, decorations, focus, overlays, the cursor, and every frame submitted to the display.

## Module Layout

Headers are layered: `wm_types.h` (POD structs/constants) -> `wm_rect.h` (rect math) -> subsystem headers (`wm_metrics.h`, `wm_damage.h`, `wm_window.h`, `wm_input.h`, `wm_render.h`, `wm_settings.h`) -> pipeline/feature headers (`wm_present.h`, `wm_overlays.h`, `wm_main.h`). Pure rect/queue/present-policy math shared with the kernel ktest suite lives in `include/wm/interaction_policy.h`.

| Module | Contents |
| --- | --- |
| `wm_main.cpp` | `main()`: bootstrap + per-frame pipeline orchestration (no shared state definitions) |
| `wm_bootstrap.cpp` | Registry allocation, swapchain setup, shell window seeding, first frame, registry publish |
| `wm_events.cpp` | Event pump and dispatch (pointer/keyboard/scroll, overlay modal handling) |
| `wm_registry_sync.cpp` | Settings/storage/wallpaper/notification generation consumption |
| `wm_adopt.cpp` | Window adoption scan, failure tombstones, dead-owner reaping |
| `wm_commit.cpp` | Entry sampling, buffer remap/validation, metadata sync, damage pop |
| `wm_frame_build.cpp` | Dirty-set normalization, overlay damage expansion, compose pass, scene alias state |
| `wm_present.cpp` | Present slots, stale tracking, display events, submit/idle/wait policy, frame sequences, scene/display globals |
| `wm_cursor.cpp` | Hardware cursor planes, software-cursor damage bookkeeping |
| `wm_stats.cpp` | TSC timing, frame stats, benchmark driver, stats overlay drawing |
| `wm_damage.cpp` | Dirty-rect queue (enqueue/normalize/clip/collapse), dirty-set queries |
| `wm_window_geometry.cpp`, `wm_window_cache.cpp`, `wm_window_stack.cpp`, `wm_window_bounds.cpp`, `wm_window_resize.cpp`, `wm_window_scroll.cpp`, `wm_window_damage.cpp` | Window bounds/visibility predicates, visibility caches, focus/z-order/lifecycle, move/snap, synchronous resize protocol, content scrolling, damage marking |
| `wm_hit_test.cpp`, `wm_client_events.cpp`, `wm_pointer.cpp` | Hit testing, client event delivery, move/hover/cursor-kind tracking |
| `wm_pixel.cpp`, `wm_wallpaper.cpp`, `wm_shell_blur.cpp`, `wm_decoration.cpp`, `wm_client_draw.cpp`, `wm_blur.cpp`, `wm_compose.cpp` | Pixel math/SIMD blits, wallpaper + desktop base, menubar/dock blur, decorations, client blit, blur kernels, rect composition |
| `wm_index.cpp`, `wm_control_center.cpp`, `wm_context_menu.cpp`, `wm_storage_prompt.cpp`, `wm_notifications.cpp`, `wm_actions.cpp` | Overlay features: each owns its state, logic, and clipped drawing |
| `wm_settings.cpp`, `wm_metrics.cpp` | Runtime settings load/persist, scaled metrics |

## Shared Registry

The protocol is a shared-memory `Registry` (`include/uapi/gui.h`) — no sockets or message passing:

1. The WM allocates the first shared-memory block at boot (id 0), maps it, and calls `SYS_GUI_REGISTER_WM` (which records the WM pid and boosts its priority).
2. Clients map block 0 and spin until `magic == REGISTRY_MAGIC` (`0x52454749`).

The registry carries: mouse state, menubar/dock canvas block ids and click flags (plus `cp_toggle_requested`, which any shell component can set to make the WM toggle the control center), focus (window index + owner pid), theme mode, settings generation + system flags, network/animation/transparency/volume settings, storage mode + request generation, wallpaper generation/status/requested/active paths, a toast-notification request slot (`notify_title`/`notify_message` + `notify_generation`, written by `gui_notify` and consumed once per generation by the WM), an app open-request slot (`open_path` + `open_generation`, written by `gui_open_request_submit` before a launcher fork/execs a viewer and taken once at startup by `gui_open_request_take`, which clears the generation; single slot, a new request overwrites a pending one), the focused app's published `MenuModel` (`menu_model`, seq-stability protocol, rendered by the menubar), a shared text clipboard (`clipboard` + `clipboard_len` + `clipboard_seq`), window count, and 32 `WindowEntry` slots. Each `WindowEntry` also carries menu dispatch fields (`menu_command_id` + `menu_command_seq`) written by the menubar and polled by the owning app.

## Windows

Slots 0 and 1 are reserved for the menubar and dock; user windows take slots 2-31 (`MAX_WINDOWS = 32`).

**Registration (client side).** `gui_register_window_ex`:

1. Claims a slot with an atomic CAS on `shm_id`.
2. Creates backing: `memfd_create` + `ftruncate(w*h*4)` + `mmap(MAP_SHARED)`.
3. Transfers the memfd to the WM with `SYS_FD_TRANSFER` (pid 0 resolves to the WM pid).
4. Publishes `shm_id = wm_fd | 0x40000000` (memfd tag), owner pid, geometry, generation counters, then sets `ready` behind store fences.

**Adoption (WM side).** Each frame the WM scans ready entries and adopts valid ones: it validates the buffer size against `SYS_FSIZE` (a memfd mapped past EOF would fault the compositor), clamps dimensions, and maps. Adoption failures leave tombstones (keyed by `shm_id + owner_pid`, 5000-tick expiry) to stop retry storms. An unadopted entry whose owner is already dead (crash, kill) is reset in place — `ready` cleared, slot returned to `WIN_SHM_INVALID`, damage zeroed, tombstone dropped — so it stops burning syscalls and cannot poison the adoption key of a relaunched app that reuses the same fd and pid.

**Validation hardening.** Window entries are sampled twice across load fences and accepted only when bitwise stable (bounded retries). Buffer dimensions are clamped to 8192. Dimension growth is re-validated against `SYS_FSIZE`/`SYS_SHM_INFO` and forced back on violations; SysV blocks must be owned by the entry's owner pid. Titles are copied with forced NUL termination. Liveness is checked with `kill(pid, 0)` every 30 frames; dead owners' windows are closed without killing recycled pids.

## Resize Protocol

For resizable windows:

1. WM publishes the target geometry into the entry, writes `resize_serial`, and posts an `EVT_WINDOW_RESIZE` event with the size and serial. Only one configure is outstanding per window at a time: while one is pending the newest pointer target is queued and published right after the ack lands, so the client is never asked for two sizes at once.
2. Client grows its backing in place when needed (new memfd + transfer; capacity grows with slack, so a drag reallocates only occasionally) and redraws. Growth swaps the shared window surface's buffer pointer, so clients must re-read it before drawing — a cached `Surface` copy keeps pointing at the unmapped old backing and faults on the next fill.
3. Client acks by publishing `buffer_resize_serial` — the exact serial it synced to and rendered, not the newest serial in the entry, which a fast drag may have advanced past the frame just drawn. The WM resends the outstanding configure if the client goes quiet.
4. Retired buffers are released when the WM's acknowledge generation reaches theirs.

Resize is synchronous and flip-based. The visible bounds never move ahead of what the client has drawn: when a configure is posted the WM records the pending geometry and keeps the window at its last committed size, presenting a WM-owned snapshot of the last committed frame (resize_snapshot, captured at configure post and on each ack, freed on close) because the client is actively overwriting the shared backing during the redraw window. When the client acks, the WM flips the bounds to the pending geometry in one step so backing and bounds land in the same frame, then publishes the next queued target if the pointer moved on. Content is therefore only ever blitted 1:1 - never scaled, stale, or partially drawn - and the window follows the pointer at the client render rate, like a toolkit live resize. The same path serves interactive edge-drags, maximize, tiling, and client-requested resizes. There is no stretch renderer and no committed-size tracking; the blit clamps to the mapped backing and scrolled (content-sized) windows read their visible slice via the scroll offset. Shell surfaces (menubar, dock) bypass the configure protocol and redraw their whole entry, so their size tracks the registry geometry directly; this lets the menubar grow its window to expose the system dropdown.

## Focus and Z-Order

Z-order is the window array order; focusing raises. `SYS_GUI_SET_FOCUS` (WM-only) records the focused pid and boosts its scheduling priority. Alt+Tab cycles visible user windows; the mouse wheel over empty desktop rotates stacking; titlebar double-click toggles maximize (ABA-guarded).

On every focus transition the WM posts `EVT_FOCUS`/`EVT_UNFOCUS` to the affected owners, posts `EVT_MOUSE_LEAVE` when the pointer leaves a client area (or a drag/overlay takes over), and posts `EVT_WINDOW_SCROLL` when it scrolls a window's content. A mouse-down delivered to a client grabs the pointer for that window until the matching release, so moves and the release reach it even outside the frame. See [Input](input.md#client-lifecycle-events).

## Damage and Composition

- Clients push damage rectangles into a lock-free SPSC ring (8 entries) per window.
- The WM pops damage, offsets it into screen space, intersects it with visible bounds, and feeds the dirty-rect queue (128 rects). Normalization clips, merges touching/overlapping rects, applies a union heuristic, and collapses to bounds when the interactive/non-interactive limit is exceeded.
- Per dirty rect, composition paints bottom-up: wallpaper (skipped when an opaque window covers the rect), user windows with decorations, system windows (menubar, dock), overlays (context menus, storage prompt, launcher, control center, toasts), then the cursor.
- Frames submit via `SYS_DISPLAY_COMPOSE_SUBMIT` (single opaque layer + damage + frame sequence + vblank present + hardware cursor) or the copy path. Up to 2 presents are in flight across 3 buffer slots; completion is tracked from display events, and present policy (submit/wait/skip) gates the frame loop. Idle frames sleep ~16 ms.
- Identity-alias mode: on copy-path backends advertising `DISPLAY_FLAG_SYNCHRONOUS_PRESENT` the scene buffer is created as the single present buffer, so compositing draws directly into the buffer the kernel copies to VRAM and the scene→present blit is skipped. The software cursor then bakes into the scene; each frame prepends the previous baked cursor rect to the damage set (composed before any window-move pixel shifts) and re-bakes the cursor after compositing. Any allocation failure falls back to the two-buffer path.
- The cursor uses a hardware cursor when the compositor backend supports page flips, otherwise a software cursor rendered into the present buffer. Cursor movement only adds damage on the software path — the hardware plane draws out of band, so damaging every move would inflate the dirty set (and trip the resize collapse heuristic).

## Wallpaper, Blur, Overlays

- Wallpaper: the `.uowp` path comes from `/data/WALLPAPR.CFG` (fallback `/etc/wallpaper.conf` or the registry request); the theme variant is loaded, scaled cover-style to the screen, and published in the registry. Default: `/usr/share/wallpapers/default.uowp`.
- Blur: the WM owns menubar/dock blur surfaces (two-pass box blur or material blur), regenerated on backdrop changes; disabled on copy-path backends.
- Overlays: the Index launcher (app catalog + system actions, fuzzy scoring), control center, right-click menus, storage mode prompt, and toast notifications (4 s, 32-slot ring).

## Debug Telemetry and Benchmarking

The WM keeps a `WmFrameStats` block updated during the frame loop: built/submitted/skipped frame counts, dirty rect count and area, full vs clipped repaints, stale-slot repairs, cursor path counts, and TSC-timed compose/present/frame durations plus input-to-submit latency.

- `SYSTEM_FLAG_SHOW_DEBUG_STATS` renders a mono-font overlay (below the menubar, left) showing last/max frame time, compose/present ms, input-to-submit latency, damage kpx + rect count, and the kernel's last VRAM copy pixels/ticks from `DisplayStatus`.
- `SYSTEM_FLAG_WM_BENCH_DRAG` / `SYSTEM_FLAG_WM_BENCH_RESIZE` run a scripted 1000-frame benchmark: the WM drives synthetic window translation (drag) or geometry oscillation (resize) on the first visible user window, measures compose/present/frame costs and dirty area, logs a summary line, restores the window, and clears the flag. The resize mode routes every step through `set_window_bounds`, so it exercises the real synchronous configure/ack/flip pipeline; the summary additionally reports `resize flips` (ack-driven geometry flips) and `stale` (acks for superseded configures) counts. No manual input is needed, so deltas are reproducible on hardware.
- `SYSTEM_FLAG_WM_PIXEL_SELFTEST` runs the libgui SIMD pixel-op self-test at startup and logs PASS/FAIL. Debug builds always run it at boot. See [Userspace runtime](userspace.md#libgui).

## Settings

The WM loads settings from `/data/SYSTEM.CFG` (fallback `/etc/system.conf`), applies them, and persists changes during idle frames. Settings propagate live to clients through the registry's `settings_generation` counter. See [Runtime configuration](config.md).
