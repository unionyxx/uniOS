#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run QEMU until expected serial smoke-test markers appear.")
    parser.add_argument("--timeout", type=float, default=30.0, help="Maximum runtime in seconds.")
    parser.add_argument("--success", action="append", default=[], help="Required output substring.")
    parser.add_argument("--fail", action="append", default=[], help="Fail immediately if this substring appears.")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="QEMU command after --.")
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("missing QEMU command")
    return args


def terminate_process(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3.0)


def main() -> int:
    args = parse_args()
    output = bytearray()
    lock = threading.Lock()

    proc = subprocess.Popen(
        args.command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    def reader() -> None:
        assert proc.stdout is not None
        while True:
            chunk = proc.stdout.readline()
            if not chunk:
                break
            with lock:
                output.extend(chunk)
            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            sys.stdout.flush()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    deadline = time.monotonic() + args.timeout
    success_seen = False
    failure_seen = None

    while time.monotonic() < deadline:
        with lock:
            text = output.decode("utf-8", errors="replace")

        failure_seen = next((pattern for pattern in args.fail if pattern in text), None)
        if failure_seen:
            break

        if all(pattern in text for pattern in args.success):
            success_seen = True
            break

        if proc.poll() is not None:
            break

        time.sleep(0.1)

    terminate_process(proc)
    thread.join(timeout=2.0)

    with lock:
        text = output.decode("utf-8", errors="replace")

    failure_seen = failure_seen or next((pattern for pattern in args.fail if pattern in text), None)
    if failure_seen:
        print(f"\nqemu-smoke: failed after seeing marker: {failure_seen}", file=sys.stderr)
        return 1

    missing = [pattern for pattern in args.success if pattern not in text]
    if missing:
        print(f"\nqemu-smoke: missing success marker(s): {', '.join(missing)}", file=sys.stderr)
        return 1

    print("\nqemu-smoke: success markers observed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
