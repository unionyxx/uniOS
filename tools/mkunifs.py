#!/usr/bin/env python3
"""Pack a directory tree into a UNIFS v1 image consumed by src/fs/unifs/unifs.cpp."""

from __future__ import annotations

import argparse
import os
import struct
import sys

MAGIC = b"UNIFS v1"
HEADER_FORMAT = "<8sQ"
ENTRY_FORMAT = "<64sQQ"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
ENTRY_SIZE = struct.calcsize(ENTRY_FORMAT)
MAX_NAME_BYTES = 63  # 64-byte name field, NUL-terminated
MAX_ENTRIES = 256  # the kernel refuses to mount images with more entries (MAX_FILES * 4)
COPY_BUFFER_SIZE = 1024 * 1024


def collect_entries(source_dir: str) -> list[tuple[str, bool, str | None]]:
    entries: list[tuple[str, bool, str | None]] = []
    for root, dirs, filenames in os.walk(source_dir):
        for dirname in dirs:
            dirpath = os.path.join(root, dirname)
            rel_path = os.path.relpath(dirpath, source_dir).replace(os.sep, "/")
            entries.append((rel_path + "/", True, None))
        for filename in filenames:
            filepath = os.path.join(root, filename)
            rel_path = os.path.relpath(filepath, source_dir).replace(os.sep, "/")
            entries.append((rel_path, False, filepath))
    # Sort so the image layout is identical regardless of platform enumeration order.
    entries.sort(key=lambda item: item[0])
    return entries


def encode_name(name: str) -> bytes:
    name_bytes = name.encode("utf-8")
    if len(name_bytes) > MAX_NAME_BYTES:
        raise SystemExit(f"mkunifs: entry name too long ({len(name_bytes)} > {MAX_NAME_BYTES} bytes): {name}")
    return name_bytes


def create_unifs(source_dir: str, output_file: str) -> None:
    if not os.path.isdir(source_dir):
        raise SystemExit(f"mkunifs: source directory not found: {source_dir}")

    entries = collect_entries(source_dir)
    if not entries:
        raise SystemExit(f"mkunifs: source directory is empty: {source_dir}")
    if len(entries) > MAX_ENTRIES:
        raise SystemExit(f"mkunifs: too many entries ({len(entries)} > {MAX_ENTRIES}); the kernel would refuse to mount")

    names = [encode_name(name) for name, _, _ in entries]
    sizes: list[int] = []
    for name, is_dir, filepath in entries:
        if is_dir:
            sizes.append(0)
            continue
        try:
            sizes.append(os.path.getsize(filepath))
        except OSError as exc:
            raise SystemExit(f"mkunifs: cannot stat {filepath}: {exc}")

    data_offset = HEADER_SIZE + len(entries) * ENTRY_SIZE
    offsets: list[int] = []
    cursor = data_offset
    for (_, is_dir, _), size in zip(entries, sizes):
        offsets.append(cursor)
        if not is_dir:
            cursor += size

    out_dir = os.path.dirname(os.path.abspath(output_file))
    os.makedirs(out_dir, exist_ok=True)
    with open(output_file, "wb") as out:
        out.write(struct.pack(HEADER_FORMAT, MAGIC, len(entries)))
        for name_bytes, offset, size in zip(names, offsets, sizes):
            out.write(struct.pack(ENTRY_FORMAT, name_bytes, offset, size))

        for (name, is_dir, filepath), size in zip(entries, sizes):
            if is_dir:
                continue
            try:
                written = 0
                with open(filepath, "rb") as src:
                    while True:
                        chunk = src.read(COPY_BUFFER_SIZE)
                        if not chunk:
                            break
                        out.write(chunk)
                        written += len(chunk)
            except OSError as exc:
                raise SystemExit(f"mkunifs: failed to read {filepath}: {exc}")
            if written != size:
                raise SystemExit(f"mkunifs: {filepath} changed size while packing ({size} -> {written})")

    total_bytes = cursor
    print(f"Created {output_file} with {len(entries)} files.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Pack a directory tree into a UNIFS v1 image.")
    parser.add_argument("source_dir", help="Directory whose contents become the image.")
    parser.add_argument("output_file", help="Output unifs.img path.")
    args = parser.parse_args()
    create_unifs(args.source_dir, args.output_file)
    return 0


if __name__ == "__main__":
    sys.exit(main())
