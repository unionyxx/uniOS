# Security Policy

## Supported Branch

Security fixes are applied on `main`.

## Scope

Reports may cover:

- Kernel memory safety issues.
- Syscall validation bugs.
- Userspace process isolation bugs.
- VFS, FAT32, uniFS, or block-device bugs.
- Bootloader parsing or memory map bugs.
- USB, storage, network, display, input, or audio driver bugs.
- Persistent `/data` handling bugs.
- Rootfs or image-generation bugs.

## Reporting

Send reports to `uniosdev@proton.me`.

Include:

- A clear description of the issue.
- Steps to reproduce it.
- A proof of concept if available.
- The affected commit hash.
- Build type: release, debug, or debugoptimized.
- Boot target: `boot.img`, ISO, QEMU disk, QEMU USB, or hardware.
- QEMU command or Meson run target used.
- Firmware details when relevant.
- Disk image or `/data` state when relevant.
- Serial logs when available.

## Process

- Initial acknowledgment target: 48 hours.
- Follow-up and disclosure timing are coordinated per report.
- Avoid public disclosure until a fix or mitigation is ready.
