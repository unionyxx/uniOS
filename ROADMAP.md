# uniOS Roadmap

One line per item, with the file that proves the current state. Order within a section is suggested priority, not commitment.

## Now — recently landed

- Network status, ICMP ping, HTTP fetch and DHCP renew are exposed to userland: syscalls 293-296, shell `ifconfig`/`ping`/`fetch`/`dhcp`, live Settings Network tab (`include/uapi/syscalls.h`, `src/usr/shell/shell_cmds.cpp`).
- E2E network CI: `smoke-net` boots with slirp user networking against a host HTTP server and asserts DHCP, ARP, ICMP and a TCP download (`tools/smoke_net.py`, `meson.build`).
- Default interactive QEMU runs are networked (`meson.build` run targets).

## Next — the highest-leverage gaps

- **Userspace threading (pthreads on the kernel)**: `SYS_THREAD_CREATE` (271) creates real shared-address-space threads and futex (270) + epoll exist, but no userspace code uses them — no join/exit/TLS/mutex API, and `THREAD_DETACHED`/`thread_attr_t` in `include/uapi/syscalls_ext.h` are declared but unused. A pthread-shaped layer unlocks responsive apps, background networking and any future browser.
- **UTF-8 / international input end-to-end**: input is ASCII-only (chars 32-126 reach the GUI; `src/usr/shell/shell_editor.cpp`, `include/uapi/input.h`). Needs the input path, fonts, terminal and editor.
- **Multiuser sessions**: a complete `/etc/passwd` + `/etc/shadow` stack with SHA-256 salted hashing exists in `src/kernel/core/kuser.cpp` with zero callers; `rootfs/etc/passwd` and `rootfs/etc/shadow` are empty. Wiring it up = login flow + per-uid file semantics + the dead `cmd_login`/`cmd_useradd` shell commands.
- **Sockets as file descriptors**: sockets are separate handles today — no `dup2`, no epoll integration, no pty (`docs/reference/networking.md`, `src/usr/libc/socket.c`). This is the prerequisite for a real terminal/tty model and portable tooling.
- **Audio player app**: AC97 and HDA drivers plus WAV playback exist (`src/drivers/sound/`), but no app ships and MP3 decode is a stub (`src/drivers/sound/mp3.cpp`).

## Later

- TCP server sockets: `tcp_listen`/`tcp_accept` exist kernel-side (`include/kernel/net/tcp.h`) but are not exposed as syscalls; userspace TCP is client-only.
- TLS: no crypto in-tree; `fetch` is HTTP-only.
- DNS caching and retry: single 5-second attempt, no cache (`src/net/dns.cpp`).
- MP3 decode: stub only (`src/drivers/sound/mp3.cpp`).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the toolchain, build, and validation expectations. The `good-first-issue` label collects curated entry points (DNS retry/cache, UDP receive queue, socket-stats syscall for `netstat -s`, fetch flags, kernel log/PCI exports to userland).
