#!/usr/bin/env python3

from __future__ import annotations

import argparse
import io
import math
import os
import re
import shutil
import struct
import sys
import tempfile
from pathlib import Path
from typing import Optional, Tuple

from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

try:
    import numpy as np
except Exception:  # Optional. Without numpy we fall back to high-quality Pillow resizing.
    np = None

try:
    import cairosvg
except Exception:
    cairosvg = None

try:
    from PySide6.QtCore import QByteArray, QRectF
    from PySide6.QtGui import QGuiApplication, QImage, QPainter
    from PySide6.QtSvg import QSvgRenderer
except Exception:
    QByteArray = None
    QRectF = None
    QGuiApplication = None
    QImage = None
    QPainter = None
    QSvgRenderer = None


_qt_app = None

try:
    RESAMPLE = Image.Resampling
except AttributeError:  # Pillow < 9.1
    class _Resample:
        BOX = Image.BOX
        BICUBIC = Image.BICUBIC
        LANCZOS = Image.LANCZOS
    RESAMPLE = _Resample()


UOIC_MAGIC = 0x43494F55  # "UOIC", little-endian
UOIC_VERSION = 1
UOIC_CODEC_QOI = 1
UOIC_VARIANT_DEFAULT = 0
UOIC_ALPHA_STRAIGHT = 1
UOIC_ALPHA_PREMULTIPLIED = 2
QOI_END_MARKER = b"\x00\x00\x00\x00\x00\x00\x00\x01"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render SVG app icons into UOIC runtime assets.")
    parser.add_argument("--source-root", required=True, help="Directory containing SVG app icon sources.")
    parser.add_argument("--output-root", required=True, help="Directory to place generated .uoic app icons.")
    parser.add_argument("--sizes", required=True, nargs="+", type=int, help="Square icon sizes to render.")

    parser.add_argument(
        "--renderer",
        choices=("auto", "cairo", "qt"),
        default="auto",
        help="SVG renderer. 'auto' prefers CairoSVG because it handles SVG 1.1 / Figma-style filters better.",
    )
    parser.add_argument(
        "--supersample",
        type=int,
        default=8,
        help="Render SVGs this many times larger before downsampling. Use 8 or 12 for small icons.",
    )
    parser.add_argument(
        "--filter",
        choices=("box", "lanczos", "bicubic"),
        default="lanczos",
        help=(
            "Downsampling filter. 'lanczos' is sharpest for app icons; 'box' gives clean coverage antialiasing and less ringing. "
            ""
        ),
    )
    parser.add_argument(
        "--color-space",
        choices=("linear", "srgb"),
        default="linear",
        help="Downsample colors in linear light when numpy is available. Falls back to sRGB if numpy is missing.",
    )
    parser.add_argument(
        "--alpha",
        choices=("premultiplied", "straight", "opaque"),
        default="premultiplied",
        help=(
            "Pixel alpha layout stored in UOIC frames. The runtime loader normalizes UOIC "
            "frames to premultiplied BGRA8888; use 'opaque' with --background."
        ),
    )
    parser.add_argument(
        "--background",
        default=None,
        help="Optional #RRGGBB background to flatten against.",
    )
    parser.add_argument(
        "--no-sanitize-svg",
        action="store_true",
        help="Disable small SVG cleanup pass for invalid NaN gradient coordinates exported by some design tools.",
    )
    parser.add_argument(
        "--emit-png-preview",
        action="store_true",
        help="Also write PNG previews under output-root/<size>/ for quick visual inspection.",
    )
    return parser.parse_args()


def resampling_filter(name: str) -> int:
    if name == "box":
        return RESAMPLE.BOX
    if name == "bicubic":
        return RESAMPLE.BICUBIC
    return RESAMPLE.LANCZOS


def parse_background(value: Optional[str]) -> Optional[Tuple[int, int, int, int]]:
    if value is None:
        return None
    text = value.strip()
    if text.startswith("#"):
        text = text[1:]
    if len(text) != 6:
        raise ValueError("--background must be a #RRGGBB color")
    try:
        r = int(text[0:2], 16)
        g = int(text[2:4], 16)
        b = int(text[4:6], 16)
    except ValueError as exc:
        raise ValueError("--background must be a valid #RRGGBB color") from exc
    return (r, g, b, 255)


