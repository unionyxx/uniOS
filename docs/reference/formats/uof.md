# UOF Font Format

UOF is the uniOS runtime font format. It stores preprocessed bitmap font data for the userspace GUI library and desktop applications.

## Identity

- Extension: `.uof`
- Purpose: runtime font data
- Runtime users: GUI library, desktop apps, terminal, menubar, dock, and window manager text rendering paths

## Runtime Use

Generated font files are staged under:

```text
/usr/share/fonts/
```

The current runtime font assets include Geist Mono, Inter UI, and Inter Title sizes used by the desktop session.

## Generation

Font sources live under `assets/fonts/`.

The font conversion tool writes `.uof` files:

```sh
python3 tools/uof_convert.py
```

The Meson build includes generated font files in the root filesystem image.

## Notes

UOF is not a general desktop font format. It is a uniOS runtime format used to avoid parsing full desktop font formats inside the current GUI runtime.
