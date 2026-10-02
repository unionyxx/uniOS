#!/usr/bin/env python3
# UOF v2 font converter: rasterizes TTF/OTF sources into preprocessed glyph
# atlases for the uniOS GUI runtime. Cross-platform (Linux/macOS/Windows) via
# freetype-py (wheels bundle FreeType) and fontTools (GPOS kerning, metrics).
#
# Quality model, in order:
#   - 4x4 supersampled rasterization (box-filtered coverage, no hinting)
#   - optional 4x horizontal oversampling for runtime subpixel positioning
#   - advances/kerning in 26.6 fixed point, GPOS kerning as a compact
#     class matrix plus an additive exception pair list
#   - RLE-compressed 8-bit coverage atlas
from __future__ import annotations

import argparse
import os
import struct
import sys
from collections import defaultdict

try:
    import freetype as ft
except ImportError:
    print("uof_convert.py requires freetype-py: pip install freetype-py", file=sys.stderr)
    raise

try:
    from fontTools.ttLib import TTFont
except ImportError:
    print("uof_convert.py requires fontTools: pip install fonttools", file=sys.stderr)
    raise


MAGIC = b"UOFN"
VERSION = 2
FLAG_H_OVERSAMPLE = 0x0001
SS = 4  # rasterization supersample factor (both axes at bake time)
OVERSAMPLE = 4  # horizontal subcolumn count per output pixel (UI preset)
OVERSAMPLE_MONO = 1  # mono fonts render on an integer grid; no subcolumns

HEADER_FORMAT = "<4sHHHHHHhhhIIHHIIIII"
GLYPH_FORMAT = "<IHHHHhhhBB"
KERN_FORMAT = "<IIh"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
GLYPH_SIZE = struct.calcsize(GLYPH_FORMAT)
KERN_SIZE = struct.calcsize(KERN_FORMAT)
KERN_MIN_26 = 4  # keep exception pairs |value| >= 1/16 px
ATLAS_ROW_LIMIT_PX = 2048
SLOT_GAP_PX = 2
V16_MAX = 32767
V16_MIN = -32768

