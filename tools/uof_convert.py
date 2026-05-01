#!/usr/bin/env python3
import argparse
import ctypes
import math
import os
import struct
import sys
from dataclasses import dataclass
from typing import List


MAGIC = b"UOFN"
VERSION = 1
GLYPH_FORMAT = "<IHHHHhhhh"
HEADER_FORMAT = "<4sHHIHHhhhIIIIII"
KERNING_FORMAT = "<IIh"
GLYPH_RECORD_SIZE = struct.calcsize(GLYPH_FORMAT)
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
ASCII_START = 32
ASCII_END = 126
DEFAULT_PADDING = 2
ATLAS_ROW_LIMIT = 1024


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


class CGSize(ctypes.Structure):
    _fields_ = [("width", ctypes.c_double), ("height", ctypes.c_double)]


class CGRect(ctypes.Structure):
    _fields_ = [("origin", CGPoint), ("size", CGSize)]


@dataclass
class GlyphBitmap:
    codepoint: int
    width: int
    height: int
    bearing_x: int
    bearing_y: int
    advance_x: int
    pixels: bytes
    atlas_x: int = 0
    atlas_y: int = 0


class CoreTextRasterizer:
    def __init__(self) -> None:
        if sys.platform != "darwin":
            raise RuntimeError("uof_convert.py currently requires macOS CoreText/CoreGraphics")

        self.cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        self.cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        self.ct = ctypes.CDLL("/System/Library/Frameworks/CoreText.framework/CoreText")
        self.k_cf_allocator_default = ctypes.c_void_p.in_dll(self.cf, "kCFAllocatorDefault")
        self._bind()

    def _bind(self) -> None:
        self.cf.CFDataCreate.restype = ctypes.c_void_p
        self.cf.CFDataCreate.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]

        self.cg.CGDataProviderCreateWithCFData.restype = ctypes.c_void_p
        self.cg.CGDataProviderCreateWithCFData.argtypes = [ctypes.c_void_p]
        self.cg.CGFontCreateWithDataProvider.restype = ctypes.c_void_p
        self.cg.CGFontCreateWithDataProvider.argtypes = [ctypes.c_void_p]
        self.cg.CGColorSpaceCreateDeviceGray.restype = ctypes.c_void_p
        self.cg.CGColorSpaceCreateDeviceGray.argtypes = []
        self.cg.CGBitmapContextCreate.restype = ctypes.c_void_p
        self.cg.CGBitmapContextCreate.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
        self.cg.CGContextSetGrayFillColor.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double]
        self.cg.CGContextFillRect.argtypes = [ctypes.c_void_p, CGRect]
        self.cg.CGColorSpaceRelease.argtypes = [ctypes.c_void_p]
        self.cg.CGContextRelease.argtypes = [ctypes.c_void_p]
        self.cg.CGFontRelease.argtypes = [ctypes.c_void_p]
        self.cg.CGDataProviderRelease.argtypes = [ctypes.c_void_p]

        self.ct.CTFontCreateWithGraphicsFont.restype = ctypes.c_void_p
        self.ct.CTFontCreateWithGraphicsFont.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_void_p, ctypes.c_void_p]
        self.ct.CTFontGetGlyphsForCharacters.restype = ctypes.c_bool
        self.ct.CTFontGetGlyphsForCharacters.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.c_size_t,
        ]
        self.ct.CTFontGetBoundingRectsForGlyphs.restype = CGRect
        self.ct.CTFontGetBoundingRectsForGlyphs.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.POINTER(CGRect),
            ctypes.c_size_t,
        ]
        self.ct.CTFontGetAdvancesForGlyphs.restype = ctypes.c_double
        self.ct.CTFontGetAdvancesForGlyphs.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.POINTER(CGSize),
            ctypes.c_size_t,
        ]
        self.ct.CTFontGetAscent.restype = ctypes.c_double
        self.ct.CTFontGetAscent.argtypes = [ctypes.c_void_p]
        self.ct.CTFontGetDescent.restype = ctypes.c_double
        self.ct.CTFontGetDescent.argtypes = [ctypes.c_void_p]
        self.ct.CTFontGetLeading.restype = ctypes.c_double
        self.ct.CTFontGetLeading.argtypes = [ctypes.c_void_p]
        self.ct.CTFontDrawGlyphs.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.POINTER(CGPoint),
            ctypes.c_size_t,
            ctypes.c_void_p,
        ]
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]

    def load_font(self, font_path: str, pixel_size: int):
        with open(font_path, "rb") as handle:
            raw = handle.read()
        if not raw:
            raise RuntimeError(f"font file is empty: {font_path}")

        raw_array = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
        cf_data = self.cf.CFDataCreate(self.k_cf_allocator_default, raw_array, len(raw))
        if not cf_data:
            raise RuntimeError(f"failed to create CFData for {font_path}")

        provider = self.cg.CGDataProviderCreateWithCFData(cf_data)
        if not provider:
            self.cf.CFRelease(cf_data)
            raise RuntimeError(f"failed to create data provider for {font_path}")

        cg_font = self.cg.CGFontCreateWithDataProvider(provider)
        if not cg_font:
            self.cg.CGDataProviderRelease(provider)
            self.cf.CFRelease(cf_data)
            raise RuntimeError(f"failed to create CGFont for {font_path}")

        ct_font = self.ct.CTFontCreateWithGraphicsFont(cg_font, float(pixel_size), None, None)
        if not ct_font:
            self.cg.CGFontRelease(cg_font)
            self.cg.CGDataProviderRelease(provider)
            self.cf.CFRelease(cf_data)
            raise RuntimeError(f"failed to create CTFont for {font_path}")

        return {
            "ct_font": ct_font,
            "cg_font": cg_font,
            "provider": provider,
            "cf_data": cf_data,
            "ascent": self.ct.CTFontGetAscent(ct_font),
            "descent": self.ct.CTFontGetDescent(ct_font),
            "leading": self.ct.CTFontGetLeading(ct_font),
        }

    def close_font(self, font_info) -> None:
        self.cf.CFRelease(font_info["ct_font"])
        self.cg.CGFontRelease(font_info["cg_font"])
        self.cg.CGDataProviderRelease(font_info["provider"])
        self.cf.CFRelease(font_info["cf_data"])

    def rasterize_glyph(self, font_info, codepoint: int, padding: int) -> GlyphBitmap:
        chars = (ctypes.c_uint16 * 1)(codepoint)
        glyphs = (ctypes.c_uint16 * 1)()
        if not self.ct.CTFontGetGlyphsForCharacters(font_info["ct_font"], chars, glyphs, 1):
            raise RuntimeError(f"could not map codepoint U+{codepoint:04X}")

        rects = (CGRect * 1)()
        _ = self.ct.CTFontGetBoundingRectsForGlyphs(font_info["ct_font"], 0, glyphs, rects, 1)
        advances = (CGSize * 1)()
        _ = self.ct.CTFontGetAdvancesForGlyphs(font_info["ct_font"], 0, glyphs, advances, 1)

        rect = rects[0]
        min_x = int(math.floor(rect.origin.x)) - padding
        min_y = int(math.floor(rect.origin.y)) - padding
        max_x = int(math.ceil(rect.origin.x + rect.size.width)) + padding
        max_y = int(math.ceil(rect.origin.y + rect.size.height)) + padding
        width = max(1, max_x - min_x)
        height = max(1, max_y - min_y)
        pixels = (ctypes.c_uint8 * (width * height))()
        color_space = self.cg.CGColorSpaceCreateDeviceGray()
        if not color_space:
            raise RuntimeError("failed to create grayscale color space")

        context = self.cg.CGBitmapContextCreate(pixels, width, height, 8, width, color_space, 0)
        if not context:
            self.cg.CGColorSpaceRelease(color_space)
            raise RuntimeError(f"failed to create bitmap context for U+{codepoint:04X}")

        self.cg.CGContextSetGrayFillColor(context, 0.0, 1.0)
        self.cg.CGContextFillRect(context, CGRect(CGPoint(0.0, 0.0), CGSize(float(width), float(height))))
        self.cg.CGContextSetGrayFillColor(context, 1.0, 1.0)

        positions = (CGPoint * 1)(
            CGPoint(
                float(-min_x),
                float(-min_y),
            )
        )
        self.ct.CTFontDrawGlyphs(font_info["ct_font"], glyphs, positions, 1, context)

        self.cg.CGContextRelease(context)
        self.cg.CGColorSpaceRelease(color_space)

        return GlyphBitmap(
            codepoint=codepoint,
            width=width,
            height=height,
            bearing_x=min_x,
            bearing_y=max_y,
            advance_x=int(round(advances[0].width)),
            pixels=bytes(pixels),
        )


