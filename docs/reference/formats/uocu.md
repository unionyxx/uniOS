# UOCU Cursor Format

UOCU is the uniOS runtime cursor package format. It stores cursor images with role and hotspot metadata.

## Identity

- Extension: `.uocu`
- Magic: `UOCU`
- Byte order: little-endian
- Runtime pixel format: BGRA8888
- Origin: top-left

## Header

```c
#define UOCU_MAGIC 0x55434F55u // "UOCU", little-endian

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t entry_count;
    uint32_t directory_offset;
    uint32_t directory_size;
    uint32_t metadata_offset;
    uint32_t metadata_size;
} UocuHeader;
```

## Directory Entry

```c
typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t scale;
    uint16_t variant;
    uint16_t cursor_role;
    uint16_t codec;
    uint16_t hotspot_x;
    uint16_t hotspot_y;
    uint32_t frame_duration_ms;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t decoded_size;
    uint32_t checksum;
} UocuEntry;
```

## Runtime Use

The GUI image loader selects a cursor entry by role, display scale, and variant. The selected entry provides both image data and hotspot coordinates.

Cursor files are staged under:

```text
/usr/share/cursors/
```

The current cursor set includes the default pointer, text pointer, busy/progress cursors, drag cursors, zoom cursors, and resize cursors.

## Generation

Cursor source SVG files and metadata live under `cursors/`.

The cursor generation tool writes `.uocu` packages:

```sh
python3 tools/cursor_rasterize.py \
    --source-root cursors \
    --output-root rootfs/usr/share/cursors
```
