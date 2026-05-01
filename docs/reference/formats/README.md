# Runtime Asset Formats

uniOS uses generated binary asset formats in the runtime filesystem.

- [UOIC](uoic.md): icon packages.
- [UOCU](uocu.md): cursor packages.
- [UOF](uof.md): font files.
- [UOWP](uowp.md): wallpaper packages.

The source assets live under `appicons/`, `assets/`, and `cursors/`. The generated files are staged into `rootfs/usr/share/` by the build.
