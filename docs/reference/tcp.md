# TCP

TCP (`src/net/tcp.cpp`) is a client-oriented implementation with a full connection state machine, windowed send, and RFC 5681-style congestion control. The congestion-control policy lives socket-free in `include/kernel/net/tcp_congestion.h` so it is unit-testable.

## Constants

| Constant | Value |
| --- | --- |
| Sockets | 32 |
| MSS | 1460 |
| Advertised window | 65535 (fixed) |
| Receive ring | 64 KiB per socket |
| Send ring | 5840 bytes (4 segments) |
| In-flight descriptors | 8 |
| SYN/FIN retries | 5 |
| Data RTO retries | 5 |
| TIME_WAIT | 5000 ms |
| Ephemeral ports | 49152-65535 |
| Initial cwnd | 2 segments |
| Initial ssthresh | 65535 |
| Duplicate ACK threshold | 3 |
| RTO | 500 ms initial, 200 ms floor, 10 s ceiling |

## State Machine

All states are handled: CLOSED, LISTEN, SYN SENT, SYN RECEIVED, ESTABLISHED, FIN WAIT 1/2, CLOSE WAIT, CLOSING, LAST ACK, TIME WAIT. RST in any state resets immediately. Simultaneous close is supported (CLOSING); simultaneous open is not.

Listening works internally: a LISTEN socket that receives a SYN allocates a new socket in SYN RECEIVED and keeps listening. However, listen/accept are not exposed to userspace, so from applications TCP is client-only.

ISNs are FNV-1a hashes of the four-tuple mixed with a boot-seeded secret and the tick count.

## Send Path

`tcp_send()` copies application bytes into the send ring (blocking on `net_poll()` + yield while full) and returns the bytes accepted. `tcp_try_send()` transmits while unsent data exists and flight is below the flight limit:

```text
flight_limit = min(cwnd, peer_window, send_ring_size)
```

Segments are MSS-sized; a partial tail segment is held back while other data is in flight (Nagle-style). Each transmitted segment owns one of the 8 in-flight descriptors `{seq, length, sent_time, retransmitted}`. SYN/FIN travel as a single control segment outside the ring with their own retry/RTO state. Headers are always exactly 20 bytes — no TCP options are sent or parsed.

## Congestion Control

RFC 5681-style rules:

- **Forward progress** (new ACK): exit fast recovery (`cwnd = ssthresh`), slow start (`cwnd += MSS`) below ssthresh, congestion avoidance (`cwnd += MSS*MSS/cwnd`) above it.
- **Three duplicate ACKs**: enter fast recovery, fast-retransmit the oldest unacked segment; further duplicates inflate cwnd without re-triggering.
- **Fast retransmit**: `ssthresh = max(flight/2, 2*MSS)`, `cwnd = ssthresh + 3*MSS`.
- **RTO timeout**: `ssthresh = max(flight/2, 2*MSS)`, `cwnd = MSS` (collapse to one segment), backoff doubles the RTO capped at 10 s.

There is no SACK — recovery retransmits the oldest unacked segment.

## RTT Estimation

Jacobson/Karels: first sample seeds `srtt` with `rttvar = sample/2`; thereafter `rttvar = (3*rttvar + |sample - srtt|)/4` and `srtt = (7*srtt + sample)/8`. `RTO = srtt + 4*rttvar`, clamped to 200..10000 ms. Karn's rule: RTT is sampled only from newly-acked, never-retransmitted segments.

## Receive Path

In-order only: a segment is accepted when it covers the expected edge; covered prefixes are skipped; bytes append to the 64 KiB ring until full (excess dropped). Out-of-order and duplicate segments are dropped and re-ACKed — those duplicate ACKs drive the peer's fast retransmit. ACKs are immediate (no delayed ACK). The advertised window is the constant 65535 and never shrinks, even when the receive ring is full.

## Timers

`tcp_poll()` (run from `net_poll()`):

- Control segments retransmit after their RTO (doubling, capped), up to 5 retries, then reset.
- Data retransmits the oldest in-flight segment after `rto_ms`; each timeout applies the RTO policy and backoff; 5 retries without forward progress reset the socket.
- TIME_WAIT expires 5 s after last activity, re-ACKing retransmitted FINs meanwhile.
- Close is deferred until flight and unsent data drain (flush-before-FIN).

## Not Supported

No window scaling, SACK, timestamps, or MSS negotiation (no options at all). No simultaneous open, keep-alives, zero-window probing, dynamic advertised window, or delayed ACKs. Receive overflow drops data while still advertising 65535. Listen/accept exist internally but have no syscall surface.

## Validation

`src/net/tcp_congestion_tests.cpp` covers slow start into congestion avoidance, 3-dup-ACK fast retransmit, fast-recovery exit, RTO collapse, flight limiting, RTT convergence/backoff, and the zero-sample floor. `src/net/dns_tests.cpp` covers the DNS parser.