def sanitize_svg_text(text: str) -> str:
    broken_gradient_ids = {}

    gradient_re = re.compile(
        r'(<linearGradient\b(?P<attrs>[^>]*)>)(?P<body>.*?</linearGradient>)',
        flags=re.IGNORECASE | re.DOTALL,
    )

    def first_stop_color(body: str) -> str:
        m = re.search(r'stop-color="([^"]+)"', body, flags=re.IGNORECASE)
        if m:
            return m.group(1)
        m = re.search(r'stop-color\s*:\s*([^;"\']+)', body, flags=re.IGNORECASE)
        if m:
            return m.group(1).strip()
        return "#FFFFFF"

    for match in gradient_re.finditer(text):
        attrs = match.group("attrs")
        if "nan" not in attrs.lower():
            continue
        id_match = re.search(r'id="([^"]+)"', attrs, flags=re.IGNORECASE)
        if not id_match:
            continue
        broken_gradient_ids[id_match.group(1)] = first_stop_color(match.group("body"))

    for gradient_id, color in broken_gradient_ids.items():
        text = re.sub(rf'url\(\s*#{re.escape(gradient_id)}\s*\)', color, text)

    # Last-resort cleanup for any remaining NaN tokens in numeric attributes.
    text = re.sub(r'(?i)([-+]?nan)', "0", text)
    return text


def read_svg_bytes(svg_path: Path, sanitize: bool = True) -> bytes:
    text = svg_path.read_text(encoding="utf-8")
    if sanitize:
        cleaned = sanitize_svg_text(text)
        if cleaned != text:
            print(f"[warn] sanitized invalid numeric SVG data in {svg_path.name}", file=sys.stderr)
        text = cleaned
    return text.encode("utf-8")


def qimage_to_pillow_rgba(image: "QImage") -> Image.Image:
    image = image.convertToFormat(QImage.Format_RGBA8888)
    width = image.width()
    height = image.height()
    bytes_per_line = image.bytesPerLine()
    expected_stride = width * 4

    bits = image.constBits()
    try:
        bits.setsize(image.sizeInBytes())  # PyQt compatibility
    except AttributeError:
        pass
    data = bytes(bits)

    if bytes_per_line == expected_stride:
        return Image.frombytes("RGBA", (width, height), data)

    rows = bytearray()
    for y in range(height):
        start = y * bytes_per_line
        rows.extend(data[start : start + expected_stride])
    return Image.frombytes("RGBA", (width, height), bytes(rows))


def render_svg_with_cairo(svg_bytes: bytes, render_size: int) -> Image.Image:
    if cairosvg is None:
        raise RuntimeError("CairoSVG is not available")
    png_bytes = cairosvg.svg2png(
        bytestring=svg_bytes,
        output_width=render_size,
        output_height=render_size,
    )
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def render_svg_with_qt(svg_bytes: bytes, render_size: int) -> Image.Image:
    global _qt_app

    if QSvgRenderer is None or QImage is None or QPainter is None or QRectF is None:
        raise RuntimeError("PySide6 QtSvg is not available")

    if _qt_app is None:
        _qt_app = QGuiApplication.instance() or QGuiApplication([])

    renderer = QSvgRenderer(QByteArray(svg_bytes) if QByteArray is not None else svg_bytes)
    if not renderer.isValid():
        raise ValueError("Invalid SVG")

    image = QImage(render_size, render_size, QImage.Format_ARGB32_Premultiplied)
    image.fill(0)

    painter = QPainter(image)
    painter.setRenderHint(QPainter.Antialiasing, True)
    painter.setRenderHint(QPainter.TextAntialiasing, True)
    painter.setRenderHint(QPainter.SmoothPixmapTransform, True)
    renderer.render(painter, QRectF(0, 0, render_size, render_size))
    painter.end()

    return qimage_to_pillow_rgba(image)


def render_svg(svg_path: Path, size: int, supersample: int, renderer: str, sanitize: bool) -> Image.Image:
    render_size = max(size, size * max(1, supersample))
    svg_bytes = read_svg_bytes(svg_path, sanitize=sanitize)

    if renderer == "cairo":
        return render_svg_with_cairo(svg_bytes, render_size)
    if renderer == "qt":
        return render_svg_with_qt(svg_bytes, render_size)

    # Auto mode: CairoSVG first because Figma-style SVGs often use SVG 1.1
    # gradients/filters beyond Qt's Tiny profile. Fall back to Qt if Cairo is absent.
    if cairosvg is not None:
        try:
            return render_svg_with_cairo(svg_bytes, render_size)
        except Exception as exc:
            print(f"[warn] CairoSVG failed for {svg_path.name}: {exc}; falling back to Qt", file=sys.stderr)

    return render_svg_with_qt(svg_bytes, render_size)


