## Description

What changed?

Fixes #

## Type of Change

- [ ] Bug fix
- [ ] Feature
- [ ] Refactor / cleanup
- [ ] Documentation
- [ ] Tooling / build
- [ ] Asset update

## Area

- [ ] Meridian / boot image
- [ ] Kernel
- [ ] Memory management
- [ ] Filesystems / storage
- [ ] USB / input
- [ ] Display / window manager
- [ ] Userspace / shell
- [ ] App
- [ ] Networking
- [ ] Audio
- [ ] Documentation / website
- [ ] Tools / generated assets

## Checklist

- [ ] Built release: `meson compile -C build/release boot-disk iso`
- [ ] Built debug: `meson compile -C build/debug boot-disk iso`
- [ ] Ran smoke tests when applicable: `meson test -C build/debug --suite smoke --print-errorlogs`
- [ ] Tested the affected QEMU target or hardware path
- [ ] Checked serial output when changing boot, kernel, drivers, shutdown, or panic paths
- [ ] Tested `/data` persistence when changing storage, FAT32, USB storage, rootfs staging, or image generation
- [ ] Included screenshots for visible desktop, app, website, cursor, icon, font, or wallpaper changes
- [ ] Kept generated files and source assets in sync when changing runtime assets

## Validation

List the exact commands and manual tests performed.

```text
[paste commands and results here]
```
