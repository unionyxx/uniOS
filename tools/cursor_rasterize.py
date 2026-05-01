#!/usr/bin/env python3

import argparse
import io
import json
import os
import shutil
import struct
from pathlib import Path

from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    from PySide6.QtCore import QRectF
    from PySide6.QtGui import QGuiApplication, QImage, QPainter
    from PySide6.QtSvg import QSvgRenderer
except Exception:
    QRectF = None
    QGuiApplication = None
    QImage = None
    QPainter = None
    QSvgRenderer = None

try:
    import cairosvg
except Exception:
    cairosvg = None


UOCU_MAGIC = 0x55434F55  # "UOCU", little-endian
UOCU_VERSION = 1
UOCU_CODEC_QOI = 1
UOCU_VARIANT_DEFAULT = 0
UOCU_ROLE_ARROW = 0
UOCU_ROLE_IBEAM = 1
UOCU_ROLE_HAND = 2
UOCU_ROLE_CROSSHAIR = 3
UOCU_ROLE_WAIT = 4
UOCU_ROLE_PROGRESS = 5
UOCU_ROLE_MOVE = 6
UOCU_ROLE_RESIZE_NS = 7
UOCU_ROLE_RESIZE_EW = 8
UOCU_ROLE_RESIZE_NESW = 9
UOCU_ROLE_RESIZE_NWSE = 10
UOCU_ROLE_NOT_ALLOWED = 11
QOI_END_MARKER = b"\x00\x00\x00\x00\x00\x00\x00\x01"


_qt_app = None


ROLE_BY_CURSOR_DIR = {
    "all-scroll": UOCU_ROLE_MOVE,
    "cell": UOCU_ROLE_CROSSHAIR,
    "copy": UOCU_ROLE_HAND,
    "crosshair": UOCU_ROLE_CROSSHAIR,
    "default": UOCU_ROLE_ARROW,
    "ew-resize": UOCU_ROLE_RESIZE_EW,
    "grab": UOCU_ROLE_HAND,
    "grabbing": UOCU_ROLE_HAND,
    "move": UOCU_ROLE_MOVE,
    "nesw-resize": UOCU_ROLE_RESIZE_NESW,
    "no-drop": UOCU_ROLE_NOT_ALLOWED,
    "not-allowed": UOCU_ROLE_NOT_ALLOWED,
    "ns-resize": UOCU_ROLE_RESIZE_NS,
    "nwse-resize": UOCU_ROLE_RESIZE_NWSE,
    "pointer": UOCU_ROLE_HAND,
    "progress": UOCU_ROLE_PROGRESS,
    "text": UOCU_ROLE_IBEAM,
    "vertical-text": UOCU_ROLE_IBEAM,
    "wait": UOCU_ROLE_WAIT,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render SVG cursor sources into UOCU cursor packages.")
    parser.add_argument("--source-root", required=True, help="Directory containing cursor source folders.")
    parser.add_argument("--output-root", required=True, help="Directory to place generated .uocu cursor packages.")
    parser.add_argument("--sizes", required=True, nargs="+", type=int, help="Square cursor sizes to render.")
    return parser.parse_args()


def load_metadata(path: Path) -> list[dict]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, list):
        raise ValueError(f"{path} must contain a JSON array")
    return data


def render_svg(svg_path: Path, size: int) -> Image.Image:
    global _qt_app

    if QSvgRenderer is not None and QImage is not None and QPainter is not None:
        if _qt_app is None:
            _qt_app = QGuiApplication.instance() or QGuiApplication([])
        renderer = QSvgRenderer(str(svg_path))
        if not renderer.isValid():
            raise ValueError(f"Invalid SVG: {svg_path}")

        image = QImage(size, size, QImage.Format_ARGB32)
        image.fill(0)
        painter = QPainter(image)
        renderer.render(painter, QRectF(0, 0, size, size))
        painter.end()

        out = Image.new("RGBA", (size, size))
        for y in range(size):
            for x in range(size):
                color = image.pixelColor(x, y)
                out.putpixel((x, y), (color.red(), color.green(), color.blue(), color.alpha()))
        return out

    if cairosvg is None:
        raise RuntimeError("No SVG renderer is available. Install PySide6 QtSvg or CairoSVG with native Cairo.")

    png_bytes = cairosvg.svg2png(url=str(svg_path), output_width=size, output_height=size)
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def resolve_svg_path(cursor_dir: Path, filename: str) -> Path:
    direct = cursor_dir / filename
    if direct.is_file():
        return direct

    alternate = cursor_dir / filename.replace("-", "_")
    if alternate.is_file():
        return alternate

    raise FileNotFoundError(f"Missing cursor SVG: {direct}")


def premultiply_rgba(image: Image.Image) -> Image.Image:
    image = image.convert("RGBA")
    pixels = bytearray(image.tobytes())
    for i in range(0, len(pixels), 4):
        alpha = pixels[i + 3]
        if alpha == 0:
            pixels[i] = 0
            pixels[i + 1] = 0
            pixels[i + 2] = 0
        elif alpha < 255:
            pixels[i] = (pixels[i] * alpha + 127) // 255
            pixels[i + 1] = (pixels[i + 1] * alpha + 127) // 255
            pixels[i + 2] = (pixels[i + 2] * alpha + 127) // 255
    return Image.frombytes("RGBA", image.size, bytes(pixels))


