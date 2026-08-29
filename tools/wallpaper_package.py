#!/usr/bin/env python3

from __future__ import annotations

import argparse
import io
import json
import os
import struct
from pathlib import Path

from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    import cairosvg
except ImportError:
    raise SystemExit(
        "wallpaper_package: the 'cairosvg' module is required to rasterize wallpaper SVGs, "
        "but was not found in the Python interpreter meson used to run this script. "
        "Install cairosvg (and Pillow) into that interpreter and reconfigure the build."
    )


UOWP_MAGIC = 0x50574F55  # "UOWP", little-endian
UOWP_VERSION = 1
UOWP_CODEC_RAW = 4
UOWP_VARIANT_LIGHT = 1
UOWP_VARIANT_DARK = 2
UOWP_COLOR_SRGB = 1
UOWP_TRANSFER_SDR = 1

try:
    RESAMPLE = Image.Resampling
except AttributeError:  # Pillow < 9.1
    class _Resample:
        LANCZOS = Image.LANCZOS

    RESAMPLE = _Resample()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Package SVG wallpapers into a UOWP wallpaper container.")
    parser.add_argument("--light", required=True, help="Light wallpaper SVG source.")
    parser.add_argument("--dark", required=True, help="Dark wallpaper SVG source.")
    parser.add_argument("--output", required=True, help="Output .uowp package.")
    parser.add_argument("--width", type=int, default=2560, help="Rendered wallpaper width.")
    parser.add_argument("--height", type=int, default=1440, help="Rendered wallpaper height.")
    parser.add_argument("--preview-width", type=int, default=512, help="Embedded preview width.")
    parser.add_argument("--preview-height", type=int, default=288, help="Embedded preview height.")
    return parser.parse_args()


def render_svg(svg_path: Path, width: int, height: int, variant: int) -> Image.Image:
    # Pass the SVG as bytes: cairosvg's url= handling is fragile with
    # non-URI paths (especially Windows drive letters).
    svg_bytes = svg_path.read_bytes()
    png_bytes = cairosvg.svg2png(bytestring=svg_bytes, output_width=width, output_height=height)
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def raw_bgra_bytes(image: Image.Image) -> bytes:
    image = image.convert("RGBA")
    # Pillow's raw encoder swaps the channels in C; same bytes as a Python loop.
    return image.tobytes("raw", "BGRA", 0, 1)


def png_preview_bytes(image: Image.Image, width: int, height: int) -> bytes:
    preview = image.convert("RGBA").resize((width, height), RESAMPLE.LANCZOS)
    out = io.BytesIO()
    preview.save(out, format="PNG", optimize=True)
    return out.getvalue()


def write_uowp(path: Path, entries: list[dict], metadata: dict) -> None:
    header_size = 28
    entry_size = 36
    entry_count = len(entries)
    directory_offset = header_size
    directory_size = entry_count * entry_size
    data_offset = header_size + directory_size

    directory = bytearray()
    blobs = bytearray()
    pending_entries = []

    for entry in entries:
        data = entry["data"]
        preview = entry["preview"]
        current = {
            **entry,
            "data_offset": data_offset,
            "data_size": len(data),
            "preview_offset": data_offset + len(data) if preview else 0,
            "preview_size": len(preview),
        }
        blobs.extend(data)
        data_offset += len(data)
        if preview:
            blobs.extend(preview)
            data_offset += len(preview)
        pending_entries.append(current)

    metadata_blob = json.dumps(metadata, separators=(",", ":"), sort_keys=True).encode("utf-8")
    metadata_offset = data_offset if metadata_blob else 0

    for entry in pending_entries:
        directory.extend(
            struct.pack(
                "<IIHHHHIIIII",
                entry["width"],
                entry["height"],
                UOWP_CODEC_RAW,
                entry["variant"],
                UOWP_COLOR_SRGB,
                UOWP_TRANSFER_SDR,
                entry["data_offset"],
                entry["data_size"],
                entry["preview_offset"],
                entry["preview_size"],
                0,
            )
        )

    header = struct.pack(
        "<IHHIIIII",
        UOWP_MAGIC,
        UOWP_VERSION,
        0,
        entry_count,
        directory_offset,
        directory_size,
        metadata_offset,
        len(metadata_blob),
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + directory + blobs + metadata_blob)


def main() -> int:
    args = parse_args()
    light_path = Path(args.light)
    dark_path = Path(args.dark)
    output_path = Path(args.output)

    if args.width <= 0 or args.height <= 0:
        raise SystemExit("wallpaper dimensions must be positive")
    if args.preview_width <= 0 or args.preview_height <= 0:
        raise SystemExit("preview dimensions must be positive")
    for source in (light_path, dark_path):
        if not source.is_file():
            raise SystemExit(f"wallpaper SVG not found: {source}")

    sources = [
        (UOWP_VARIANT_LIGHT, "light", light_path),
        (UOWP_VARIANT_DARK, "dark", dark_path),
    ]
    entries = []
    for variant, _name, source in sources:
        image = render_svg(source, args.width, args.height, variant)
        entries.append(
            {
                "variant": variant,
                "width": args.width,
                "height": args.height,
                "data": raw_bgra_bytes(image),
                "preview": png_preview_bytes(image, args.preview_width, args.preview_height),
            }
        )

    metadata = {
        "name": "Default",
        "author": "uniOS",
        "layout": "cover",
        "default_variant": "light",
        "dark_variant": "dark",
        "codec": "raw",
        "preview": "png",
    }
    write_uowp(output_path, entries, metadata)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