def srgb_to_linear(arr):
    return np.where(arr <= 0.04045, arr / 12.92, ((arr + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(arr):
    return np.where(arr <= 0.0031308, arr * 12.92, 1.055 * np.power(np.maximum(arr, 0.0), 1.0 / 2.4) - 0.055)


def resize_channel_f32(channel, target_size: int, filt: int):
    img = Image.fromarray(channel.astype("float32"), mode="F")
    return np.asarray(img.resize((target_size, target_size), filt), dtype=np.float32)


def downsample_rgba(
    image: Image.Image,
    target_size: int,
    filt: int,
    color_space: str = "linear",
    alpha_mode: str = "straight",
) -> Image.Image:
    """Resize RGBA while respecting alpha and avoiding dark/bright edge fringes."""
    image = image.convert("RGBA")
    if image.size == (target_size, target_size):
        # Still apply alpha premultiplication if requested.
        if alpha_mode == "premultiplied":
            return convert_alpha_layout(image, "premultiplied")
        return image

    if color_space == "linear" and np is not None:
        src = np.asarray(image, dtype=np.float32) / 255.0
        rgb = src[:, :, :3]
        alpha = src[:, :, 3:4]

        rgb_work = srgb_to_linear(rgb)
        pm = rgb_work * alpha

        channels = [resize_channel_f32(pm[:, :, i], target_size, filt) for i in range(3)]
        alpha_r = resize_channel_f32(alpha[:, :, 0], target_size, filt)
        pm_r = np.stack(channels, axis=2)
        alpha_r = np.clip(alpha_r, 0.0, 1.0)

        # Avoid color noise in numerically-transparent pixels.
        tiny = alpha_r < (0.5 / 255.0)
        safe_alpha = np.where(tiny, 1.0, alpha_r)

        straight_linear = np.clip(pm_r / safe_alpha[:, :, None], 0.0, 1.0)
        straight_srgb = np.clip(linear_to_srgb(straight_linear), 0.0, 1.0)

        if alpha_mode == "premultiplied":
            # Runtime premultiplied BGRA surfaces expect 8-bit sRGB values that
            # have been multiplied by alpha numerically.
            out_rgb = straight_srgb * alpha_r[:, :, None]
        else:
            out_rgb = straight_srgb

        out = np.dstack([out_rgb, alpha_r])
        out[tiny, :3] = 0.0
        out[tiny, 3] = 0.0
        out_u8 = np.clip(np.round(out * 255.0), 0, 255).astype("uint8")
        return Image.fromarray(out_u8, "RGBA")

    # Fallback: premultiply in byte/sRGB space before filtering.
    r, g, b, a = image.split()
    pm = Image.merge(
        "RGBA",
        (
            Image.eval(Image.merge("L", (r,)), lambda v: v),  # dummy to keep old Pillow happy
            Image.eval(Image.merge("L", (g,)), lambda v: v),
            Image.eval(Image.merge("L", (b,)), lambda v: v),
            a,
        ),
    )
    # Manual premultiply using point math keeps Pillow-only fallback dependency-free.
    import PIL.ImageChops as ImageChops

    pm = Image.merge("RGBA", (ImageChops.multiply(r, a), ImageChops.multiply(g, a), ImageChops.multiply(b, a), a))
    reduced = pm.resize((target_size, target_size), filt)

    out = Image.new("RGBA", (target_size, target_size))
    src = reduced.load()
    dst = out.load()
    for y in range(target_size):
        for x in range(target_size):
            pr, pg, pb, pa = src[x, y]
            if pa <= 0:
                dst[x, y] = (0, 0, 0, 0)
            elif alpha_mode == "premultiplied":
                dst[x, y] = (pr, pg, pb, pa)
            else:
                dst[x, y] = (
                    min(255, int(round(pr * 255 / pa))),
                    min(255, int(round(pg * 255 / pa))),
                    min(255, int(round(pb * 255 / pa))),
                    pa,
                )
    return out


def convert_alpha_layout(image: Image.Image, alpha_mode: str) -> Image.Image:
    image = image.convert("RGBA")
    if alpha_mode != "premultiplied":
        return image
    if np is not None:
        arr = np.asarray(image, dtype=np.float32)
        alpha = arr[:, :, 3:4] / 255.0
        arr[:, :, :3] = np.round(arr[:, :, :3] * alpha)
        return Image.fromarray(np.clip(arr, 0, 255).astype("uint8"), "RGBA")

    import PIL.ImageChops as ImageChops
    r, g, b, a = image.split()
    return Image.merge("RGBA", (ImageChops.multiply(r, a), ImageChops.multiply(g, a), ImageChops.multiply(b, a), a))


def flatten_if_requested(image: Image.Image, background: Optional[Tuple[int, int, int, int]]) -> Image.Image:
    image = image.convert("RGBA")
    if background is None:
        return image
    base = Image.new("RGBA", image.size, background)
    return Image.alpha_composite(base, image)


def qoi_hash(px: Tuple[int, int, int, int]) -> int:
    r, g, b, a = px
    return (r * 3 + g * 5 + b * 7 + a * 11) % 64


def qoi_encode_rgba(image: Image.Image) -> bytes:
    image = image.convert("RGBA")
    width, height = image.size
    pixels = list(image.getdata())
    out = bytearray(struct.pack(">4sIIBB", b"qoif", width, height, 4, 0))

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

        index_pos = qoi_hash(px)
        if index[index_pos] == px:
            out.append(index_pos)
        else:
            index[index_pos] = px
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


def uoic_alpha_value(alpha_mode: str) -> int:
    if alpha_mode == "premultiplied":
        return UOIC_ALPHA_PREMULTIPLIED
    return UOIC_ALPHA_STRAIGHT


def write_uoic(path: Path, frames: list[Tuple[int, Image.Image]], alpha_mode: str) -> None:
    encoded_frames = [(size, qoi_encode_rgba(image)) for size, image in frames]
    entry_count = len(encoded_frames)
    header_size = 28
    entry_size = 28
    directory_offset = header_size
    directory_size = entry_count * entry_size
    data_offset = header_size + directory_size

    entries = bytearray()
    blobs = bytearray()
    alpha = uoic_alpha_value(alpha_mode)
    for size, data in encoded_frames:
        decoded_size = size * size * 4
        entries.extend(
            struct.pack(
                "<HHHHHHIIII",
                size,
                size,
                100,
                UOIC_VARIANT_DEFAULT,
                UOIC_CODEC_QOI,
                alpha,
                data_offset,
                len(data),
                decoded_size,
                0,
            )
        )
        blobs.extend(data)
        data_offset += len(data)

    header = struct.pack(
        "<IHHIIIII",
        UOIC_MAGIC,
        UOIC_VERSION,
        0,
        entry_count,
        directory_offset,
        directory_size,
        0,
        0,
    )
    path.write_bytes(header + entries + blobs)


def main() -> int:
    args = parse_args()
    source_root = Path(args.source_root)
    output_root = Path(args.output_root)
    sizes = sorted({size for size in args.sizes if size > 0})
    background = parse_background(args.background)
    filt = resampling_filter(args.filter)

    if not source_root.is_dir():
        raise SystemExit(f"App icon source root not found: {source_root}")
    if not sizes:
        raise SystemExit("At least one positive icon size is required")
    if args.color_space == "linear" and np is None:
        print("[warn] numpy is not installed; falling back to sRGB byte-space resizing", file=sys.stderr)
    if args.alpha == "opaque" and background is None:
        raise SystemExit("--alpha opaque requires --background #RRGGBB")

    shutil.rmtree(output_root, ignore_errors=True)
    output_root.mkdir(parents=True, exist_ok=True)

    for svg_path in sorted(source_root.glob("*.svg")):
        frames: list[Tuple[int, Image.Image]] = []
        for size in sizes:
            high_res = render_svg(
                svg_path,
                size=size,
                supersample=args.supersample,
                renderer=args.renderer,
                sanitize=not args.no_sanitize_svg,
            )
            frame_alpha = "straight" if background is not None or args.alpha == "opaque" else args.alpha
            image = downsample_rgba(
                high_res,
                target_size=size,
                filt=filt,
                color_space=args.color_space,
                alpha_mode=frame_alpha,
            )
            image = flatten_if_requested(image, background)
            if args.alpha == "opaque":
                image = image.convert("RGB").convert("RGBA")
                # Force alpha fully opaque after flatten.
                if np is not None:
                    arr = np.asarray(image, dtype=np.uint8).copy()
                    arr[:, :, 3] = 255
                    image = Image.fromarray(arr, "RGBA")
                else:
                    r, g, b, _a = image.split()
                    image = Image.merge("RGBA", (r, g, b, Image.new("L", image.size, 255)))

            frames.append((size, image))

            if args.emit_png_preview:
                size_dir = output_root / str(size)
                size_dir.mkdir(parents=True, exist_ok=True)

            if args.emit_png_preview:
                preview_path = size_dir / f"{svg_path.stem}.png"
                # PNGs should be straight-alpha for accurate inspection in normal viewers.
                if args.alpha == "premultiplied" and background is None:
                    # Re-render as straight preview rather than trying to unpremultiply quantized frame pixels.
                    preview_image = downsample_rgba(
                        high_res,
                        target_size=size,
                        filt=filt,
                        color_space=args.color_space,
                        alpha_mode="straight",
                    )
                    preview_image.save(preview_path)
                else:
                    image.save(preview_path)

        write_uoic(output_root / f"{svg_path.stem}.uoic", frames, args.alpha)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
