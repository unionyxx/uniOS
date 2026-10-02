#!/usr/bin/env python3
# Renders sample text from a .uof font to PNG using the same math as the
# libgui runtime (26.6 pen, GPOS kerning, FIR phase filtering over the
# oversampled atlas, polarity gamma, linear-light blending). Host-side
# visual verification for font work; not part of the OS image.
from __future__ import annotations

import argparse
import struct
import sys

try:
    from PIL import Image
except ImportError:
    print("uof_preview.py requires Pillow: pip install Pillow", file=sys.stderr)
    raise

HEADER_FORMAT = "<4sHHHHHHhhhIIHHIIIII"
GLYPH_FORMAT = "<IHHHHhhhBB"
KERN_FORMAT = "<IIh"

# FIR weights for 16 subpixel phases (1/16 px steps). For phase p, taps at
# subcols [4k - tf - 1 .. 4k - tf + 3] with weights (r, 1, 1, 1, 1-r)/4,
# stored here in 1/64 units; coverage = sum >> 8.
FIR_WEIGHTS = []
for phase in range(16):
    t = phase * 4 // 16  # tf in 0..3 (phase/4 in subcolumn units)
    r = (phase * 4 % 16) * 64 // 16  # r in 0..60, 1/64 units
    w = [r, 64, 64, 64, 64 - r] if r else [0, 64, 64, 64, 64]
    FIR_WEIGHTS.append(w)

SRGB_TO_LINEAR = []
for v in range(256):
    u = v / 255.0
    lin = u / 12.92 if u <= 0.04045 else ((u + 0.055) / 1.055) ** 2.4
    SRGB_TO_LINEAR.append(round(lin * 255))
LINEAR_TO_SRGB = []
for v in range(256):
    u = v / 255.0
    s = u * 12.92 if u <= 0.0031308 else 1.055 * u ** (1 / 2.4) - 0.055
    LINEAR_TO_SRGB.append(round(s * 255))
GAMMA_LIGHT_ON_DARK = [round((c / 255) ** 2.0 * 255) for c in range(256)]


class UofFont:
    def __init__(self, path):
        data = open(path, "rb").read()
        (magic, version, flags, self.size, self.oversample, self.atlas_w, self.atlas_h, self.ascent, self.descent,
         self.line_gap, glyph_count, kern_count, self.matrix_c1, self.matrix_c2, fallback_index, glyph_off,
         kern_off, matrix_off, atlas_off) = struct.unpack_from(HEADER_FORMAT, data, 0)
        if magic != b"UOFN" or version != 2:
            raise ValueError(f"{path} is not a UOF v2 font")
        self.glyphs = {}
        for i in range(glyph_count):
            (cp, ax, ay, w, h, bx26, by, adv26, kl, kr) = struct.unpack_from(
                GLYPH_FORMAT, data, glyph_off + i * struct.calcsize(GLYPH_FORMAT))
            self.glyphs[cp] = {
                "ax": ax, "ay": ay, "w": w, "h": h, "bx26": bx26, "by": by, "adv26": adv26, "kl": kl, "kr": kr
            }
        self.fallback = self.glyphs[
            struct.unpack_from(GLYPH_FORMAT, data, glyph_off + fallback_index * struct.calcsize(GLYPH_FORMAT))[0]]
        self.kern = {}
        for i in range(kern_count):
            left, right, value = struct.unpack_from(KERN_FORMAT, data, kern_off + i * struct.calcsize(KERN_FORMAT))
            self.kern[(left, right)] = value
        c1, c2 = self.matrix_c1 + 1, self.matrix_c2 + 1
        flat = struct.unpack_from(f"<{c1 * c2}h", data, matrix_off)
        self.matrix = [flat[c2 * i : c2 * (i + 1)] for i in range(c1)]
        # RLE atlas
        stride = self.atlas_w * self.oversample
        expected = stride * self.atlas_h
        atlas = bytearray()
        i = atlas_off
        while i < len(data):
            run = data[i] + 1
            atlas.extend(bytes([data[i + 1]]) * run)
            i += 2
        if len(atlas) != expected:
            raise ValueError(f"atlas decode mismatch: {len(atlas)} != {expected}")
        self.atlas = atlas
        self.stride = stride

    def kern_26(self, left_cp, right_cp):
        g1, g2 = self.glyphs.get(left_cp), self.glyphs.get(right_cp)
        if g1 is None or g2 is None:
            return 0
        total = 0
        if g1["kl"] and g2["kr"]:
            total += self.matrix[g1["kl"]][g2["kr"]]
        total += self.kern.get((left_cp, right_cp), 0)
        return total


def render_text(font, text, px, py, fg, bg, out_w, out_h, enable_kern=True, enable_phase=True):
    bg_tuple = ((bg >> 16) & 255, (bg >> 8) & 255, bg & 255)
    img = Image.new("RGB", (out_w, out_h), bg_tuple)
    pixels = img.load()
    srgb_l = SRGB_TO_LINEAR
    lin_s = LINEAR_TO_SRGB
    gamma = GAMMA_LIGHT_ON_DARK if luma(fg) > luma(bg) else None

    pen26 = px * 64
    prev_cp = None
    text = text.encode("utf-8", "surrogateescape")
    i = 0
    while i < len(text):
        cp, adv_i = decode_utf8(text, i)
        i = adv_i
        glyph = font.glyphs.get(cp, font.fallback)
        if prev_cp is not None and enable_kern:
            pen26 += font.kern_26(prev_cp, cp)
        pos26 = pen26 + glyph["bx26"]
        phase = ((pos26 % 64) * 16) // 64 if enable_phase else 0
        draw_glyph(pixels, font, glyph, pos26, py, phase, fg, bg, gamma, srgb_l, lin_s, out_w, out_h)
        pen26 += glyph["adv26"]
        prev_cp = cp
    return img


