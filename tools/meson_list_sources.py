#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="List source files for Meson.")
    parser.add_argument("root", help="Root directory to scan, relative to the repo root.")
    parser.add_argument(
        "--glob",
        action="append",
        dest="globs",
        default=[],
        help="Glob pattern to include. Can be passed multiple times.",
    )
    parser.add_argument(
        "--exclude-prefix",
        action="append",
        dest="exclude_prefixes",
        default=[],
        help="Relative path prefix to exclude. Can be passed multiple times.",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    root = (repo_root / args.root).resolve()
    globs = args.globs or ["*"]
    exclude_prefixes = [prefix.replace("\\", "/").rstrip("/") for prefix in args.exclude_prefixes]

    matches: set[str] = set()
    for pattern in globs:
        for path in root.rglob(pattern):
            if not path.is_file():
                continue
            rel = path.relative_to(repo_root).as_posix()
            if any(rel == prefix or rel.startswith(prefix + "/") for prefix in exclude_prefixes):
                continue
            matches.add(rel)

    for rel in sorted(matches):
        print(rel)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
