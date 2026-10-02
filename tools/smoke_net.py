#!/usr/bin/env python3
"""smoke-net: qemu_smoke wrapper with a host-side HTTP server.

Boots the uniOS image with slirp user networking and asserts the kernel net
self-test summary markers (arp/ping/http PASS). The HTTP leg fetches
hello.txt from the guest's 10.0.2.2 (slirp maps it to host loopback), so a
real python http.server must be listening on 127.0.0.1:8931 before QEMU
starts. Every QEMU invocation gets a fresh server; teardown always runs.
"""

from __future__ import annotations

import http.server
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

PORT = 8931
HELLO_BODY = b"uniOS-smoke-net\n"
HELLO_NAME = "hello.txt"


def split_args(argv: list[str]) -> tuple[str | None, list[str]]:
    """Extract --qemu-smoke PATH; pass every other argument through verbatim."""
    qemu_smoke: str | None = None
    rest: list[str] = []
    i = 0
    while i < len(argv):
        if argv[i] == "--qemu-smoke" and i + 1 < len(argv):
            qemu_smoke = argv[i + 1]
            i += 2
            continue
        rest.append(argv[i])
        i += 1
    return qemu_smoke, rest


def wait_for_server(port: int, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"http server on 127.0.0.1:{port} did not come up")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:  # noqa: A002
        pass


def main() -> int:
    qemu_smoke, rest = split_args(sys.argv[1:])
    if not qemu_smoke:
        print("smoke-net: --qemu-smoke <path> is required", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="unios-smoke-net-") as served:
        with open(os.path.join(served, HELLO_NAME), "wb") as f:
            f.write(HELLO_BODY)

        os.chdir(served)
        handler = lambda *req, **kw: QuietHandler(*req, directory=served, **kw)  # noqa: E731
        server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            wait_for_server(PORT)
            cmd = [sys.executable, qemu_smoke] + rest
            return subprocess.run(cmd).returncode
        finally:
            server.shutdown()
            server.server_close()

    return 2


if __name__ == "__main__":
    sys.exit(main())
