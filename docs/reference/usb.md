# USB

USB support centers on a full xHCI host controller driver (`src/drivers/bus/usb/xhci/`), a core enumeration layer (`src/drivers/bus/usb/`), hubs, HID, and mass storage.

## xHCI Controller

- PCI discovery via `pci_find_xhci`; firmware (USBLEGSUP) handoff, controller reset, DCBAA, command ring, event ring with ERST, interrupter 0 (IMOD 160), scratchpad buffers, port power-on, run.
- Rings are fixed 256-TRB arrays with a self-referencing Link TRB and per-ring cycle-bit tracking. Constants: 256 slots, 32 endpoints per device.
- Context stride honors the CSZ capability bit (32/64-byte contexts).
- Device addressing builds slot + EP0 input contexts (route string, speed, root port, TT info for low/full speed behind hubs) and issues Address Device. Endpoint configuration converts intervals (LS/FS: log2+3; HS/SS: interval-1, clamped to 15).
- Transfers: control (Setup/Data/Status TRBs, immediate data for setup), bulk (Normal TRBs with IOC), and persistent interrupt transfers with callbacks.
- Completion drains the event ring: transfer events route to the waiting transfer state or invoke interrupt callbacks and re-arm; port status change events mark ports for enumeration. Endpoint error recovery stops/resets the endpoint, resets the ring, sets the TR dequeue, and clears stalls.
- Interrupt delivery prefers MSI-X, then MSI, then a routed legacy IRQ, then polling.

## Enumeration

`usb_enumerate_device()` (in `usb_core.cpp`):

1. Port reset, Enable Slot, Address Device (one retry).
2. Read 8-byte device descriptor, update EP0 max packet size, read the full descriptor.
3. Parse the configuration descriptor, building HID keyboard/mouse and MSC bulk endpoint candidates (including SuperSpeed companion burst/mult/ESIT).
4. Set Configuration, configure endpoints, then notify class drivers (HID, MSC). Class 9 devices register as hubs.

Limits: 16 devices. Hotplug flows through `usb_poll()` (port change re-enumeration) and hub polling. The hub driver supports up to 8 hubs with interrupt-in status change notification, per-port status/clear feature/reset handling.

## HID

`src/drivers/class/hid/usb_hid.cpp` supports keyboards and mice:

- Boot protocol when available; otherwise the report descriptor is parsed (bit fields, arrays, variables, report IDs).
- Keyboards: Set Idle (0), interrupt-in callback, key repeat with 500 ms delay and 33 ms rate, HID usage to ASCII tables with shift handling.
- Mice: boot protocol, accumulated x/y clamped to the screen, buttons, and wheel.
- Both feed the shared input layer; USB input overrides PS/2 when present (see [Input](input.md)).

## Mass Storage

See [Storage drivers](storage-drivers.md) for the Bulk-Only Transport and SCSI subset.

## Runtime Wiring

`kmain` runs `usb_init()` + `usb_hid_init()` during driver init, settles USB storage with poll bursts before partition scans, and re-settles it during `/data` mount retries. QEMU run targets with USB attach a `qemu-xhci` controller with USB keyboard and mouse.
