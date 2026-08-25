# Runtime Asset Formats

uniOS uses generated binary asset formats in the runtime filesystem.

- [UOIC](uoic.md): icon packages.
- [UOCU](uocu.md): cursor packages.
- [UOF](uof.md): font files.
- [UOWP](uowp.md): wallpaper packages.

The source assets live under `appicons/`, `assets/`, and `cursors/`. Icons, cursors, and fonts are regenerated with `tools/appicon_rasterize.py`, `tools/cursor_rasterize.py`, and `tools/uof_convert.py` and committed under `rootfs/usr/share/`, which is staged into the runtime image as-is. Only the default wallpaper package (`.uowp`) is generated at build time from `assets/wallpapers/`.