def draw_glyph(pixels, font, glyph, pos26, baseline, phase, fg, bg, gamma, srgb_l, lin_s, out_w, out_h):
    os_ = font.oversample
    origin_x = pos26 >> 6  # floor; fractional part drives the FIR phase
    top_y = baseline + font.ascent - glyph["by"]
    fr, fgc, fb = (srgb_l[(fg >> 16) & 255], srgb_l[(fg >> 8) & 255], srgb_l[fg & 255])
    br, bgc, bb = (srgb_l[(bg >> 16) & 255], srgb_l[(bg >> 8) & 255], srgb_l[bg & 255])
    weights = FIR_WEIGHTS[phase]
    tf = phase * 4 // 16
    atlas, stride = font.atlas, font.stride
    glyph_sub_w = glyph["w"] * 4

    for row in range(glyph["h"]):
        dy = top_y + row
        if dy < 0 or dy >= out_h:
            continue
        src_row = (glyph["ay"] + row) * stride + glyph["ax"]
        for col in range(glyph["w"]):
            dx = origin_x + col
            if dx < 0 or dx >= out_w:
                continue
            if os_ > 1:
                n0 = col * 4 - tf - 1
                acc = 0
                for tap in range(5):
                    n = n0 + tap
                    if 0 <= n < glyph_sub_w:
                        acc += weights[tap] * atlas[src_row + n]
                coverage = acc >> 8
            else:
                coverage = atlas[src_row + col]
            if coverage == 0:
                continue
            if gamma is not None:
                coverage = gamma[coverage]
            alpha = coverage
            inv = 255 - alpha
            r = lin_s[(fr * alpha + br * inv + 127) // 255]
            g = lin_s[(fgc * alpha + bgc * inv + 127) // 255]
            b = lin_s[(fb * alpha + bb * inv + 127) // 255]
            pixels[dx, dy] = (r, g, b)


def luma(c):
    return ((c >> 16 & 255) * 54 + (c >> 8 & 255) * 183 + (c & 255) * 19 + 128) >> 8


def decode_utf8(text, i):
    """Decode one UTF-8 codepoint starting at i; invalid bytes map to U+FFFD."""
    b = text[i]
    if b < 0x80:
        return b, i + 1
    if 0xC0 <= b < 0xFE:
        length = 2 if b < 0xE0 else 3 if b < 0xF0 else 4
        chunk = text[i : i + length]
        try:
            return ord(chunk.decode("utf-8")), i + length
        except (UnicodeDecodeError, AttributeError):
            pass
    return 0xFFFD, i + 1


def main():
    parser = argparse.ArgumentParser(description="Render .uof text samples to PNG")
    parser.add_argument("font", help=".uof v2 font file")
    parser.add_argument("--text", default="Tova AV yo WAVE \u201cquoted\u201d \u2014 \u2026 caf\u00e9 \u03a3\u03a9 \u0416\u0414 \u250c\u2500\u2510 0123")
    parser.add_argument("--output", default="uof_preview.png")
    parser.add_argument("--scale", type=int, default=1, help="Integer upscale of the output PNG")
    parser.add_argument("--no-kern", action="store_true")
    parser.add_argument("--no-phase", action="store_true")
    args = parser.parse_args()

    font = UofFont(args.font)
    themes = (
        ((0x1A, 0x1D, 0x22), (0xE8, 0xEA, 0xED)),  # light text on dark bg
        ((0xF2, 0xF3, 0xF5), (0x14, 0x16, 0x18)),  # dark text on light bg
    )

    def pack(c):
        return (c[0] << 16) | (c[1] << 8) | c[2]

    height = font.ascent + font.descent + font.line_gap
    width = 900
    rows = []
    # Each theme renders twice: with kerning and phase (runtime behavior),
    # then without kerning, to eyeball what the kern table buys.
    for bg, fg in themes:
        rows.append(render_text(font, args.text, 8, 4, pack(fg), pack(bg), width, height + 8,
                                enable_kern=not args.no_kern, enable_phase=not args.no_phase))
        rows.append(render_text(font, args.text, 8, 4, pack(fg), pack(bg), width, height + 8,
                                enable_kern=False, enable_phase=not args.no_phase))

    out = Image.new("RGB", (width, sum(im.height for im in rows)), themes[0][0])
    y = 0
    for im in rows:
        out.paste(im, (0, y))
        y += im.height
    if args.scale > 1:
        out = out.resize((out.width * args.scale, out.height * args.scale), Image.NEAREST)
    out.save(args.output)
    print(f"wrote {args.output} ({out.width}x{out.height}); "
          "rows: light-kerned, light-unkerned, dark-kerned, dark-unkerned")


if __name__ == "__main__":
    raise SystemExit(main())
