# Desktop Services and Apps

`/bin/init.elf` starts the desktop session and supervises it. All windows register through the shared registry described in [Window manager](wm.md).

## init

Startup order:

1. Spawn the window manager.
2. Wait for the shared registry to become valid (magic + menubar/dock block ids), up to 500 attempts at 10 ms.
3. Spawn the menubar, then the dock.
4. Read `launch_terminal_on_boot` from `SYSTEM.CFG` and optionally spawn the terminal.
5. Supervision loop on `waitpid(-1)`:
   - WM exits → menubar and dock are terminated and the whole desktop restarts (retries with backoff, killing partial children on failure).
   - Menubar or dock exit → respawned individually.
   - Other children are logged.

## Menubar

Occupies window slot 0. Renders a canvas taller than the visible strip (for dropdown menus) into a shared block and pushes damage. Contents: uniOS logo menu, app menus of the focused app, focused window title, and a clock/date button (`Mon D  HH:MM`, seconds optional) that toggles the control center (`cp_toggle_requested`; the WM also toggles it for clicks in the rightmost 120 px). Hover damage tracks the logo and date buttons independently. The system menu offers About, Settings, Close/Minimize/Maximize of the focused window, Restart, and Shut Down. Launchers focus an existing window by app title (`gui_window_title_matches`) or fork+exec the app.

**App menus.** The focused app publishes a menu model into the registry (`MenuModel`: owner pid + seq-stability protocol, up to 8 menus x 16 items with labels, accelerators, disabled/checked flags). The menubar renders the model only while `owner_pid == focused_owner_pid`, and appends a composed **Window** menu for every app (Minimize / Maximize-Restore / Close, then a live list of open windows, checked = focused). Clicking a menu name opens its dropdown; hovering another name switches; item clicks dispatch the item ID back through the window's `menu_command_id`/`menu_command_seq` fields, which the app polls with `gui_menu_take_command`. IDs at or above `MENU_CMD_RESERVED_BASE` are handled menubar-side (`About uniOS` focuses or launches the About window). Dropdowns draw accelerators right-aligned, vector checkmarks for checked items, and dimmed disabled items.

## Dock

Occupies window slot 1: Files, Latitude, Terminal, Calculator, Calendar, Clock, Settings. Icons come from `.uoic` packages (the calendar composes day/number assets over its base icon). Running windows get indicator dots; clicks cycle matching windows starting after the focused one, or launch. Window-to-app matching uses `gui_window_title_matches` (exact title or `detail - App` suffix), so dynamically retitled windows like `data - Files` still count as running. Launching shows a hollow pending indicator and suppresses duplicate launches until the window registers (8 s timeout); clicks flash a brief pressed state. The dock renders a glass panel over a blurred backdrop and publishes its width in the registry.

## Applications

| App | Description |
| --- | --- |
| **terminal** | Terminal emulator hosting the shell over pipes; epoll on shell output plus GUI events (~16 ms idle poll); per-cell colors with an ANSI CSI subset (clear, cursor home/move, erase line, SGR foreground colors); 2048-line scrollback that holds its position while output streams; blinking caret while focused; starts 80x25. |
| **files** | File manager: places sidebar (Home `/data`, Desktop, Documents, Downloads, Pictures) + volumes; listing via `SYS_GETDENTS`; new folder, rename, copy, move; context menus clamped to the viewport; storage-mode aware. Hover feedback on all rows/places/volumes, arrow-key navigation, dialogs centered on the viewport with a dimmed scrim, release-to-apply buttons and outside-click dismiss; the titlebar tracks the browsed folder. Publishes app menus: File (New Folder, Rename, Delete), Edit (Cut/Copy/Paste through the system clipboard with `copy:`/`cut:` path payloads, Refresh), Go (Up, places, mounted volumes with checked states), Help (tips dialog, About); accelerators Ctrl+N/X/C/V. |
| **preferences** | Settings app: Appearance (theme, wallpaper, transparency, animations), Desktop (grid, clock seconds, volume), Network (ethernet + DHCP toggles), System (launch terminal, storage mode). Writes registry fields and persists `SYSTEM.CFG`/wallpaper config. Wallpaper Apply/Default use release-to-apply with pressed feedback, the volume slider drags past the window via the WM pointer grab, Esc leaves the wallpaper field, and sticky panels redraw on scroll. |
| **latitude** | Text/code editor: 2048 x 512 buffer, 512 KiB open limit with binary sniffing, syntax highlighting (text, C++, JS, Python, Rust, HTML, CSS, JSON, Markdown, shell), project browser, outline, search panes. New/Reload require a confirming second click while changes are unsaved, saves post a system toast, the titlebar tracks the open file, and hover clears on pointer exit. |
| **clock** | Analog clock with continuous-sweep hands driven by a free-running monotonic counter (TSC when available, otherwise scheduler ticks); an RTC phase servo trims it once per second through a bounded-rate correction (hand speed kept within 0.7x-1.3x) so the hands never snap, freeze, or reverse — only a genuine wall-clock step (>2 s) produces a single visible jump, and a non-`+1` RTC reading is accepted only after two consecutive matching reads. The tick rate is self-calibrated from observed RTC boundaries (with re-lock confirmation) and also paces the frame loop. Digital time and date below; per-frame damage is limited to the face and text regions. |
| **calendar** | Month grid with today highlight, Sakamoto weekday computation, leap-year-aware month lengths. Prev/next month arrows, a Today/Go-to-Today button, wheel and Page Up/Down month navigation, arrow-key day selection, and hover that clears on pointer exit. |
| **calculator** | Keypad calculator with pending-op accumulator, percent, sign toggle, and 15-place decimal cap. Full keyboard input (digits, operators, Enter/=, Backspace clears entry, Esc clears all), release-to-apply button semantics with drag-away cancel, an armed-operator highlight, divide-by-zero Error state, and a display that keeps the newest digits visible on overflow. |
| **about** | System information: kernel commit, bootloader name/version, CPU count, timer rate, memory totals, uptime, display capabilities. Double-buffered rendering; the 1 Hz refresh republishes only the runtime and memory panels unless structural data changes. |

All user windows are resizable (`WIN_FLAG_RESIZABLE`) and follow the resize protocol in [Window manager](wm.md).
