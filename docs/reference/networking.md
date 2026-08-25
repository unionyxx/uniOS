# Networking

The network stack (`src/net/`) is a freestanding, fully polled IPv4 stack in the kernel. Userspace reaches it through nine dedicated syscalls — sockets are not file descriptors.

## NIC Selection and Polling

`net_init()` (deferred boot task) tries e1000 first, then RTL8139; the first success wins and exactly one NIC is active. Both drivers are interrupt-free: masks are cleared and `net_poll()` drains receive state. `net_poll()` polls the driver, feeds up to 32 packets into the ethernet layer, then runs TCP timers. It is called from the kernel idle loop every 10th iteration and re-entered from ARP resolution, TCP operations, DHCP, and DNS waits.

There is no mbuf abstraction: each layer owns static 1600-byte staging buffers under spinlocks. TCP transmit segments are malloc'd per packet.

| Layer | Files |
| --- | --- |
| NIC dispatch/config | `net.cpp` |
| Ethernet, ARP | `ethernet.cpp`, `arp.cpp` |
| IPv4, ICMP | `ipv4.cpp`, `icmp.cpp` |
| UDP, TCP | `udp.cpp`, `tcp.cpp` (+ `tcp_congestion.h`) |
| DHCP, DNS | `dhcp.cpp`, `dns.cpp` |

## Ethernet and ARP

- Ethernet accepts unicast-to-us and broadcast only; payloads below 46 bytes are padded; sending rejects zero destinations and payloads over 1500.
- ARP table: 32 entries, no aging (entries never expire); a full table overwrites slot 0.
- `arp_resolve()` serves cache hits immediately, broadcasts for misses, and busy-waits on `net_poll()` up to 5 seconds. Only one resolution can be outstanding; broadcasts resolve to the broadcast MAC.
- Incoming ARP teaches the table on every packet (gratuitous learning) and replies to requests targeting our IP.

## IPv4 and ICMP

- One global configuration (IP, netmask, gateway, DNS). No routing table: the next hop is the destination when on-link, else the gateway.
- No fragmentation and no reassembly. Payloads are capped at 1480 bytes (MTU 1500 minus the fixed 20-byte header). TTL 64, incrementing ID.
- Receive validates version/IHL/checksum/destination before dispatching to ICMP, UDP, or TCP.
- ICMP implements echo request/reply only. No ICMP errors are generated or consumed. The shell `ping` command resolves the host instead — echo is not exposed to userland.

## UDP

16 sockets, each with a single 1500-byte receive slot (overwrite semantics, no queue). Duplicate binds are rejected; unbound sends use ephemeral port 49152. Checksums use a pseudo-header; computed zero transmits as `0xFFFF`. Datagrams addressed to port 68 with no bound socket feed DHCP.

## DHCP

`dhcp_request()` runs once at boot when the link is up: DISCOVER, wait up to 5 s for OFFER, REQUEST (option 50 + server id), wait up to 5 s for ACK, then configure IP/netmask/gateway/DNS.

- Parsed options: subnet mask (1), router (3), DNS (6), server identifier (54).
- No lease management: option 51 is never parsed, there are no renew/rebind timers, and no RELEASE/DECLINE. Configuration persists until reboot.
- One attempt, no retries; NAK is not handled. Packets are hand-built Ethernet broadcasts (source 0.0.0.0) since no address exists yet.

## DNS

`dns_resolve(hostname)` returns the first A record:

- Literal dotted quads are parsed directly.
- Server: DHCP-provided DNS, falling back to `8.8.8.8`.
- Single question, RD flag only — the resolver relies entirely on a recursive server. No iteration, no referrals.
- Transport: one UDP socket on an ephemeral port, 5-second timeout, no retry, no caching.
- Parsing validates ID/QR/RCODE, skips questions and answers with bounds checks, handles compression pointers, and returns the first A record with rdlength 4. The parser is pure and ktest-covered against hostile inputs.

## Socket API

Syscalls 220-235 (see [System calls](syscalls.md)): `SOCKET`, `BIND`, `SENDTO/RECVFROM` (UDP), `CONNECT/SEND/RECV` (TCP), `CLOSESOCKET`, `RESOLVE`. Handles encode `kind << 12 | index`; small raw handles decode as UDP for backward compatibility. libc (`src/usr/libc/socket.c`) provides the POSIX-shaped wrappers with `sockaddr_in`.

Consequences of sockets not being fds:

- `read/write/close/dup2` do not apply; sockets cannot be epoll-monitored.
- `connect` blocks up to 5 s; TCP `send` blocks while the send ring is full; TCP `recv` and UDP `recvfrom` are nonblocking (return 0 when empty).
- No listen/accept syscalls: TCP is client-only from userspace (see [TCP](tcp.md)).

## Configuration

DHCP is the only configuration source; there is no static IP path. `ethernet_enabled` / `ethernet_use_dhcp` in `SYSTEM.CFG` are control-center toggles only — nothing in the kernel or stack reads them, and `net_init()` always runs and always attempts DHCP.

QEMU networking: `run-qemu-net` and `run-qemu-full` attach `-netdev user` with an e1000 device; default run targets have no NIC.