def build_charset(extra: str) -> List[int]:
    charset = list(range(ASCII_START, ASCII_END + 1))
    for ch in extra:
        codepoint = ord(ch)
        if codepoint not in charset:
            charset.append(codepoint)
    charset.sort()
    return charset


def pack_atlas(glyphs: List[GlyphBitmap]) -> tuple[int, int, bytes]:
    atlas_width = 0
    x = DEFAULT_PADDING
    y = DEFAULT_PADDING
    row_height = 0
    for glyph in glyphs:
        if x + glyph.width + DEFAULT_PADDING > ATLAS_ROW_LIMIT:
            x = DEFAULT_PADDING
            y += row_height + DEFAULT_PADDING
            row_height = 0
        glyph.atlas_x = x
        glyph.atlas_y = y
        x += glyph.width + DEFAULT_PADDING
        row_height = max(row_height, glyph.height)
        atlas_width = max(atlas_width, x)

    atlas_height = y + row_height + DEFAULT_PADDING
    atlas_width = max(1, atlas_width)
    atlas_height = max(1, atlas_height)
    atlas = bytearray(atlas_width * atlas_height)
    for glyph in glyphs:
        for row in range(glyph.height):
            src_start = row * glyph.width
            dst_start = (glyph.atlas_y + row) * atlas_width + glyph.atlas_x
            atlas[dst_start : dst_start + glyph.width] = glyph.pixels[src_start : src_start + glyph.width]
    return atlas_width, atlas_height, bytes(atlas)


