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
- **string**: `strlen/strcpy/strcat/strncpy/strncat/strcmp/strncmp/strchr/strrchr/strstr/strtok` (single global strtok state), `memset/memcpy/memmove/memcmp` with 8-byte fast paths, `itoa` (base 2-36, INT64_MIN-safe).
- **stdio**: `printf/sprintf/snprintf/vsnprintf` into a 4 KiB stack buffer with raw fd writes — supports `%s %d %i %u %o %x %X %p %c`, `-`/`0` flags, widths, `l`/`ll`. No floats, no `FILE*`, no buffering.
- **stdlib**: a region allocator over `SYS_MMAP` — 64 KiB regions with magic-validated block lists, first-fit + split, coalescing free, fully-free regions unmapped, dedicated blocks above 32 KiB; `calloc` with overflow checks; `realloc` with in-place growth; `atoi`; a simple LCG `rand`.
- **socket**: POSIX-shaped wrappers over the network syscalls plus byte-order helpers.
- **wav**: userspace RIFF/WAV parser used by the shell `play` command.
- **log**: leveled `[sec.mmm] scope [mark] message` logging to stdout.
- **config_utils**: `key=value` config readers/writers used for `SYSTEM.CFG` and wallpaper settings.
- **cxx.cpp**: global `new`/`delete` over malloc, and a `__cxa_pure_virtual` fault handler.

## libgui

`src/usr/libgui/` is an immediate-mode GUI toolkit — no retained widget tree, no internal event loop:

- **Surfaces**: `Surface` wraps a BGRA buffer with width/height/pitch and an optional display-buffer handle.
- **Primitives**: pixels, rectangles, rounded rectangles (8x8 supersampled corner masks), circles, alpha blits, `gui_fill_rect_blend` (alpha-aware fill for dimming scrims).
- **App widgets**: header/nav/list/toggle/slider/segmented-control/text-field/button helpers, popup menus, cards/badges/metrics — the building blocks the apps compose. Buttons have a pressed variant (`gui_app_draw_button_ex`) with centered labels; text fields reveal the text tail while focused so the caret stays visible past the field width.
- **Shared chrome**: one drop-shadow recipe for every floating surface (`gui_draw_panel_shadow`: popup menus, dialogs, shell overlays), one modal dialog (`gui_dialog_layout` + `gui_draw_dialog`: scrim from the `overlay_scrim` palette token, content-derived width, optional text field, Cancel/Confirm footer), one scrollbar (`gui_draw_scrollbar` + `gui_scrollbar_w`/`gui_scrollbar_min_thumb`).
- **Design tokens**: a fixed spacing scale (`gui_space_0_5/1/1_5/2/3/4` = 4/8/12/16/24/32), radius scale (`gui_radius_xs..2xl` = 4/6/8/12/16/20), and derived metrics (`gui_app_row_h`, `gui_app_row_tall_h`, `gui_app_nav_h`, `gui_app_row_gap`, `gui_app_control_h`, card-header/badge/dialog-button sizes) so every app and shell surface shares one rhythm.
- **Theming**: dark/light palettes synced from the shared registry, UI scaling helpers.
- **Fonts** (`.uof`): atlas-based bitmap fonts with per-size loading, ASCII fast tables, alpha LUTs, measurement and clipped text drawing, built-in bitmap fallback.
- **Images**: `.uoic` icon, `.uocu` cursor, and `.uowp` wallpaper loaders with entry selection by size/scale/variant, including a QOI decoder and scaled-cover blitting.
- **Window protocol**: registration, resize handling, damage commits — see [Window manager](wm.md).
- **System integration**: `gui_set_window_title` updates the window's titlebar live (the WM recomposes it on change); `gui_notify` posts a toast through the registry (`notify_generation`) that the WM displays like its own notifications. `gui_window_title_matches` is the shared app-identity test for registry window slots: a window matches its app on an exact title or a `detail - App` suffix, so apps that rename their window (Files, Latitude) stay discoverable by the dock, menubar, and WM launchers.
- **App menus**: a focused app builds a `MenuModel` locally (`gui_menu_model_reset/add_menu/add_item/add_separator`) and publishes it with `gui_menu_publish` (registry slot guarded by an odd/even seq protocol); the menubar renders it and dispatches activated item IDs through the window entry, which the app consumes with `gui_menu_take_command`. IDs >= `MENU_CMD_RESERVED_BASE` are menubar-owned. `gui_clipboard_copy`/`gui_clipboard_paste` move text through the registry clipboard (4095-byte cap, seq-stable reads), shared by all apps.
- **Frame waits**: `gui_wait_frame/gui_poll_frame` wrap display event waits for FLIP_COMPLETE/VBLANK.

Apps poll kernel events themselves with `poll_event/wait_event` (`SYS_GET_EVENT`).

## libmedia

`src/usr/libmedia/` is a freestanding image codec library with no libc file I/O: callers load a whole file into memory and decode from the buffer.

- **API** (`media_image.h`): `media_image_decode(data, size, out)` dispatches on magic bytes, `media_image_scale` does bilinear resize, `media_image_free` releases the result. Output is straight-alpha ARGB8888 (`A<<24 | R<<16 | G<<8 | B`).
- **Codecs**: PNG (zlib/DEFLATE, all color types, tRNS, 1/2/4/8/16-bit), baseline JPEG (Huffman, fixed-point IDCT, bilinear JFIF chroma upsampling, BT.601 full-range conversion), GIF (bounded LZW, interlace, transparency as alpha-zero palette color), BMP (24/32-bit BI_RGB), QOI.
- **Limits**: `MEDIA_MAX_DIMENSION` 16384, `MEDIA_MAX_PIXELS` 16 Mi pixels (64 MiB ARGB). Progressive JPEG and Adam7 PNG are rejected; allocations go through libc `malloc`.
- Verified by a host-side harness against PIL references and hand-crafted malformed inputs.

## Building Apps

Per `meson.build`:

- `crt0` is assembled from `crt0.asm` (NASM elf64).
- `libc`, `libgui`, and `libmedia` are static libraries built freestanding (`-fno-exceptions -fno-rtti -mno-red-zone`, no stack protector, no PIE).
- The `app_sources` map lists each app directory; sources are globbed at setup time. Apps link `crt0 + libc` (`shell`, `init`) or `crt0 + libc + libgui` (everything else) with `--whole-archive` through `ld.lld` and `user.ld`. `libmedia` is linked after `--no-whole-archive` for apps that need it (currently `files` and `imageviewer`), so only referenced codec objects are pulled in.
- Each app ELF is staged as `/bin/<name>.elf` and packed into `unifs.img`.

Current apps: shell, init, wm, menubar, dock, files, terminal, latitude, about, preferences, clock, calendar, calculator, imageviewer. Adding an app requires a map entry (see [Building and running](build.md)).
