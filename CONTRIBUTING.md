# Contributing

## Development Baseline

- Target: x86-64.
- Bootloader: in-tree Meridian UEFI loader.
- Kernel language: freestanding C++20.
- Userspace languages: C and C++.
- Build system: Meson + LLVM.
- Runtime image: `boot.img` with an EFI system partition and optional persistent `UNI_DATA` FAT32 partition.
- Userspace model: native ELF programs under `src/usr/`.
- Desktop model: userspace init starts the window manager, menubar, dock, and apps.

## Required Tools

Install:

- `meson`
- `ninja`
- `clang`
- `clang++`
- `ld.lld`
- `llvm-ar`
- `llvm-strip`
- `nasm`
- `qemu-system-x86_64`
- OVMF UEFI firmware
- `python3`
- Pillow
- CairoSVG

## Code Style

- Match surrounding formatting.
- Keep names descriptive.
- Prefer `constexpr` over magic numbers.
- Do not use `std::` in kernel code.
- Do not use exceptions or RTTI.
- Keep comments for hardware behavior, ordering constraints, invariants, and non-obvious code.
- Remove comments that restate the code.

## Build and Test

Release build:

```sh
meson setup build/release --cross-file toolchains/llvm.ini --buildtype release
meson compile -C build/release boot-disk iso
```

Debug build:

```sh
meson setup build/debug --cross-file toolchains/llvm.ini --buildtype debug
meson compile -C build/debug boot-disk iso
meson test -C build/debug --suite smoke --print-errorlogs
```

Useful run targets:

```sh
meson compile -C build/debug run
meson compile -C build/debug run-serial
meson compile -C build/debug run-headless
meson compile -C build/debug run-usb
meson compile -C build/debug run-qemu-net
meson compile -C build/debug run-qemu-full
```

Static checks when relevant:

```sh
meson compile -C build/debug lint
meson compile -C build/debug analyze
```

## Validation Expectations

Before opening a pull request:

1. Build the touched configuration.
2. Boot the affected path in QEMU.
3. Run the smoke test when the change touches boot, kernel startup, display, userspace init, or the desktop session.
4. Test both serial and graphical paths when changing boot, display, panic, or logging code.
5. Test persistent `/data` behavior when changing storage, FAT32, USB mass storage, rootfs staging, or image generation.
6. Include the exact commands you ran in the pull request.

## Pull Request Notes

- Use an imperative subject.
- Conventional Commit style is preferred.
- Start with user-visible behavior changes when there are any.
- Mention boot, image layout, or persistent storage changes explicitly.
- Include screenshots for visible desktop, app, website, wallpaper, cursor, or icon changes.
- Keep a pull request scoped to one logical change.
