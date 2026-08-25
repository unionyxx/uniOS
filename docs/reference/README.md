# uniOS Reference

This directory is the source of truth for the uniOS documentation. It is rendered as the project wiki at:

<https://unionyxx.github.io/uniOS/wiki/>

The same markdown files are generated into a styled site by `tools/docs_site.py` (run `meson compile -C build/debug wiki`); broken internal links fail the build.

## Pages

- [Home](index.md)
- [Building and running](build.md)
- [Boot images](images.md)
- [Architecture](architecture.md)
- [Boot path](boot.md)
- [Memory management](memory.md)
- [Scheduling](scheduling.md)
- [SMP](smp.md)
- [Processes](processes.md)
- [System calls](syscalls.md)
- [VFS](vfs.md)
- [Filesystems](filesystems.md)
- [Drivers](drivers.md)
- [Storage drivers](storage-drivers.md)
- [USB](usb.md)
- [Input](input.md)
- [Display](display.md)
- [Audio](audio.md)
- [Networking](networking.md)
- [TCP](tcp.md)
- [Userspace runtime](userspace.md)
- [Window manager](wm.md)
- [Shell](shell.md)
- [Shell scripting](scripting.md)
- [Desktop services and apps](apps.md)
- [Runtime configuration](config.md)
- [Testing](testing.md)

## Asset Formats

- [UOIC icon format](formats/uoic.md)
- [UOCU cursor format](formats/uocu.md)
- [UOF font format](formats/uof.md)
- [UOWP wallpaper format](formats/uowp.md)
