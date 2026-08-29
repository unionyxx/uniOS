# Building and Running

uniOS builds with Meson using the LLVM toolchain as a cross build. Configuring without the cross file is a hard error.

## Requirements

- `meson` (>= 1.4.0) and `ninja`
- `clang`, `clang++`, `ld.lld`, `lld-link`, `llvm-ar`, `llvm-strip`
- `nasm`
- `qemu-system-x86_64` and OVMF UEFI firmware (for running)
- `python3` with `Pillow` and `CairoSVG` (asset generation tools)

  The wallpaper packager rasterizes `assets/wallpapers/*.svg` via CairoSVG and **requires** it (no silent fallback). If the default `python3` lacks CairoSVG (common on Windows where `python3` resolves to msys2's interpreter), `meson setup` probes `python` and prefers a CairoSVG-capable interpreter for the asset targets, so the SVG art is baked into the `.uowp` instead of a placeholder gradient.

CI additionally installs `clang-format`, `clang-tidy`, and `cppcheck` for the developer checks.

## Configure and Build

```sh
# Release image
meson setup build/release --cross-file toolchains/llvm.ini --buildtype release
meson compile -C build/release boot-disk iso

# Debug image
meson setup build/debug --cross-file toolchains/llvm.ini --buildtype debug
meson compile -C build/debug boot-disk iso
```

Debug builds keep boot logs, run registered kernel tests, and print boot-phase timings. Release builds boot straight to the desktop.

## Build Outputs

Generated artifacts live in the build directory. Never hand-edit them; they are rebuilt by ninja targets.

| Artifact | Built by | Contents |
| --- | --- | --- |
| `kernel.elf` | `ld.lld` with `linker.ld` | Higher-half kernel, link base `0xffffffff80000000` |
| `BOOTX64.EFI` | `lld-link` (`/subsystem:efi_application`) | Meridian UEFI loader (PE/COFF) |
| `unifs.img` | `tools/mkunifs.py` | Read-only root filesystem image |
| `efi.img` | `tools/build_efi_image.py --mode fat` | FAT32 EFI system partition image |
| `boot.img` | `tools/build_efi_image.py --mode disk` | Raw disk: ESP + writable `UNI_DATA` FAT32 partition |
| `uniOS.iso` | `tools/create_iso.py` | UEFI-bootable ISO9660 image |

See [Boot images](images.md) for the on-disk layouts.

## Run Targets

All run targets boot `boot.img` in QEMU unless noted. The QEMU machine is `q35` on Linux and `pc` on Windows; display is `virtio-vga` at 1920x1080 with 512 MiB RAM.

| Target | Description |
| --- | --- |
| `run` | Standard graphical run |
| `run-serial` | Serial console on stdio |
| `run-headless` | No VGA output, serial on stdio |
| `run-usb` | xHCI controller with USB keyboard and mouse |
| `run-qemu-net` | User-mode networking with an e1000 NIC |
| `run-qemu-full` | USB devices + network + serial |
| `run-esp-usb` | Boot `efi.img` from an emulated USB stick |
| `run-smp` / `run-smp4` | 2-core / 4-core boots (default targets are single-core) |

Suffix variants exist for most targets (`-serial`, `-headless`).

```sh
meson compile -C build/release run
```

## Tests

Smoke tests boot the real image headless in QEMU and watch the serial log.

```sh
meson test -C build/debug --suite smoke --print-errorlogs
```

A run passes when the log shows `first desktop frame submitted` (and `ktest suite passed` in debug builds), and fails on `KERNEL PANIC` or `ktest suite failed`.

SMP suites are opt-in:

```sh
meson test -C build/debug --suite smoke-smp     # 2 cores
meson test -C build/debug --suite smoke-smp4    # 4 cores (slow)
meson compile -C build/debug smp-soak           # repeated 4-core boots
```

See [Testing](testing.md) for the test framework and validation guidance.

## Developer Checks

```sh
meson compile -C build/debug lint       # cppcheck (optional tool)
meson compile -C build/debug analyze    # clang-tidy (optional tool)
meson compile -C build/debug format     # clang-format in place
```

CI runs lint and a format check (`git diff --exit-code` after `format`) as continue-on-error steps; keep the tree format-clean locally.

These targets are whole-tree: `format` rewrites every `src/` and `include/` source in place, and `lint`/`analyze` scan all sources. When working on a change, scope the checks to the files you modified — `clang-format -i <file>` and `clang-tidy -p build/debug <file>.cpp` — and do not commit reformatting or fixes for files you did not touch.

clang-tidy reads the curated `.clang-tidy` at the repository root (bugprone, clang-analyzer, and performance checks tuned for freestanding code, with the noisy or OS-hostile checks disabled). The file is required: without a config clang-tidy errors out with `no checks enabled`.

## Adding Sources and Apps

Kernel and userspace source lists are computed by `tools/meson_list_sources.py` **at meson setup time**:

- A new kernel `.cpp` may be invisible to an already-configured build directory. Run `meson setup --reconfigure build/debug`, or add the file to the hardcoded keep-visible list near the top of `meson.build`.
- New apps must be registered in the `app_sources` map in `meson.build`. Apps link against `crt0` + libc (+ libgui, unless the app is `shell` or `init`) and are staged automatically as `/bin/<name>.elf`.

Nothing reaches the runtime image until it is staged into the build rootfs and `unifs.img` is regenerated, which the build does automatically.

## Toolchain Notes

- Kernel: `--target=x86_64-unknown-none-elf`, `-mcmodel=kernel`, `-mno-red-zone`, `-mgeneral-regs-only` (the kernel never touches vector registers so interrupts cannot corrupt FPU state), `-fno-pie`, `-fno-exceptions`, `-fno-rtti`, `-fno-omit-frame-pointer`.
- Meridian: compiled for `x86_64-pc-windows-msvc` and linked with `lld-link` because UEFI applications are PE/COFF, not ELF. It is never linked with `ld.lld`.
- `toolchains/llvm.ini` sets `host_machine.system = 'none'` and selects `lld` for kernel linking.
- The kernel build bakes `git rev-parse --short HEAD` in as `GIT_COMMIT`; it is visible in the `about` app and `SYS_GETSYSINFO`.