def qoi_hash(px: tuple[int, int, int, int]) -> int:
    r, g, b, a = px
    return (r * 3 + g * 5 + b * 7 + a * 11) % 64


def qoi_encode_rgba(image: Image.Image) -> bytes:
    image = image.convert("RGBA")
    width, height = image.size
    pixels = list(image.getdata())
    out = bytearray()
    out.extend(b"qoif")
    out.extend(struct.pack(">II", width, height))
    out.extend(bytes((4, 0)))

    index = [(0, 0, 0, 0)] * 64
    prev = (0, 0, 0, 255)
    run = 0

    for px in pixels:
        if px == prev:
            run += 1
            if run == 62:
                out.append(0xC0 | (run - 1))
                run = 0
            continue

        if run:
            out.append(0xC0 | (run - 1))
            run = 0

        idx = qoi_hash(px)
        if index[idx] == px:
            out.append(idx)
        else:
            index[idx] = px
            r, g, b, a = px
            pr, pg, pb, pa = prev
            if a == pa:
                dr = r - pr
                dg = g - pg
                db = b - pb
                dr_dg = dr - dg
                db_dg = db - dg
                if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                    out.append(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2))
                elif -32 <= dg <= 31 and -8 <= dr_dg <= 7 and -8 <= db_dg <= 7:
                    out.append(0x80 | (dg + 32))
                    out.append(((dr_dg + 8) << 4) | (db_dg + 8))
                else:
                    out.extend((0xFE, r, g, b))
            else:
                out.extend((0xFF, r, g, b, a))

        prev = px

    if run:
        out.append(0xC0 | (run - 1))

    out.extend(QOI_END_MARKER)
    return bytes(out)


def scaled_hotspot(value: int, size: int, nominal_size: int) -> int:
    if nominal_size <= 0:
        return value
    return max(0, min(size - 1, (value * size + nominal_size // 2) // nominal_size))


def write_uocu(path: Path, role: int, rendered_entries: list[dict], metadata: dict) -> None:
    header_size = 28
    entry_size = 36
    entry_count = len(rendered_entries)
    directory_offset = header_size
    directory_size = entry_count * entry_size
    data_offset = header_size + directory_size

    entries = bytearray()
    blobs = bytearray()
    for rendered in rendered_entries:
        data = rendered["data"]
        width = rendered["width"]
        height = rendered["height"]
        decoded_size = width * height * 4
        entries.extend(
            struct.pack(
                "<HHHHHHHHIIIII",
                width,
                height,
                100,
                UOCU_VARIANT_DEFAULT,
                role,
                UOCU_CODEC_QOI,
                rendered["hotspot_x"],
                rendered["hotspot_y"],
                rendered["frame_duration_ms"],
                data_offset,
                len(data),
                decoded_size,
                0,
            )
        )
        blobs.extend(data)
        data_offset += len(data)

    metadata_blob = json.dumps(metadata, separators=(",", ":"), sort_keys=True).encode("utf-8")
    metadata_offset = data_offset if metadata_blob else 0

    header = struct.pack(
        "<IHHIIIII",
        UOCU_MAGIC,
        UOCU_VERSION,
        0,
        entry_count,
        directory_offset,
        directory_size,
        metadata_offset,
        len(metadata_blob),
    )
    path.write_bytes(header + entries + blobs + metadata_blob)


def main() -> int:
    args = parse_args()
    source_root = Path(args.source_root)
    output_root = Path(args.output_root)
    sizes = sorted({size for size in args.sizes if size > 0})

    if not source_root.is_dir():
        raise SystemExit(f"Cursor source root not found: {source_root}")
    if not sizes:
        raise SystemExit("At least one positive cursor size is required")

    shutil.rmtree(output_root, ignore_errors=True)
    output_root.mkdir(parents=True, exist_ok=True)

    for cursor_dir in sorted(path for path in source_root.iterdir() if path.is_dir()):
        metadata_path = cursor_dir / "metadata.json"
        if not metadata_path.is_file():
            continue

        role = ROLE_BY_CURSOR_DIR.get(cursor_dir.name, UOCU_ROLE_ARROW)
        source_entries = load_metadata(metadata_path)
        rendered_entries: list[dict] = []
        for size in sizes:
            for source_entry in source_entries:
                filename = source_entry.get("filename")
                if not filename:
                    continue
                svg_path = resolve_svg_path(cursor_dir, filename)
                nominal_size = int(source_entry.get("nominal_size", 24))
                image = premultiply_rgba(render_svg(svg_path, size))
                rendered_entries.append(
                    {
                        "width": size,
                        "height": size,
                        "hotspot_x": scaled_hotspot(int(source_entry.get("hotspot_x", 0)), size, nominal_size),
                        "hotspot_y": scaled_hotspot(int(source_entry.get("hotspot_y", 0)), size, nominal_size),
                        "frame_duration_ms": int(source_entry.get("delay", 0)),
                        "data": qoi_encode_rgba(image),
                    }
                )

        package_metadata = {
            "name": cursor_dir.name,
            "role": role,
            "codec": "qoi",
            "variant": "default",
        }
        write_uocu(output_root / f"{cursor_dir.name}.uocu", role, rendered_entries, package_metadata)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
