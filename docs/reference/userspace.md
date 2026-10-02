# Userspace Runtime

Userspace programs are native ELF binaries under `src/usr/`, linked at `0x400000` by `src/usr/user.ld`, and staged into the root filesystem as `/bin/<name>.elf`.

## crt0 and Program Startup

`src/usr/libc/crt0.asm`:

1. Aligns the stack to 16 bytes and clears `rbp`.
2. Runs `__init_array` global constructors.
3. Calls `main()` with no arguments (no argc/argv/envp or aux vector is delivered yet).
4. Calls `exit(retval)`; a hang loop catches returns.

It also exports `__sigret` (`SYS_SIGRETURN` trampoline), which libc installs as the signal restorer.

## libc Subset

`src/usr/libc/` is a freestanding C/C++ subset:

- **Syscalls**: inline wrappers `syscall0..syscall6` using the System V syscall ABI (number in `rax`, args `rdi/rsi/rdx/r10/r8/r9`). Wrappers exist for the whole syscall surface — files, processes, memory, display, events, sound, shm, network, extended calls (futex/threads/epoll/memfd/mprotect). Time helpers: `get_ticks`/`get_uptime`/`get_tsc_freq`, `sleep_ms` (a real `SYS_SLEEP_MS` sleep), and `sleep_until_ticks` for deadline-paced animation loops.
- **string**: `strlen/strcpy/strcat/strncpy/strncat/strcmp/strncmp/strchr/strrchr/strstr/strtok` (single global strtok state), `memset/memcpy/memmove` with SSE2 16-byte aligned-store fast paths (forward and backward), `memcmp`, `itoa` (base 2-36, INT64_MIN-safe).
- **stdio**: `printf/sprintf/snprintf/vsnprintf` into a 4 KiB stack buffer with raw fd writes — supports `%s %d %i %u %o %x %X %p %c`, `-`/`0` flags, widths, `l`/`ll`. No floats, no `FILE*`, no buffering.
- **stdlib**: a region allocator over `SYS_MMAP` — 64 KiB regions with magic-validated block lists, first-fit + split, coalescing free, fully-free regions unmapped, dedicated blocks above 32 KiB; `calloc` with overflow checks; `realloc` with in-place growth; `atoi`; a simple LCG `rand`.
- **socket**: POSIX-shaped wrappers over the network syscalls plus byte-order helpers.
- **wav**: userspace RIFF/WAV parser used by the shell `play` command.
- **log**: leveled `[sec.mmm] scope [mark] message` logging to stdout.
- **config_utils**: `key=value` config readers/writers used for `SYSTEM.CFG` and wallpaper settings, including `cfg_write_text_file_atomic` (write-temp-then-rename; FAT32 rename refuses to overwrite, so the destination is unlinked first).
- **math**: freestanding `sqrt/sin/cos/tan/fabs/fmod` (plus `f` variants) — bit-seeded Newton sqrt (machine epsilon across the full double range) and range-reduced Taylor trig, so apps never hand-roll math.
- **cxx.cpp**: global `new`/`delete` over malloc, a `__cxa_pure_virtual` fault handler, and the static-object runtime glue (`__cxa_guard_acquire/release/abort`, `__cxa_atexit`/`__cxa_finalize`/`__dso_handle` — single-threaded fast-path guards; static destructors are not run at exit because process teardown reclaims everything).
- **vec.h / str.h**: header-only C++ containers for apps. `Vec<T>` is a growable array for trivially-copyable elements (realloc + memmove growth; `push`/`pop`/`insert`/`remove`/`resize`/`reserve`, checked `at()`, move-only ownership) that bounds-checks indexed access in debug builds and returns false on allocation failure instead of aborting. `String` is a growable NUL-terminated buffer (`assign`/`append`/`equals`, always-valid `c_str()`). They replace fixed-size row/entry arrays so lists are no longer silently truncated.

## libgui

`src/usr/libgui/` is an immediate-mode GUI toolkit — no retained widget tree, no internal event loop:

- **Surfaces**: `Surface` wraps a BGRA buffer with width/height/pitch and an optional display-buffer handle.
- **Primitives**: pixels, rectangles, rounded rectangles (8x8 supersampled corner masks), circles, alpha blits, `gui_fill_rect_blend` (alpha-aware fill for dimming scrims).
- **Pixel core** (`gui_pixops`): SSE2 row primitives for XRGB8888 — `pix_copy_row`, `pix_fill_row`, premultiplied src-over blend rows (`out = src + dst*(255-src_a)/255`, `/255` via the `x+128+((x+128)>>8)>>8` approximation, bit-exact with the scalar `gui_blend_premultiplied` reference). Cached stores only (targets are read back by the next pass). `gui_blit_alpha` uses the blend row on non-overlapping spans and falls back to the directional scalar loop for in-buffer overlap; `gui_fill_rect` uses the fill row. A randomized self-test (`gui_pixops_self_test`) diffs SIMD against scalar; the WM runs it every boot in debug builds and behind `SYSTEM_FLAG_WM_PIXEL_SELFTEST` otherwise.
- **App widgets**: header/nav/list/toggle/slider/segmented-control/text-field/button helpers, popup menus, cards/badges/metrics, boxed-list group containers (`gui_draw_boxed_container`/`gui_draw_boxed_divider`), and the icon glyph set (`gui_draw_glyph`, `GuiGlyphKind`) — the building blocks the apps compose. Glyphs are SVG-authored white silhouettes rasterized to `.uoic` assets under `/usr/share/glyphs/`; `gui_draw_glyph` caches each kind/size and tints the silhouette to the requested foreground. Buttons have a pressed variant (`gui_app_draw_button_ex`) with centered labels; text fields reveal the text tail while focused so the caret stays visible past the field width.
- **Shared chrome**: one drop-shadow recipe for every floating surface (`gui_draw_panel_shadow`: popup menus, dialogs, shell overlays), one modal dialog (`gui_dialog_layout` + `gui_draw_dialog`: scrim from the `overlay_scrim` palette token, content-derived width, optional text field, Cancel/Confirm footer), one scrollbar (`gui_draw_scrollbar` + `gui_scrollbar_w`/`gui_scrollbar_min_thumb`), and one outline shared with windows: the crisp 1 px translucent hairline the WM paints around every window (`gui_window_hairline_color`), with `gui_draw_window_frame` (opaque body fill + hairline) for solid panels and `gui_draw_window_ring` (hairline only) for surfaces that paint their own body, like the dock's glass. Popup menus, dialogs, WM overlays and the dock all draw it, so every floating edge on the desktop is pixel-identical to a window frame.
- **Unified headerbar**: windows have no titlebar strip — the top `gui_headerbar_h()` rows of the client are the headerbar band, where the WM overlays the traffic lights at the left (`gui_traffic_lights_w()` is the cluster's right edge, the x apps start their own headerbar content at). `gui_app_begin` fills the canvas with the app background and returns the layout with `header_rect` set to the band and `body_rect` to the content region below; there is no divider under the band. Controls an app places in the band are published with `gui_window_set_header_input` (canvas-coordinate rects, `WINDOW_HEADER_INPUT_MAX` = 8) so the WM forwards clicks and hovers to them instead of treating the band as the window's drag zone; rects are re-published each frame and cleared to zero on window registration. See [Window manager — Window Chrome and Unified Headerbar](wm.md#window-chrome-and-unified-headerbar).
- **Design tokens**: a fixed spacing scale (`gui_space_0_5/1/1_5/2/3/4` = 4/8/12/16/24/32), radius scale (`gui_radius_xs..xl` = 4/6/10/12/12), and derived metrics (`gui_app_row_h`, `gui_app_row_tall_h`, `gui_app_nav_h`, `gui_app_row_gap`, `gui_app_control_h`, card-header/badge/dialog-button sizes) so every app and shell surface shares one rhythm. Windows, popups, dialogs and WM overlays share the `xl` radius (card headers sit at radius-1 inside the 1 px hairline); the dock rounds at the app-icon ratio instead — 25% of its height, the same proportion as the `.uoic` icon corners — so the panel reads as one shape with the icons it carries.
- **Theming**: dark/light palettes synced from the shared registry, UI scaling helpers.
- **Fonts** (`.uof`): atlas-based bitmap fonts with per-size loading, UTF-8 decoding, 26.6 fixed-point advances with GPOS kerning, subpixel glyph placement for UI fonts (mono stays on an integer grid), measurement and clipped text drawing, built-in bitmap fallback. See [UOF Font Format](formats/uof.md).
- **Images**: `.uoic` icon, `.uocu` cursor, and `.uowp` wallpaper loaders with entry selection by size/scale/variant, including a QOI decoder and scaled-cover blitting.
- **Window protocol**: registration, resize handling, damage commits — see [Window manager](wm.md).
- **System integration**: `gui_set_window_title` updates the window title live (the WM samples it each commit; it feeds app-identity matching, the window context menu and launchers); `gui_notify` posts a toast through the registry (`notify_generation`) that the WM displays like its own notifications. `gui_window_title_matches` is the shared app-identity test for registry window slots: a window matches its app on an exact title or a `detail - App` suffix, so apps that rename their window (Files, Latitude) stay discoverable by the dock, menubar, and WM launchers.
- **App menus**: a focused app builds a `MenuModel` locally (`gui_menu_model_reset/add_menu/add_item/add_separator`) and publishes it with `gui_menu_publish` (registry slot guarded by an odd/even seq protocol); the menubar renders it and dispatches activated item IDs through the window entry, which the app consumes with `gui_menu_take_command`. App item IDs must stay below `MENU_CMD_APP_MAX` (0xE000): the menubar reserves that range and above for its own window-operation (0xE001+), system (`MENU_CMD_RESERVED_BASE`=0xF000+) and window-list (0x10000+) commands, and `gui_menu_model_add_item` rejects IDs in it. `gui_clipboard_copy`/`gui_clipboard_paste` move text through the registry clipboard (4095-byte cap, seq-stable reads), shared by all apps.
- **Frame waits**: `gui_wait_frame/gui_poll_frame` wrap display event waits for FLIP_COMPLETE/VBLANK.

## libapp

`src/usr/libapp/` is the application runtime and widget kit that every graphical app is built on. It owns the window, the double-buffered canvas, the event loop, resize/theme/menu/focus plumbing, and damage publishing, so an app supplies only drawing and interaction logic.

- **Runtime** (`app.h`): an app fills an `AppConfig` (title, size, min size, `WIN_FLAG_*`, pacing, callbacks) and calls `app_run`. The runtime registers the window, allocates a private backbuffer canvas, and loops. `on_draw(canvas)` paints a whole frame into the canvas; the app marks what changed with `app_invalidate`/`app_invalidate_all` and the runtime copies only those rects to the window and publishes damage. `on_event` receives input and lifecycle events (close is handled by the runtime as a clean exit; resize arrives after the window and canvas were re-synced; scroll arrives after invalidation). `on_menu` gets menubar commands, `on_menus` rebuilds the model at startup and on focus gain (call `app_publish_menus` to refresh otherwise), `on_settings` fires after a registry settings-generation change and theme re-sync, and `on_idle` runs each iteration for periodic work.
- **Pacing**: `frame_ticks == 0` runs event-driven — `on_draw` only when something was invalidated, sleeping `idle_ms` (default 16) when idle. `frame_ticks > 0` runs continuous — `on_draw` every frame, paced with `sleep_until_ticks` and catch-up (`app_set_frame_ticks` adjusts the interval live).
- **Scrollable content**: `app_set_content_size` grows the window backing and canvas so the whole content is retained and the WM scrolls it (`app_scroll_x/y` read the offset); call it at the start of a frame, before drawing into the grown area. It also clamps the WM-owned scroll offset to the new content bounds in the same store-fence group as the content size, so the app's draw and the compositor agree on the offset on the very frame a folder shrinks while scrolled (otherwise sticky panels draw off-canvas for a frame). The grown canvas is pre-filled with the app background before the previous rows are preserved, so uncovered pixels never reach the backing as garbage when the viewport scrolls into them.
- **Manual mode**: apps with an extra event source use `app_create`/`app_pump`/`app_commit`/`app_destroy` instead of `app_run` to interleave GUI plumbing with their own loop — the terminal does this around its epoll shell pipe and incremental renderer, with `on_draw` left unset so it keeps drawing to the window directly.
- **Widgets** (`widgets.h`): stateful controls over the immediate-mode libgui primitives. Each owns its hover/press/focus/value state; `*_event` returns `WIDGET_*` bitmasks (`WIDGET_CHANGED` = invalidate the rect) and `*_draw` renders. Provided: buttons (release-to-apply with drag-away cancel, optional fire-on-down), toggle rows, sliders (live drag with `gui_app_slider_track_rect` hit-testing and a release-to-persist click), segmented controls, text fields (caller-owned buffer, Enter/Escape, click-outside blur), popup menus (viewport-clamped placement), modal dialogs (confirm/cancel/dismiss with an optional field), help overlays, a scroll view (wheel + draggable vertical/horizontal thumbs, track page-jumps, `widget_scroll_view_reveal_y` for keyboard nav), and a shared rect hit-test helper.
- **Standard menus and shortcuts** (`app.h`): `app_menus_add_edit` publishes a uniform Edit menu (Undo/Redo/Cut/Copy/Paste/Delete/Select All) with accelerators and per-entry enable flags under shared `APP_CMD_*` IDs; `app_menus_add_help` publishes the standard Help menu (an optional tips entry plus the reserved About uniOS item); `app_edit_shortcut` maps a Ctrl+letter `EVT_KEY_DOWN` to its `APP_CMD_*`. Apps with non-standard bindings (the terminal uses Ctrl+X for Copy because Ctrl+C is SIGINT) keep their own menu.
- **Settings** (`app.h`): `app_setting_load_int`/`app_setting_save_int` persist per-app toggles in `/data/APPS.CFG`, kept separate from `SYSTEM.CFG` so app writes never race WM/Preferences persistence of system settings.

Apps that bypass libapp still poll kernel events directly with `poll_event/wait_event` (`SYS_GET_EVENT`).

## libmedia

`src/usr/libmedia/` is a freestanding image codec library with no libc file I/O: callers load a whole file into memory and decode from the buffer.

- **API** (`media_image.h`): `media_image_decode(data, size, out)` dispatches on magic bytes, `media_image_scale` does bilinear resize, `media_image_free` releases the result. Output is straight-alpha ARGB8888 (`A<<24 | R<<16 | G<<8 | B`).
- **Codecs**: PNG (zlib/DEFLATE, all color types, tRNS, 1/2/4/8/16-bit), baseline JPEG (Huffman, fixed-point IDCT, bilinear JFIF chroma upsampling, BT.601 full-range conversion), GIF (bounded LZW, interlace, transparency as alpha-zero palette color), BMP (24/32-bit BI_RGB), QOI.
- **Limits**: `MEDIA_MAX_DIMENSION` 16384, `MEDIA_MAX_PIXELS` 16 Mi pixels (64 MiB ARGB). Progressive JPEG and Adam7 PNG are rejected; allocations go through libc `malloc`.
- Verified by a host-side harness against PIL references and hand-crafted malformed inputs.

## Building Apps

Per `meson.build`:

- `crt0` is assembled from `crt0.asm` (NASM elf64).
- `libc`, `libgui`, `libapp`, and `libmedia` are static libraries built freestanding (`-fno-exceptions -fno-rtti -mno-red-zone`, no stack protector, no PIE).
- The `app_sources` map lists each app directory; sources are globbed at setup time. Apps link `crt0 + libc` (`shell`, `init`) or `crt0 + libc + libapp + libgui` (everything else) with `--whole-archive` through `ld.lld` and `user.ld`. `libmedia` is linked after `--no-whole-archive` for apps that need it (currently `files` and `imageviewer`), so only referenced codec objects are pulled in.
- Each app ELF is staged as `/bin/<name>.elf` and packed into `unifs.img`.

Current apps: shell, init, wm, menubar, dock, files, terminal, latitude, preferences, clock, calendar, calculator, imageviewer. Adding an app requires a map entry (see [Building and running](build.md)).
