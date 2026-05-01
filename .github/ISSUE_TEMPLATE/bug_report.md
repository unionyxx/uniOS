---
name: Bug Report
about: Report a bug in uniOS
title: ''
labels: bug
assignees: ''
---

**Describe the bug**
A clear description of the panic, freeze, incorrect behavior, driver issue, desktop issue, app issue, or tooling issue.

**To reproduce**
1. Build with `meson compile -C build/debug boot-disk iso` or describe the build you used.
2. Run the exact target, for example `meson compile -C build/debug run-serial`.
3. Describe the exact boot path, app, shell command, driver path, or tool command involved.
4. Describe the failure.

**Expected behavior**
What you expected to happen.

**Environment**
- Host OS: [for example Ubuntu 24.04, Fedora, macOS, WSL2]
- Build type: [release / debug / debugoptimized]
- Target: [QEMU disk / QEMU USB / QEMU network / ISO / hardware]
- QEMU version, if applicable:
- Firmware/OVMF package, if applicable:
- Hardware, if applicable:

**Disk image and persistence**
- Did the issue involve `boot.img`?
- Did the issue involve `/data` or the `UNI_DATA` partition?
- Was this a fresh image or an image reused across rebuilds?

**Serial log**
If the bug happens during boot, driver initialization, session startup, or shutdown, include serial output from:

```sh
meson compile -C build/debug run-serial
```

```text
[paste serial log here]
```

**Screenshots**
If this is a visible desktop, app, cursor, wallpaper, font, compositor, or website issue, attach screenshots.

**Additional context**
Add any relevant notes about recent changes, connected USB devices, network mode, sound mode, or filesystem state.
