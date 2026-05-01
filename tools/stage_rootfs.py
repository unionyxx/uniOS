#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage the runtime rootfs for Meson builds.")
    parser.add_argument("--source-rootfs", required=True)
    parser.add_argument("--build-rootfs", required=True)
    parser.add_argument("--stamp", required=True)
    parser.add_argument(
        "--overlay",
        nargs=2,
        action="append",
        metavar=("RELATIVE_DEST", "SOURCE"),
        default=[],
        help="Copy SOURCE into BUILD_ROOTFS/RELATIVE_DEST.",
    )
    parser.add_argument(
        "--overlay-tree",
        nargs=2,
        action="append",
        metavar=("RELATIVE_DEST", "SOURCE_DIR"),
        default=[],
        help="Copy SOURCE_DIR into BUILD_ROOTFS/RELATIVE_DEST recursively.",
    )
    args = parser.parse_args()

    source_rootfs = Path(args.source_rootfs).resolve()
    build_rootfs = Path(args.build_rootfs).resolve()
    stamp = Path(args.stamp).resolve()

    build_rootfs.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source_rootfs, build_rootfs, dirs_exist_ok=True)

    for rel_dest, source in args.overlay:
        src = Path(source).resolve()
        dest = build_rootfs / Path(rel_dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)

    for rel_dest, source_dir in args.overlay_tree:
        src = Path(source_dir).resolve()
        dest = build_rootfs / Path(rel_dest)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(src, dest, dirs_exist_ok=True)

    stamp.parent.mkdir(parents=True, exist_ok=True)
    stamp.write_text("ok\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
