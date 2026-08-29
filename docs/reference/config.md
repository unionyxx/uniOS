# Runtime Configuration

Configuration files are plain text, one `key=value` per line (trailing CR/spaces trimmed). The persistent location is `/data`; bootstrap fallbacks live in the read-only root image under `/etc`.

## Paths

| Purpose | Primary | Fallback |
| --- | --- | --- |
| System settings | `/data/SYSTEM.CFG` | `/etc/system.conf` |
| Wallpaper path | `/data/WALLPAPR.CFG` | `/etc/wallpaper.conf` |
| App view settings | `/data/APPS.CFG` | — |
| User database | `/etc/passwd`, `/etc/shadow` | persisted to `/data/etc/` |
| Shell startup | `/etc/shell.rc` | `/data/shell.rc` |
| Default wallpaper | `/usr/share/wallpapers/default.uowp` | — |

The wallpaper config is a single path on the first line, not key/value.

## SYSTEM.CFG Keys

| Key | Values | Default | Consumers |
| --- | --- | --- | --- |
| `theme` | `dark`, `light` | `dark` | WM, terminal, registry |
| `show_desktop_grid` | `0`, `1` | `1` | WM |
| `clock_show_seconds` | `0`, `1` | `0` | Menubar |
| `launch_terminal_on_boot` | `0`, `1` | `0` | init |
| `ethernet_enabled` | `0`, `1` | `1` | Control center only (the stack always runs) |
| `ethernet_use_dhcp` | `0`, `1` | `1` | Control center only |
| `animations_enabled` | `0`, `1` | `1` | WM |
| `transparency_level` | `0..255` | `180` | WM |
| `volume_level` | `0..100` | `75` | WM |
| `input_pointer_speed` | `16..1024` (Q8, 256 = 1.0x) | `256` | Kernel input layer, WM restores at boot |
| `input_repeat_delay` | `50..2000` (ms) | `500` | Kernel input layer (USB HID repeat) |
| `input_repeat_rate` | `10..500` (ms) | `33` | Kernel input layer (USB HID repeat) |

Writes are performed by the WM (idle-frame persistence, read-modify-writing the input keys to preserve them since they have no registry mirror) and by the Preferences app (atomic write-temp-then-rename). Settings also propagate live through the registry's `settings_generation` counter; storage mode changes go through `storage_request_generation` because `SYS_STORAGE_SET_MODE` is WM-only. Input-device enable state is session-only (USB slot ids are transient) and is not persisted.

## APPS.CFG Keys

Per-app view toggles live in `/data/APPS.CFG`, kept separate from `SYSTEM.CFG` so app writes never race the WM/Preferences persistence. Each app reads its keys at startup and writes them when the user toggles the setting; if storage is off or read-only the toggle applies for the session only. Values are integers (`0`/`1` for booleans, a step offset for zoom).

| Key | Values | Default | Consumer |
| --- | --- | --- | --- |
| `files_sidebar` | `0`, `1` | `1` | Files (Places/Storage sidebar) |
| `files_view_mode` | `0`=list, `1`=icons | `0` | Files (listing layout) |
| `latitude_wrap` | `0`, `1` | `0` | Latitude (word wrap) |
| `latitude_gutter` | `0`, `1` | `1` | Latitude (line numbers) |
| `latitude_highlight` | `0`, `1` | `1` | Latitude (syntax colors) |
| `terminal_zoom` | `-4..6` | `0` | Terminal (font step from base size) |

## Volumes and Labels

| Volume | Label | Role |
| --- | --- | --- |
| EFI system partition | `UNI_OS` | Boot loader, kernel, root image |
| Data partition | `UNI_DATA` | Persistent storage, mounted at `/data` |

Removable volumes mount under `/vol/<label>`. See [Filesystems](filesystems.md) for mount discovery and the storage guard.

## System Behavior Knobs Outside Files

- Boot logging: debug builds log verbosely to the framebuffer; `SYS_SET_QUIET` toggles it at runtime (the WM silences it after the first frame).
- Storage guard mode: runtime state (default writable), changed through the WM/Preferences, not a config file.
- There is no boot command line and no kernel configuration file.