def write_uof(output_path: str, pixel_size: int, ascent: int, descent: int, line_gap: int,
              fallback_index: int, glyphs: List[GlyphBitmap], atlas_width: int, atlas_height: int,
              atlas_blob: bytes) -> None:
    glyph_offset = HEADER_SIZE
    kerning_offset = glyph_offset + len(glyphs) * GLYPH_RECORD_SIZE
    atlas_offset = kerning_offset
    kerning_count = 0

    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        VERSION,
        0,
        pixel_size,
        atlas_width,
        atlas_height,
        ascent,
        descent,
        line_gap,
        len(glyphs),
        kerning_count,
        fallback_index,
        glyph_offset,
        kerning_offset,
        atlas_offset,
    )

    records = bytearray()
    for glyph in glyphs:
        records.extend(
            struct.pack(
                GLYPH_FORMAT,
                glyph.codepoint,
                glyph.atlas_x,
                glyph.atlas_y,
                glyph.width,
                glyph.height,
                glyph.bearing_x,
                glyph.bearing_y,
                glyph.advance_x,
                0,
            )
        )

    with open(output_path, "wb") as handle:
        handle.write(header)
        handle.write(records)
        handle.write(atlas_blob)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert TTF/OTF fonts into uniOS .uof atlases")
    parser.add_argument("--font", required=True, help="Input TTF/OTF font file")
    parser.add_argument("--output", required=True, help="Output .uof file")
    parser.add_argument("--size", required=True, type=int, help="Nominal pixel size")
    parser.add_argument("--fallback", default="?", help="Fallback glyph character")
    parser.add_argument("--extra-chars", default="", help="Extra characters to include beyond printable ASCII")
    parser.add_argument("--verbose", action="store_true", help="Print glyph/atlas stats")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if len(args.fallback) != 1:
        print("fallback must be a single character", file=sys.stderr)
        return 1
    if args.size <= 0:
        print("size must be positive", file=sys.stderr)
        return 1
    if not os.path.exists(args.font):
        print(f"font not found: {args.font}", file=sys.stderr)
        return 1

    rasterizer = CoreTextRasterizer()
    font_info = rasterizer.load_font(args.font, args.size)
    try:
        charset = build_charset(args.extra_chars)
        fallback_cp = ord(args.fallback)
        if fallback_cp not in charset:
            charset.append(fallback_cp)
            charset.sort()

        glyphs: List[GlyphBitmap] = []
        for codepoint in charset:
            glyphs.append(rasterizer.rasterize_glyph(font_info, codepoint, DEFAULT_PADDING))

        atlas_width, atlas_height, atlas_blob = pack_atlas(glyphs)
        fallback_index = charset.index(fallback_cp)
        ascent = int(round(font_info["ascent"]))
        descent = int(round(font_info["descent"]))
        line_gap = int(round(font_info["leading"]))
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        write_uof(
            args.output,
            args.size,
            ascent,
            descent,
            line_gap,
            fallback_index,
            glyphs,
            atlas_width,
            atlas_height,
            atlas_blob,
        )

        if args.verbose:
            print(
                f"wrote {args.output}: glyphs={len(glyphs)} size={args.size} "
                f"atlas={atlas_width}x{atlas_height} ascent={ascent} descent={descent} gap={line_gap}"
            )
    finally:
        rasterizer.close_font(font_info)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
