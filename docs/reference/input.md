# Input

Input comes from PS/2 controllers and USB HID devices, is merged in the kernel input layer (`src/drivers/class/hid/input.cpp`), and reaches userspace through per-process event queues — there are no input device files.

## PS/2 Keyboard

`src/drivers/class/hid/ps2_keyboard.cpp` uses ports `0x60`/`0x64` with scan code set 1:

- `0xE0` extended prefix decodes arrows, Home/End, Delete, Page Up/Down into special characters (`0x80-0x88`; with shift held, left/right/up/down/Home/End become `0x90-0x95` for selection).
- Modifier tracking (shift/ctrl/alt/capslock), Ctrl+letter control codes, Alt+Space triggers the desktop launcher. Ctrl+C sends SIGINT to the current process only in a terminal/desktop context (`input_ctrl_c_to_signal()`); while a GUI app is focused the `0x03` control char is delivered instead so apps can bind it (Edit > Copy).
- A 256-byte lock-free ring buffer holds decoded characters; IRQ1 (vector 33).

## PS/2 Mouse

`src/drivers/class/hid/ps2_mouse.cpp` enables the auxiliary device, then runs the IntelliMouse magic sequence (sample rates 200, 100, 80) and reads the device ID:

- ID 3 or 4: 4-byte packets with scroll wheel; otherwise 3-byte packets.
- Packet validation (bit 3), sign extension, Y inversion, scroll accumulation, pointer acceleration (small moves stay 1:1, faster moves amplified ~1.5×), cursor clamping to the framebuffer; position starts at screen center. IRQ12 (vector 44).

## Merging

`input_mouse_get_state()` merges PS/2 and USB mouse state (USB overrides when connected); `input_keyboard_get_char()` prefers USB, then PS/2. `input_poll()` pumps HID polling and key repeat; it runs from the kernel idle loop and from event consumers.

## Event Delivery

Two paths, both syscall-based:

**Graphical (window manager).** Kernel event pumping converts polled input into `Event` records — mouse move/down/up/scroll, key down/up, window resize/close (`include/uapi/event.h`) — and pushes them into the window manager's per-process `EventQueue` (128-slot ring, IRQ-safe spinlock). The WM drains it with `SYS_GET_EVENT` (blocking or non-blocking, interruptible by fatal signals) and retargets events to client windows with `SYS_POST_EVENT`, translating screen coordinates to window-local space. The WM registers itself with `SYS_GUI_REGISTER_WM`; focus is tracked with `SYS_GUI_SET_FOCUS`.

**Client delivery rules (WM → app).** Mouse moves are forwarded only to the focused user window while the pointer is inside its client area — except while a client holds a pointer grab. A mouse-down delivered to a client grabs the pointer for that window: until the matching button is released, moves and the release are forwarded to the grabbed window even outside its client area, so in-window drags (sliders, scrollbars, selections) keep working past the frame. The release is likewise delivered outside the client area when a grab is active. Keys go to the focused window; the wheel scrolls WM-side content first and is only forwarded to the client when there is nothing to scroll.

## Client Lifecycle Events

The WM posts synthetic lifecycle events so clients never have to poll the registry for interaction state:

| Event | Posted when |
| --- | --- |
| `EVT_FOCUS` / `EVT_UNFOCUS` | the window's owner gains/loses keyboard focus |
| `EVT_MOUSE_LEAVE` | the pointer exits the client area, a shell overlay takes over, the WM starts a window drag, or the window is minimized |
| `EVT_WINDOW_SCROLL` | the WM changed the window's scroll offset (wheel); `scroll` data carries the new offsets |

Clients use these to clear hover state, stop drags, and redraw sticky overlays; unknown events are ignored by older apps.

**Text.** `SYS_READ` on stdin polls the keyboard (and serial) directly, sleeping briefly between polls.

Producers wake blocked consumers through the scheduler's input wait queues.

## Device Management

`SYS_INPUT_ENUM_DEVICES` (`src/drivers/class/hid/input.cpp`) returns a snapshot of input sources in `InputDeviceInfo` (`uapi/input.h`):

| Kind | id | vendor/product |
| --- | --- | --- |
| `INPUT_DEVICE_PS2_MOUSE` | `0xFE` | 0/0 |
| `INPUT_DEVICE_PS2_KEYBOARD` | `0xFF` | 0/0 |
| `INPUT_DEVICE_USB_MOUSE` | xHCI slot | from the USB device descriptor |
| `INPUT_DEVICE_USB_KEYBOARD` | xHCI slot | from the USB device descriptor |

USB combo devices yield one entry per stream (mouse + keyboard). Names are synthesized (`"PS/2 Mouse"`, `"USB Mouse"`, ...) since USB string descriptors are not fetched; the Settings app shows `vendor:product` as a secondary line. The call always lists the two PS/2 devices and one entry per connected USB HID stream.

- `SYS_INPUT_SET_POINTER_SPEED` (Q8 multiplier, 256 = 1.0x, clamped 16-1024) scales raw mouse deltas **before** position accumulation in `ps2_mouse.cpp` (`accelerate_delta` site) and `usb_hid.cpp` (`process_mouse_report`). It is orthogonal to the PS/2 acceleration curve.
- `SYS_INPUT_SET_REPEAT_RATE` (delay_ms, rate_ms) drives the USB HID software key-repeat path in `usb_hid.cpp` (`handle_key_repeat`); PS/2 keyboard repeat is hardware-typematic and not tuned here.
- `SYS_INPUT_SET_DEVICE_ENABLED` freezes a source **at the driver** (`ps2_mouse_handler`/`ps2_keyboard_handler` drop packets; `usb_hid` interrupt callbacks return early) so its accumulated state is preserved rather than zeroed — disabling the only active mouse does not snap the cursor to the origin.

Settings are volatile in the kernel; `SYSTEM.CFG` is the source of truth across boots. The WM restores pointer speed and repeat rate once at boot (`wm_apply_input_settings` in `wm_settings.cpp`); the Settings app re-applies live when the user drags a slider or toggles a device. USB device enables are session-only (slot ids are transient across replug), so they are not persisted.
