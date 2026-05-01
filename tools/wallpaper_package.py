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
except Exception:
    cairosvg = None


UOWP_MAGIC = 0x50574F55  # "UOWP", little-endian
UOWP_VERSION = 1
UOWP_CODEC_RAW = 4
UOWP_VARIANT_LIGHT = 1
UOWP_VARIANT_DARK = 2
UOWP_COLOR_SRGB = 1
UOWP_TRANSFER_SDR = 1


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


def render_svg(svg_path: Path, width: int, height: int) -> Image.Image:
    if cairosvg is None:
        raise RuntimeError("wallpaper_package.py requires CairoSVG to render SVG wallpapers")
    png_bytes = cairosvg.svg2png(url=str(svg_path), output_width=width, output_height=height)
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def raw_bgra_bytes(image: Image.Image) -> bytes:
    image = image.convert("RGBA")
    raw = bytearray()
    for r, g, b, a in image.getdata():
        raw.extend((b, g, r, a))
    return bytes(raw)


def png_preview_bytes(image: Image.Image, width: int, height: int) -> bytes:
    preview = image.convert("RGBA").resize((width, height), Image.Resampling.LANCZOS)
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

    sources = [
        (UOWP_VARIANT_LIGHT, "light", light_path),
        (UOWP_VARIANT_DARK, "dark", dark_path),
    ]
    entries = []
    for variant, _name, source in sources:
        image = render_svg(source, args.width, args.height)
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
