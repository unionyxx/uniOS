# Window Manager

The window manager (`/bin/wm.elf`, `src/usr/wm/`) is a userspace compositor process. The kernel provides present/compose syscalls and event plumbing; the WM owns window metadata, decorations, focus, overlays, the cursor, and every frame submitted to the display.

## Shared Registry

The protocol is a shared-memory `Registry` (`include/uapi/gui.h`) — no sockets or message passing:

1. The WM allocates the first shared-memory block at boot (id 0), maps it, and calls `SYS_GUI_REGISTER_WM` (which records the WM pid and boosts its priority).
2. Clients map block 0 and spin until `magic == REGISTRY_MAGIC` (`0x52454749`).

The registry carries: mouse state, menubar/dock canvas block ids and click flags (plus `cp_toggle_requested`, which any shell component can set to make the WM toggle the control center), focus (window index + owner pid), theme mode, settings generation + system flags, network/animation/transparency/volume settings, storage mode + request generation, wallpaper generation/status/requested/active paths, window count, and 32 `WindowEntry` slots.

## Windows

Slots 0 and 1 are reserved for the menubar and dock; user windows take slots 2-31 (`MAX_WINDOWS = 32`).

**Registration (client side).** `gui_register_window_ex`:

1. Claims a slot with an atomic CAS on `shm_id`.
2. Creates backing: `memfd_create` + `ftruncate(w*h*4)` + `mmap(MAP_SHARED)`.
3. Transfers the memfd to the WM with `SYS_FD_TRANSFER` (pid 0 resolves to the WM pid).
4. Publishes `shm_id = wm_fd | 0x40000000` (memfd tag), owner pid, geometry, generation counters, then sets `ready` behind store fences.

**Adoption (WM side).** Each frame the WM scans ready entries and adopts valid ones: it validates the buffer size against `SYS_FSIZE` (a memfd mapped past EOF would fault the compositor), clamps dimensions, and maps. Adoption failures leave tombstones (keyed by `shm_id + owner_pid`, 5000-tick expiry) to stop retry storms.

**Validation hardening.** Window entries are sampled twice across load fences and accepted only when bitwise stable (bounded retries). Buffer dimensions are clamped to 8192. Dimension growth is re-validated against `SYS_FSIZE`/`SYS_SHM_INFO` and forced back on violations; SysV blocks must be owned by the entry's owner pid. Titles are copied with forced NUL termination. Liveness is checked with `kill(pid, 0)` every 30 frames; dead owners' windows are closed without killing recycled pids.

## Resize Protocol

For resizable windows:

1. WM writes `resize_serial` into the entry and posts an `EVT_WINDOW_RESIZE` event with the new size and serial.
2. Client grows its backing (new memfd + transfer; capacity grows with slack) and redraws.
3. Client publishes `buffer_resize_serial`; the WM retries the configure until serials match.
4. Retired buffers are released when the WM's acknowledge generation reaches theirs.

While a resize is in flight the WM geometry changes before the client commits, so the compositor tracks each window's last committed content size (`client_committed_w/h`, updated on adoption and every resize ack) and only blits that region — never the slack pixels of an over-allocated backing. During an interactive edge/corner drag the committed content is anchored to the corner opposite the dragged edges (drag left: content pins right), and the anchor survives pointer release until the outstanding configure is acknowledged, so content never slides with the grip or snaps on release. Programmatic resizes (maximize/restore) clear the anchor.

## Focus and Z-Order

Z-order is the window array order; focusing raises. `SYS_GUI_SET_FOCUS` (WM-only) records the focused pid and boosts its scheduling priority. Alt+Tab cycles visible user windows; the mouse wheel over empty desktop rotates stacking; titlebar double-click toggles maximize (ABA-guarded).

On every focus transition the WM posts `EVT_FOCUS`/`EVT_UNFOCUS` to the affected owners, posts `EVT_MOUSE_LEAVE` when the pointer leaves a client area (or a drag/overlay takes over), and posts `EVT_WINDOW_SCROLL` when it scrolls a window's content. A mouse-down delivered to a client grabs the pointer for that window until the matching release, so moves and the release reach it even outside the frame. See [Input](input.md#client-lifecycle-events).

## Damage and Composition

- Clients push damage rectangles into a lock-free SPSC ring (8 entries) per window.
- The WM pops damage, offsets it into screen space, intersects it with visible bounds, and feeds the dirty-rect queue (128 rects). Normalization clips, merges touching/overlapping rects, applies a union heuristic, and collapses to bounds when the interactive/non-interactive limit is exceeded.
- Per dirty rect, composition paints bottom-up: wallpaper (skipped when an opaque window covers the rect), user windows with decorations, system windows (menubar, dock), overlays (context menus, storage prompt, launcher, control center, toasts), then the cursor.
- Frames submit via `SYS_DISPLAY_COMPOSE_SUBMIT` (single opaque layer + damage + frame sequence + vblank present + hardware cursor) or the copy path. Up to 2 presents are in flight across 3 buffer slots; completion is tracked from display events, and present policy (submit/wait/skip) gates the frame loop. Idle frames sleep ~16 ms.
- The cursor uses a hardware cursor when the compositor backend supports page flips, otherwise a software cursor rendered into the present buffer. Cursor movement only adds damage on the software path — the hardware plane draws out of band, so damaging every move would inflate the dirty set (and trip the resize collapse heuristic).

## Wallpaper, Blur, Overlays

- Wallpaper: the `.uowp` path comes from `/data/WALLPAPR.CFG` (fallback `/etc/wallpaper.conf` or the registry request); the theme variant is loaded, scaled cover-style to the screen, and published in the registry. Default: `/usr/share/wallpapers/default.uowp`.
- Blur: the WM owns menubar/dock blur surfaces (two-pass box blur or material blur), regenerated on backdrop changes; disabled on copy-path backends.
- Overlays: the Index launcher (app catalog + system actions, fuzzy scoring), control center, right-click menus, storage mode prompt, and toast notifications (4 s, 32-slot ring).

## Settings

The WM loads settings from `/data/SYSTEM.CFG` (fallback `/etc/system.conf`), applies them, and persists changes during idle frames. Settings propagate live to clients through the registry's `settings_generation` counter. See [Runtime configuration](config.md).