# Charset presets. Codepoints are skipped when the source font lacks them.
UI_BLOCKS = [
    (0x0020, 0x007E),  # printable ASCII
    (0x00A0, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
    (0x0370, 0x03FF),  # Greek and Coptic
    (0x0400, 0x045F),  # Cyrillic
    (0x2013, 0x2026),  # dashes, quotes, bullet, ellipsis
    (0x20AC, 0x20AC),  # euro
    (0x2212, 0x2212),  # minus sign
]
MONO_BLOCKS = [
    (0x0020, 0x007E),  # printable ASCII
    (0x00A1, 0x00A1),  # inverted exclam
    (0x00A3, 0x00A4),  # pound, currency
    (0x00A7, 0x00B1),  # section .. plus-minus
    (0x00B5, 0x00B7),  # micro, middle dot
    (0x00BB, 0x00BB),  # guillemet close
    (0x00BF, 0x00BF),  # inverted question
    (0x00D7, 0x00D7),  # multiply
    (0x00F7, 0x00F7),  # divide
    (0x2500, 0x259F),  # box drawing + block elements
]


FALLBACK_CHAR = "?"
SIZES = list(range(8, 19))
FAMILIES = [
    # (output prefix, source font, charset preset)
    ("inter-ui", "inter_regular.otf", "ui"),
    ("inter-title", "inter_semibold.otf", "ui"),
    ("geist-mono", "geist_mono_regular.ttf", "mono"),
]
FONT_SRC_DIR = "assets/fonts_src"
DEFAULT_OUTPUT_ROOT = "rootfs/usr/share/fonts"


class RasterGlyph:
    def __init__(self, codepoint, subcols, width, height, bearing_x26, bearing_y, advance26):
        self.codepoint = codepoint
        self.subcols = subcols  # flat list, len == width * height * oversample
        self.width = width  # output px (includes horizontal pad for oversample)
        self.height = height  # output rows
        self.bearing_x26 = bearing_x26
        self.bearing_y = bearing_y
        self.advance26 = advance26
        self.atlas_x = 0
        self.atlas_y = 0
        self.kern_left = 0
        self.kern_right = 0


def blocks_to_codepoints(blocks):
    cps = set()
    for lo, hi in blocks:
        cps.update(range(lo, hi + 1))
    return sorted(cps)


class FontSource:
    """fontTools view of the font: exact metrics, cmap, GPOS kerning."""

    def __init__(self, path):
        self.tt = TTFont(path)
        self.upem = self.tt["head"].unitsPerEm
        hhea = self.tt["hhea"]
        self.ascender = hhea.ascent
        self.descender = hhea.descent  # negative units
        self.line_gap = hhea.lineGap
        self.cmap = self.tt.getBestCmap()  # cp -> glyph name
        self.name_to_cps = defaultdict(set)
        for cp, name in self.cmap.items():
            self.name_to_cps[name].add(cp)
        self.hmtx = self.tt["hmtx"]

    def advance_units(self, name):
        return self.hmtx[name][0]

    def units_to_26(self, units, pixel_size):
        v = (units * pixel_size * 64 + self.upem // 2) // self.upem
        return max(V16_MIN, min(V16_MAX, v))

    def kerning(self):
        """Extract GPOS pair kerning as (matrix, exceptions).

        Returns (class1_defs, class2_defs, matrix_units, exception_units) where
        class defs map glyph name -> class id (1..N, 0 = unclassed) for the
        dominant PairPos2 subtable, matrix_units is the row-major
        (c1+1)x(c2+1) XAdvance matrix in font units, and exception_units maps
        (left_name, right_name) -> summed XAdvance of every non-dominant pair
        subtable (additive with the matrix, matching GPOS accumulation).
        Context-gated pair lookups are folded in unconditionally; values are
        small and unconditional application matches the visual result closely.
        """
        if "GPOS" not in self.tt:
            return {}, {}, [[0]], {}
        pairs = []  # every PairPos subtable reachable from the GPOS lookup tree
        seen_lookups = set()

        def visit_subtables(lookup):
            if id(lookup) in seen_lookups:
                return
            seen_lookups.add(id(lookup))
            kind = lookup.LookupType
            for st in lookup.SubTable:
                if kind == 9:  # ExtensionPos
                    inner = st.ExtensionLookupType
                    if inner == 2:
                        pairs.append(st.ExtSubTable)
                    elif inner == 6:
                        chain(st.ExtSubTable)
                elif kind == 2:
                    pairs.append(st)
                elif kind == 6:
                    chain(st)

        def chain(st):
            records = []
            if st.Format == 1:
                for ruleset in (getattr(st, "ChainPosRuleSet", None) or []):
                    if ruleset:
                        records.extend(ruleset.ChainPosRule.LookupListRecord)
            elif st.Format == 2:
                for ruleset in (getattr(st, "ChainPosClassSet", None) or []):
                    if not ruleset:
                        continue
                    records.extend(ruleset.ChainPosClassRule.LookupListRecord)
            elif st.Format == 3:
                records.extend(st.LookupListRecord)
            for rec in records:
                visit_subtables(rec.Lookup)

        for lookup in self.tt["GPOS"].table.LookupList.Lookup:
            visit_subtables(lookup)

        def nonzero_cells(st):
            return sum(1 for row in st.Class1Record for rec in row.Class2Record if rec.Value1 and rec.Value1.XAdvance)

        pair_pos2 = [st for st in pairs if st.Format == 2]
        dominant = max(pair_pos2, key=lambda st: (nonzero_cells(st), len(st.Coverage.glyphs))) if pair_pos2 else None

        exceptions = defaultdict(int)  # (left_name, right_name) -> units

        def add_exception(left, right, units):
            if units:
                exceptions[(left, right)] += units

        def collect_pairpos1(st):
            for left, pset in zip(st.Coverage.glyphs, st.PairSet):
                for rec in pset.PairValueRecord:
                    if rec.Value1:
                        add_exception(left, rec.SecondGlyph, rec.Value1.XAdvance)

        def collect_pairpos2(st):
            coverage = set(st.Coverage.glyphs)
            class1 = defaultdict(list)
            class2 = defaultdict(list)
            for glyph, cls in st.ClassDef1.classDefs.items():
                class1[cls].append(glyph)
            for glyph, cls in st.ClassDef2.classDefs.items():
                class2[cls].append(glyph)
            for c1, row in enumerate(st.Class1Record):
                for c2, rec in enumerate(row.Class2Record):
                    value = rec.Value1.XAdvance if rec.Value1 else 0
                    if not value:
                        continue
                    for left in class1.get(c1, []):
                        if left not in coverage:
                            continue
                        for right in class2.get(c2, []):
                            add_exception(left, right, value)

        for st in pairs:
            if st is dominant:
                continue
            if st.Format == 1:
                collect_pairpos1(st)
            else:
                collect_pairpos2(st)

        if dominant is None:
            return {}, {}, [[0]], exceptions

        class1_defs = dict(dominant.ClassDef1.classDefs)  # name -> class id (left)
        class2_defs = dict(dominant.ClassDef2.classDefs)  # name -> class id (right)
        n1 = max(class1_defs.values()) if class1_defs else 0
        n2 = max(class2_defs.values()) if class2_defs else 0
        if n1 > 255 or n2 > 255:
            raise RuntimeError(f"kern class ids exceed u8: {n1}x{n2}")
        matrix = [[0] * (n2 + 1) for _ in range(n1 + 1)]
        for c1, row in enumerate(dominant.Class1Record):
            if c1 > n1:
                continue
            for c2, rec in enumerate(row.Class2Record):
                if c2 > n2:
                    continue
                if rec.Value1 and rec.Value1.XAdvance:
                    matrix[c1][c2] = rec.Value1.XAdvance
        return class1_defs, class2_defs, matrix, exceptions


def rle_encode(data):
    """(run_length-1, value) byte pairs; runs capped at 256."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        value = data[i]
        run = 1
        while run < 256 and i + run < n and data[i + run] == value:
            run += 1
        out.append(run - 1)
        out.append(value)
        i += run
    return bytes(out)


def rle_decode(stream, expected_size):
    if len(stream) % 2 != 0:
        raise ValueError("rle atlas stream has odd length")
    out = bytearray()
    for i in range(0, len(stream), 2):
        run = stream[i] + 1
        value = stream[i + 1]
        if len(out) + run > expected_size:
            raise ValueError("rle atlas overflows expected size")
        out.extend(bytes([value]) * run)
    if len(out) != expected_size:
        raise ValueError(f"rle atlas decodes to {len(out)} bytes, expected {expected_size}")
    return bytes(out)


class Rasterizer:
    def __init__(self, font_path, pixel_size, oversample):
        self.face = ft.Face(font_path)
        self.oversample = oversample
        self.pixel_size = pixel_size
        self.face.set_pixel_sizes(0, pixel_size * SS)

    def glyph(self, codepoint, source):
        index = self.face.get_char_index(codepoint)
        if index == 0:
            return None
        name = source.cmap.get(codepoint)
        if name is None:
            return None
        self.face.load_glyph(index, ft.FT_LOAD_NO_HINTING | ft.FT_LOAD_RENDER)
        bitmap = self.face.glyph.bitmap
        bm_w, bm_h = bitmap.width, bitmap.rows
        left4 = self.face.glyph.bitmap_left
        top4 = self.face.glyph.bitmap_top
        if bm_w == 0 or bm_h == 0:  # space and other blank glyphs
            pitch = 0
            buf = b""
        else:
            pitch = bitmap.pitch
            if pitch <= 0:
                raise RuntimeError(f"negative bitmap pitch for U+{codepoint:04X}")
            buf = bytes(bitmap.buffer)

        if self.oversample == 1:
            width = max(1, (bm_w + SS - 1) // SS)
            height = max(1, (bm_h + SS - 1) // SS)
            subcols = bytearray(width * height)
            for row in range(height):
                for col in range(width):
                    acc = 0
                    for dy in range(SS):
                        sy = row * SS + dy
                        if sy >= bm_h:
                            continue
                        base = sy * pitch
                        for dx in range(SS):
                            sx = col * SS + dx
                            if sx < bm_w:
                                acc += buf[base + sx]
                    subcols[row * width + col] = (acc + SS * SS // 2) // (SS * SS)
            bearing_x26 = left4 * 16  # 4x px -> 26.6 at true size
        else:
            # 1 output px of zero pad on each side for the FIR headroom.
            width = (bm_w + SS - 1) // SS + 2
            height = max(1, (bm_h + SS - 1) // SS)
            subcols = bytearray(width * height * self.oversample)
            stride = width * self.oversample
            pad = self.oversample  # ink starts after 1 output px of pad
            for row in range(height):
                dst_row = row * stride + pad
                for col in range(bm_w):
                    acc = 0
                    for dy in range(SS):
                        sy = row * SS + dy
                        if sy < bm_h:
                            acc += buf[sy * pitch + col]
                    # Vertical box average; horizontal resolution preserved.
                    subcols[dst_row + col] = (acc + SS // 2) // SS
            bearing_x26 = left4 * 16 - 64  # shifted left by the pad

        bearing_y = top4 // SS  # floor: ink top row in output px
        advance26 = source.units_to_26(source.advance_units(name), self.pixel_size)
        return RasterGlyph(codepoint, bytes(subcols), width, height, bearing_x26, bearing_y, advance26)


def pack_atlas(glyphs, oversample):
    """Height-descending row packing. Returns (atlas_width_px, atlas_height, atlas_bytes)."""
    stride_scale = oversample
    limit = ATLAS_ROW_LIMIT_PX * stride_scale
    ordered = sorted(glyphs, key=lambda g: (-g.height, -g.width, g.codepoint))
    x = SLOT_GAP_PX * stride_scale
    y = SLOT_GAP_PX
    row_height = 0
    atlas_width_sub = 0
    for glyph in ordered:
        glyph_sub_w = glyph.width * stride_scale
        if x + glyph_sub_w + SLOT_GAP_PX * stride_scale > limit:
            x = SLOT_GAP_PX * stride_scale
            y += row_height + SLOT_GAP_PX
            row_height = 0
        glyph.atlas_x = x
        glyph.atlas_y = y
        x += glyph_sub_w + SLOT_GAP_PX * stride_scale
        row_height = max(row_height, glyph.height)
        atlas_width_sub = max(atlas_width_sub, x)
    atlas_height = y + row_height + SLOT_GAP_PX
    atlas_width_px = (atlas_width_sub + stride_scale - 1) // stride_scale
    atlas = bytearray(atlas_width_px * stride_scale * atlas_height)
    row_stride = atlas_width_px * stride_scale
    for glyph in glyphs:
        glyph_stride = glyph.width * stride_scale
        for row in range(glyph.height):
            src = glyph.subcols[row * glyph_stride : (row + 1) * glyph_stride]
            dst = (glyph.atlas_y + row) * row_stride + glyph.atlas_x
            atlas[dst : dst + glyph_stride] = src
    return atlas_width_px, atlas_height, bytes(atlas)


def clamp26(value):
    return max(V16_MIN, min(V16_MAX, value))


def build_kerning(source, glyphs_by_cp, pixel_size):
    """Returns (records, matrix_flat) ready for serialization."""
    class1_defs, class2_defs, matrix, exceptions = source.kerning()

    for codepoint, glyph in glyphs_by_cp.items():
        name = source.cmap.get(codepoint)
        if name is None:
            continue
        glyph.kern_left = class1_defs.get(name, 0)
        glyph.kern_right = class2_defs.get(name, 0)

    n1 = len(matrix) - 1
    n2 = len(matrix[0]) - 1 if matrix else 0
    matrix_flat = []
    for c1 in range(n1 + 1):
        for c2 in range(n2 + 1):
            matrix_flat.append(clamp26(source.units_to_26(matrix[c1][c2], pixel_size)))

    # Each codepoint maps to exactly one glyph name in the cmap, so (cp, cp)
    # keys are unique once expanded; sorting makes the table binary-searchable.
    records = []
    for (left_name, right_name), units in sorted(exceptions.items()):
        left_cps = source.name_to_cps.get(left_name, ())
        right_cps = source.name_to_cps.get(right_name, ())
        value26 = clamp26(source.units_to_26(units, pixel_size))
        if abs(value26) < KERN_MIN_26:
            continue
        for left_cp in sorted(left_cps):
            if left_cp not in glyphs_by_cp:
                continue
            for right_cp in sorted(right_cps):
                if right_cp not in glyphs_by_cp:
                    continue
                records.append((left_cp, right_cp, value26))
    records.sort(key=lambda r: (r[0], r[1]))
    return records, matrix_flat, n1, n2


def write_uof(path, header_fields, glyph_records, kern_records, matrix_flat, atlas_rle):
    (magic, version, flags, pixel_size, oversample, atlas_width, atlas_height, ascent, descent, line_gap,
     fallback_index, matrix_c1, matrix_c2) = header_fields
    glyph_count = len(glyph_records)
    kerning_count = len(kern_records)

    glyph_offset = HEADER_SIZE
    kerning_offset = glyph_offset + glyph_count * GLYPH_SIZE
    matrix_offset = kerning_offset + kerning_count * KERN_SIZE
    atlas_offset = matrix_offset + len(matrix_flat) * 2

    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        version,
        flags,
        pixel_size,
        oversample,
        atlas_width,
        atlas_height,
        ascent,
        descent,
        line_gap,
        glyph_count,
        kerning_count,
        matrix_c1,
        matrix_c2,
        fallback_index,
        glyph_offset,
        kerning_offset,
        matrix_offset,
        atlas_offset,
    )

    with open(path, "wb") as handle:
        handle.write(header)
        for rec in glyph_records:
            handle.write(struct.pack(GLYPH_FORMAT, *rec))
        for rec in kern_records:
            handle.write(struct.pack(KERN_FORMAT, *rec))
        for value in matrix_flat:
            handle.write(struct.pack("<h", value))
        handle.write(atlas_rle)


def validate_output(path, expected_glyphs, expected_atlas_size):
    data = open(path, "rb").read()
    (magic, version, flags, pixel_size, oversample, atlas_width, atlas_height, ascent, descent, line_gap,
     glyph_count, kerning_count, matrix_c1, matrix_c2, fallback_index, glyph_offset, kerning_offset,
     matrix_offset, atlas_offset) = struct.unpack_from(HEADER_FORMAT, data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError("bad magic/version")
    if glyph_offset != HEADER_SIZE:
        raise ValueError("glyph offset mismatch")
    if kerning_offset != glyph_offset + glyph_count * GLYPH_SIZE:
        raise ValueError("kerning offset mismatch")
    if matrix_offset != kerning_offset + kerning_count * KERN_SIZE:
        raise ValueError("matrix offset mismatch")
    if atlas_offset != matrix_offset + (matrix_c1 + 1) * (matrix_c2 + 1) * 2:
        raise ValueError("atlas offset mismatch")
    if glyph_count != expected_glyphs:
        raise ValueError("glyph count mismatch")
    if fallback_index >= glyph_count:
        raise ValueError("fallback index out of range")

    stride = atlas_width * oversample
    if stride * atlas_height != expected_atlas_size:
        raise ValueError("atlas size mismatch")

    prev_key = None
    prev_cp = 0
    for i in range(glyph_count):
        codepoint, ax, ay, w, h, bx, by, adv, kl, kr = struct.unpack_from(GLYPH_FORMAT, data, glyph_offset + i * GLYPH_SIZE)
        if ax + w * oversample > stride:
            raise ValueError(f"glyph U+{codepoint:04X} escapes atlas width")
        if ay + h > atlas_height:
            raise ValueError(f"glyph U+{codepoint:04X} escapes atlas height")
        if kl > matrix_c1 or kr > matrix_c2:
            raise ValueError(f"glyph U+{codepoint:04X} class exceeds matrix dims")
        if i > 0 and codepoint <= prev_cp:
            raise ValueError("glyph table not sorted by codepoint")
        prev_cp = codepoint

    for i in range(kerning_count):
        left, right, _ = struct.unpack_from(KERN_FORMAT, data, kerning_offset + i * KERN_SIZE)
        key = (left, right)
        if prev_key is not None and key <= prev_key:
            raise ValueError("kerning table not sorted/unique")
        prev_key = key

    rle_decode(data[atlas_offset:], expected_atlas_size)
    return glyph_count, kerning_count, matrix_c1, matrix_c2


def main():
    parser = argparse.ArgumentParser(description="Convert TTF/OTF fonts into uniOS .uof v2 atlases")
    parser.add_argument("--font", help="Input TTF/OTF font file")
    parser.add_argument("--output", help="Output .uof file")
    parser.add_argument("--size", type=int, help="Nominal pixel size")
    parser.add_argument("--charset", default="ui", choices=["ui", "mono"], help="Character preset")
    parser.add_argument("--fallback", default=FALLBACK_CHAR, help="Fallback glyph character")
    parser.add_argument("--no-oversample", action="store_true", help="Disable horizontal oversampling (integer grid)")
    parser.add_argument("--all", action="store_true", help=f"Regenerate all families x sizes {SIZES[0]}-{SIZES[-1]}")
    parser.add_argument("--output-root", default=DEFAULT_OUTPUT_ROOT, help=f"Output dir for --all (default {DEFAULT_OUTPUT_ROOT})")
    parser.add_argument("--verbose", action="store_true", help="Print glyph/atlas stats")
    args = parser.parse_args()

    if args.all:
        return build_all(args)

    if not args.font or not args.output or not args.size:
        parser.error("--font, --output and --size are required (or use --all)")
    return convert_one(args.font, args.output, args.size, args.charset, args.fallback, args.no_oversample, args.verbose)


def convert_one(font_path, output_path, pixel_size, charset, fallback, no_oversample, verbose):
    if pixel_size <= 0:
        print("size must be positive", file=sys.stderr)
        return 1
    if not os.path.exists(font_path):
        print(f"font not found: {font_path}", file=sys.stderr)
        return 1
    if len(fallback) != 1:
        print("fallback must be a single character", file=sys.stderr)
        return 1

    blocks = UI_BLOCKS if charset == "ui" else MONO_BLOCKS
    oversample = OVERSAMPLE_MONO if (charset == "mono" or no_oversample) else OVERSAMPLE

    source = FontSource(font_path)
    rasterizer = Rasterizer(font_path, pixel_size, oversample)

    charset_cps = [cp for cp in blocks_to_codepoints(blocks) if cp in source.cmap]
    fallback_cp = ord(fallback)
    if fallback_cp not in charset_cps:
        charset_cps.append(fallback_cp)
        charset_cps.sort()

    glyphs = []
    for cp in charset_cps:
        glyph = rasterizer.glyph(cp, source)
        if glyph is not None:
            glyphs.append(glyph)
    if not glyphs:
        print("no glyphs rasterized", file=sys.stderr)
        return 1
    glyphs.sort(key=lambda g: g.codepoint)
    if fallback_cp not in {g.codepoint for g in glyphs}:
        print("fallback glyph failed to rasterize", file=sys.stderr)
        return 1

    atlas_width, atlas_height, atlas = pack_atlas(glyphs, oversample)
    atlas_rle = rle_encode(atlas)

    glyphs_by_cp = {g.codepoint: g for g in glyphs}
    kern_records, matrix_flat, n1, n2 = build_kerning(source, glyphs_by_cp, pixel_size)

    ascent = (source.units_to_26(source.ascender, pixel_size) + 32) >> 6
    descent = (source.units_to_26(-source.descender, pixel_size) + 32) >> 6
    line_gap = (source.units_to_26(source.line_gap, pixel_size) + 32) >> 6

    glyph_records = [
        (g.codepoint, g.atlas_x, g.atlas_y, g.width, g.height, g.bearing_x26, g.bearing_y, g.advance26, g.kern_left,
         g.kern_right)
        for g in glyphs
    ]
    fallback_index = [g.codepoint for g in glyphs].index(fallback_cp)
    flags = FLAG_H_OVERSAMPLE if oversample > 1 else 0

    header_fields = (MAGIC, VERSION, flags, pixel_size, oversample, atlas_width, atlas_height, ascent, descent,
                     line_gap, fallback_index, n1, n2)
    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    write_uof(output_path, header_fields, glyph_records, kern_records, matrix_flat, atlas_rle)

    count_g, count_k, mc1, mc2 = validate_output(output_path, len(glyphs), len(atlas))
    if verbose:
        matrix_bytes = len(matrix_flat) * 2
        print(
            f"wrote {output_path}: glyphs={count_g} size={pixel_size} oversample={oversample} "
            f"atlas={atlas_width}x{atlas_height} raw={len(atlas)} rle={len(atlas_rle)} "
            f"kern_pairs={count_k} matrix={mc1}x{mc2} ({matrix_bytes}B) "
            f"asc={ascent} desc={descent} gap={line_gap}"
        )
    return 0


def build_all(args):
    root = os.path.abspath(args.output_root)
    for prefix, font_name, charset in FAMILIES:
        font_path = os.path.join(FONT_SRC_DIR, font_name)
        for size in SIZES:
            output = os.path.join(root, f"{prefix}-{size}.uof")
            status = convert_one(font_path, output, size, charset, args.fallback, args.no_oversample, True)
            if status != 0:
                return status
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
