/**
 * @file tcp.cpp
 * @brief TCP (Transmission Control Protocol) Implementation for uniOS
 *
 * This module implements RFC 793 TCP with simplified state management.
 * It provides reliable, ordered, connection-oriented data delivery.
 *
 * Features:
 *   - Connection establishment (3-way handshake)
 *   - Windowed data transmission with multiple segments in flight
 *   - Congestion control: slow start, congestion avoidance, fast
 *     retransmit/recovery and RTO with exponential backoff (tcp_congestion.h)
 *   - RTT estimation (Jacobson/Karels) with Karn's rule
 *   - In-order receive with duplicate/out-of-order re-ACKs
 *   - Connection teardown with flush-before-FIN semantics
 *
 * TCP State Machine:
 *   CLOSED → LISTEN (passive open)
 *   CLOSED → SYN_SENT → ESTABLISHED (active open)
 *   ESTABLISHED → FIN_WAIT → CLOSED (active close)
 *
 * Limitations:
 *   - Fixed advertised receive window (no dynamic rwnd)
 *   - No SACK: recovery retransmits the oldest unacked segment
 *   - Maximum TCP_MAX_SOCKETS concurrent sockets
 *
 * Usage:
 *   tcp_socket() → tcp_connect() → tcp_send()/tcp_recv() → tcp_close()
 *
 * @see tcp.h for structure definitions and constants
 */

#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/ipv4.h>
#include <kernel/net/net.h>
#include <kernel/net/tcp.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

using kstring::memcpy;

static TcpSocket sockets[TCP_MAX_SOCKETS];
static Spinlock tcp_sockets_lock = SPINLOCK_INIT;
static uint32_t tcp_isn_secret = 0;

// Ephemeral port range (IANA recommended: 49152-65535)
#define EPHEMERAL_PORT_MIN 49152
#define EPHEMERAL_PORT_MAX 65535
static uint16_t next_ephemeral_port = EPHEMERAL_PORT_MIN;

#define TCP_CTRL_MAX_RETRIES 5
#define TCP_RTO_MAX_RETRIES 5
// 2*MSL is far longer than needed inside this host, but 5 s let stale
// duplicates of a previous incarnation of the same 4-tuple land in a fresh
// connection on busy networks.
#define TCP_TIME_WAIT_MS 30000
#define TCP_PERSIST_INITIAL_MS 1000
#define TCP_PERSIST_MAX_MS 30000

static inline uint64_t read_tsc()
{
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

// RFC 1982-style serial comparison on wrapping sequence numbers.
static inline bool seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static inline uint64_t ms_to_ticks(uint32_t ms)
{
    return ((uint64_t)ms * timer_get_frequency()) / 1000;
}

static uint32_t tcp_generate_isn(uint32_t local_ip, uint32_t remote_ip, uint16_t local_port, uint16_t remote_port)
{
    struct
    {
        uint32_t local_ip;
        uint32_t remote_ip;
        uint16_t local_port;
        uint16_t remote_port;
        uint32_t secret;
    } tuple;
    tuple.local_ip = local_ip;
    tuple.remote_ip = remote_ip;
    tuple.local_port = local_port;
    tuple.remote_port = remote_port;
    tuple.secret = tcp_isn_secret;

    uint32_t hash = 2166136261u;
    const uint8_t *data = reinterpret_cast<const uint8_t *>(&tuple);
    for (size_t i = 0; i < sizeof(tuple); i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash + (uint32_t)(timer_get_ticks() & 0xFFFFFFFF);
}

static uint16_t get_ephemeral_port()
{
    uint64_t global_flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
    if (next_ephemeral_port < EPHEMERAL_PORT_MIN)
        next_ephemeral_port = EPHEMERAL_PORT_MIN;
    uint16_t start_port = next_ephemeral_port;
    while (true) {
        uint16_t port = next_ephemeral_port++;
        if (next_ephemeral_port == 0)
            next_ephemeral_port = EPHEMERAL_PORT_MIN;

        bool in_use = false;
        for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
            if (sockets[i].in_use && sockets[i].local_port == port) {
                in_use = true;
                break;
            }
        }
        if (!in_use) {
            spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
            return port;
        }
        if (next_ephemeral_port == start_port) {
            spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
            return 0; // All ports used
        }
    }
}

// Resets connection state for reuse. Caller holds sock->lock; the spinlock
// itself is left initialized.
static void tcp_socket_reset(TcpSocket *s)
{
    s->in_use = false;
    s->state = TCP_CLOSED;
    s->local_port = 0;
    s->remote_port = 0;
    s->remote_ip = 0;
    s->seq_num = 0;
    s->ack_num = 0;
    s->send_next = 0;
    s->send_una = 0;
    s->tx_end = 0;
    s->rx_head = 0;
    s->rx_tail = 0;
    for (auto &d : s->tx_segments)
        d.in_flight = false;
    s->rto_retries = 0;
    s->cc = tcpcc::initial_state();
    s->rtt = tcpcc::initial_rtt();
    s->peer_window = TCP_WINDOW_SIZE;
    s->persist_ms = 0;
    s->last_probe_ticks = 0;
    s->ctrl.pending = false;
    s->want_close = false;
    s->pending_ack = false;
}

void tcp_init()
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        spinlock_init(&sockets[i].lock);
        tcp_socket_reset(&sockets[i]);
    }
    uint64_t tsc = read_tsc();
    tcp_isn_secret = (uint32_t)(tsc ^ (tsc >> 32) ^ timer_get_ticks());
    DEBUG_INFO("tcp: layer initialized (%d sockets)", TCP_MAX_SOCKETS);
}

// Pseudo-header for checksum
struct TcpPseudoHeader
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t zero;
    uint8_t protocol;
    uint16_t tcp_length;
} __attribute__((packed));

// Calculate TCP checksum (reentrant - stack-allocated buffer)
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp_data, uint16_t length)
{
    if (!tcp_data || length == 0 || sizeof(TcpPseudoHeader) + length > 1600)
        return 0;

    uint8_t buffer[1600];
    TcpPseudoHeader *pseudo = reinterpret_cast<TcpPseudoHeader *>(buffer);

    pseudo->src_ip = src_ip;
    pseudo->dst_ip = dst_ip;
    pseudo->zero = 0;
    pseudo->protocol = IP_PROTO_TCP;
    pseudo->tcp_length = htons(length);

    const uint8_t *src = (const uint8_t *)tcp_data;
    for (uint16_t i = 0; i < length; i++) {
        buffer[sizeof(TcpPseudoHeader) + i] = src[i];
    }

    return ipv4_checksum(buffer, sizeof(TcpPseudoHeader) + length);
}

static void tx_ring_read(const TcpSocket *s, uint32_t seq, uint8_t *dst, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
        dst[i] = s->tx_buffer[(seq + i) % TCP_TX_BUFFER_SIZE];
}

static void tx_ring_write(TcpSocket *s, uint32_t seq, const uint8_t *src, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        s->tx_buffer[(seq + i) % TCP_TX_BUFFER_SIZE] = src[i];
}

static inline uint32_t tx_flight(const TcpSocket *s)
{
    return s->send_next - s->send_una;
}

static inline uint32_t tx_unsent(const TcpSocket *s)
{
    return s->tx_end - s->send_next;
}

static inline uint32_t tx_buffered(const TcpSocket *s)
{
    return s->tx_end - s->send_una;
}

static TxSegment *tx_desc_alloc(TcpSocket *s)
{
    for (auto &d : s->tx_segments) {
        if (!d.in_flight)
            return &d;
    }
    return nullptr;
}

static TxSegment *tx_desc_oldest(TcpSocket *s)
{
    TxSegment *oldest = nullptr;
    for (auto &d : s->tx_segments) {
        if (!d.in_flight)
            continue;
        if (!oldest || seq_after(oldest->seq_num, d.seq_num))
            oldest = &d;
    }
    return oldest;
}

static void tx_desc_free_acked(TcpSocket *s)
{
    for (auto &d : s->tx_segments) {
        if (d.in_flight && !seq_after(d.seq_num + d.length, s->send_una))
            d.in_flight = false;
    }
}

// Transmit one segment. Must be called with sock->lock held: the per-socket
// scratch buffer replaces a malloc/free per segment (which also happened with
// interrupts disabled on the RX path).
static bool tcp_transmit(TcpSocket *sock, uint8_t flags, uint32_t seq, uint32_t ack_num, const void *data,
                         uint16_t length)
{
    if (!sock || (!data && length > 0) || length > TCP_MSS)
        return false;
    uint8_t *packet = sock->tx_scratch;

    TcpHeader *hdr = reinterpret_cast<TcpHeader *>(packet);
    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    hdr->seq_num = htonl(seq);
    hdr->ack_num = (flags & TCP_FLAG_ACK) ? htonl(ack_num) : 0;
    hdr->data_offset = (TCP_HEADER_SIZE / 4) << 4;
    hdr->flags = flags;
    hdr->window = htons(TCP_WINDOW_SIZE);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    if (data && length > 0)
        memcpy(packet + TCP_HEADER_SIZE, data, length);

    uint16_t total_len = TCP_HEADER_SIZE + length;
    hdr->checksum = tcp_checksum(net_get_ip(), sock->remote_ip, packet, total_len);

    bool result = ipv4_send(sock->remote_ip, IP_PROTO_TCP, packet, total_len);
    if (result)
        sock->last_activity = timer_get_ticks();
    return result;
}

// Transmit payload bytes straight out of the send ring. Caller holds
// sock->lock (guards tx_scratch).
static bool tcp_transmit_ring(TcpSocket *s, uint32_t seq, uint16_t len, bool psh)
{
    if (len == 0 || len > TCP_MSS)
        return false;
    uint8_t *packet = s->tx_scratch;

    TcpHeader *hdr = reinterpret_cast<TcpHeader *>(packet);
    hdr->src_port = htons(s->local_port);
    hdr->dst_port = htons(s->remote_port);
    hdr->seq_num = htonl(seq);
    hdr->ack_num = htonl(s->ack_num);
    hdr->data_offset = (TCP_HEADER_SIZE / 4) << 4;
    hdr->flags = TCP_FLAG_ACK | (psh ? TCP_FLAG_PSH : 0);
    hdr->window = htons(TCP_WINDOW_SIZE);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    tx_ring_read(s, seq, packet + TCP_HEADER_SIZE, len);

    uint16_t total_len = TCP_HEADER_SIZE + len;
    hdr->checksum = tcp_checksum(net_get_ip(), s->remote_ip, packet, total_len);

    bool result = ipv4_send(s->remote_ip, IP_PROTO_TCP, packet, total_len);
    if (result)
        s->last_activity = timer_get_ticks();
    return result;
}

// Forward declaration: tcp_process_ack may release window space.
static void tcp_try_send(TcpSocket *s);

// Retransmit the oldest unacked data segment (RTO or fast retransmit).
static void tcp_retransmit_oldest(TcpSocket *s)
{
    TxSegment *d = tx_desc_oldest(s);
    if (!d)
        return;
    if (tcp_transmit_ring(s, d->seq_num, d->length, true)) {
        d->retransmitted = true; // Karn: no RTT sample from this segment
        d->sent_time = timer_get_ticks();
        s->last_activity = d->sent_time;
    }
}

// Send as much queued data as cwnd, the peer window and the send buffer
// allow. Segments are MSS-sized except the tail; a partial tail is held
// back (Nagle) while other data is in flight to bound descriptor use.
// Zero-window persist probe: the peer advertised window 0 and nothing is in
// flight, so the normal send loop and RTO both stay silent. Send the next
// byte (tracked as a normal segment so ACK/RTO bookkeeping applies) to
// elicit a window update; the probe interval backs off exponentially.
static void tcp_persist_probe(TcpSocket *s)
{
    if (tx_unsent(s) == 0 || tx_flight(s) > 0)
        return;
    const uint64_t now = timer_get_ticks();
    if (s->last_probe_ticks != 0 && now - s->last_probe_ticks < ms_to_ticks(s->persist_ms))
        return;
    TxSegment *d = tx_desc_alloc(s);
    if (!d)
        return;
    uint8_t probe_byte = s->tx_buffer[s->send_next % TCP_TX_BUFFER_SIZE];
    if (tcp_transmit(s, TCP_FLAG_ACK | TCP_FLAG_PSH, s->send_next, s->ack_num, &probe_byte, 1)) {
        d->in_flight = true;
        d->retransmitted = true; // Karn: no RTT sample from a probe
        d->seq_num = s->send_next;
        d->length = 1;
        d->sent_time = now;
        s->send_next += 1;
        s->last_probe_ticks = now;
        if (s->persist_ms == 0)
            s->persist_ms = TCP_PERSIST_INITIAL_MS;
        else if (s->persist_ms < TCP_PERSIST_MAX_MS)
            s->persist_ms = s->persist_ms * 2 > TCP_PERSIST_MAX_MS ? TCP_PERSIST_MAX_MS : s->persist_ms * 2;
    }
}

static void tcp_try_send(TcpSocket *s)
{
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return;

    if (s->peer_window == 0) {
        tcp_persist_probe(s);
        return;
    }

    while (tx_unsent(s) > 0) {
        const uint32_t flight = tx_flight(s);
        const uint32_t limit = tcpcc::flight_limit(s->cc, s->peer_window, TCP_TX_BUFFER_SIZE);
        if (flight >= limit)
            break;

        uint32_t seg_len = tx_unsent(s);
        if (seg_len > TCP_MSS)
            seg_len = TCP_MSS;
        if (seg_len > limit - flight)
            seg_len = limit - flight;
        if (seg_len < TCP_MSS && flight > 0 && seg_len < tx_unsent(s))
            break; // Nagle: partial segment waits behind in-flight data

        TxSegment *d = tx_desc_alloc(s);
        if (!d)
            break;
        if (!tcp_transmit_ring(s, s->send_next, (uint16_t)seg_len, true))
            break;

        d->in_flight = true;
        d->retransmitted = false;
        d->seq_num = s->send_next;
        d->length = (uint16_t)seg_len;
        d->sent_time = timer_get_ticks();
        s->send_next += seg_len;
    }
}

// Process an incoming cumulative ACK in the data-transfer states. Returns
// true when send_una advanced.
static bool tcp_process_ack(TcpSocket *s, uint32_t ack)
{
    // FIN consumes one sequence number beyond the data space.
    uint32_t snd_max = s->send_next;
    if (s->ctrl.pending && (s->ctrl.flags & TCP_FLAG_FIN))
        snd_max++;
    if (seq_after(ack, snd_max))
        return false;

    if (ack == s->send_una) {
        // Duplicate ACK while data is outstanding drives fast retransmit.
        if (tx_flight(s) > 0 && tcpcc::on_dup_ack(s->cc)) {
            tcpcc::on_fast_retransmit(s->cc, tx_flight(s));
            tcp_retransmit_oldest(s);
        }
        return false;
    }
    if (!seq_after(ack, s->send_una))
        return false; // Stale ACK: already acked data.

    // Forward progress: sample RTT from the oldest segment when Karn allows.
    TxSegment *oldest = tx_desc_oldest(s);
    if (oldest && !oldest->retransmitted) {
        const uint64_t now = timer_get_ticks();
        if (now > oldest->sent_time && timer_get_frequency() > 0) {
            uint64_t sample_ms = (now - oldest->sent_time) * 1000 / timer_get_frequency();
            tcpcc::rtt_sample(s->rtt, sample_ms);
        }
    }

    s->send_una = ack;
    s->rto_retries = 0;
    tx_desc_free_acked(s);
    tcpcc::on_ack(s->cc);

    if (s->ctrl.pending && seq_after(s->send_una, s->ctrl.seq_num))
        s->ctrl.pending = false;

    tcp_try_send(s);
    return true;
}

static void tcp_send_fin_if_drained(TcpSocket *s)
{
    if (!s->want_close || s->ctrl.pending || tx_flight(s) > 0 || tx_unsent(s) > 0)
        return;
    if (s->state == TCP_ESTABLISHED) {
        s->ctrl = {true, TCP_FLAG_FIN | TCP_FLAG_ACK, s->send_next, timer_get_ticks(), 0, tcpcc::INITIAL_RTO_MS};
        s->state = TCP_FIN_WAIT_1;
        tcp_transmit(s, s->ctrl.flags, s->ctrl.seq_num, s->ack_num, nullptr, 0);
    } else if (s->state == TCP_CLOSE_WAIT) {
        s->ctrl = {true, TCP_FLAG_FIN | TCP_FLAG_ACK, s->send_next, timer_get_ticks(), 0, tcpcc::INITIAL_RTO_MS};
        s->state = TCP_LAST_ACK;
        tcp_transmit(s, s->ctrl.flags, s->ctrl.seq_num, s->ack_num, nullptr, 0);
    }
}

// Find socket for incoming segment
static TcpSocket *tcp_find_socket(uint32_t src_ip, uint16_t src_port, uint16_t dst_port)
{
    // First, look for established connection
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].state != TCP_LISTEN && sockets[i].local_port == dst_port &&
            sockets[i].remote_port == src_port && sockets[i].remote_ip == src_ip) {
            return &sockets[i];
        }
    }

    // Then look for listening socket
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].state == TCP_LISTEN && sockets[i].local_port == dst_port) {
            return &sockets[i];
        }
    }

    return nullptr;
}

// Receive TCP segment
void tcp_receive(const void *data, uint16_t length, uint32_t src_ip, uint32_t dst_ip)
{
    if (!data || length < TCP_HEADER_SIZE) {
        return;
    }

    const TcpHeader *hdr = (const TcpHeader *)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq = ntohl(hdr->seq_num);
    uint32_t ack = ntohl(hdr->ack_num);
    uint8_t flags = hdr->flags;
    uint8_t header_len = (hdr->data_offset >> 4) * 4;

    if (header_len < TCP_HEADER_SIZE || header_len > length) {
        return;
    }

    // The TCP checksum is mandatory; unlike UDP, zero is not a valid
    // "no checksum" sentinel.
    if (tcp_checksum(src_ip, dst_ip, data, length) != 0) {
        DEBUG_WARN("tcp: bad checksum");
        return;
    }

    const uint8_t *payload = (const uint8_t *)data + header_len;
    uint16_t payload_len = length - header_len;

    uint64_t global_flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
    TcpSocket *sock = tcp_find_socket(src_ip, src_port, dst_port);
    uint64_t sock_flags = 0;
    if (sock) {
        sock_flags = spinlock_acquire_irqsave(&sock->lock);
    }
    spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);

    if (!sock) {
        // Send RST for unknown connection
        return;
    }

    // Track the peer's advertised receive window; it caps new transmissions.
    sock->peer_window = ntohs(hdr->window);
    if (sock->peer_window > 0) {
        sock->persist_ms = 0;
        sock->last_probe_ticks = 0;
    }

    if (flags & TCP_FLAG_RST) {
        // RFC 5961: honor an RST only when its sequence matches the expected
        // receive edge, and never let an RST reset a listener — otherwise one
        // spoofed/off-window segment tears down the connection.
        if (sock->state != TCP_LISTEN && seq == sock->ack_num)
            tcp_socket_reset(sock);
        spinlock_release_irqrestore(&sock->lock, sock_flags);
        return;
    }

    switch (sock->state) {
        case TCP_LISTEN:
            if (flags & TCP_FLAG_SYN) {
                // Accept connection - create new socket
                uint64_t g_flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
                int new_idx = -1;
                for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
                    if (!sockets[i].in_use) {
                        new_idx = i;
                        break;
                    }
                }
                if (new_idx >= 0) {
                    TcpSocket *new_sock = &sockets[new_idx];
                    uint64_t new_flags = spinlock_acquire_irqsave(&new_sock->lock);
                    spinlock_release_irqrestore(&tcp_sockets_lock, g_flags);

                    tcp_socket_reset(new_sock);
                    new_sock->in_use = true;
                    new_sock->state = TCP_SYN_RECEIVED;
                    new_sock->local_port = dst_port;
                    new_sock->remote_port = src_port;
                    new_sock->remote_ip = src_ip;
                    new_sock->ack_num = seq + 1;
                    new_sock->seq_num = tcp_generate_isn(dst_ip, src_ip, dst_port, src_port);
                    // Data space begins after the SYN; the SYN itself is
                    // tracked by the control segment until it is acked.
                    new_sock->send_una = new_sock->seq_num + 1;
                    new_sock->send_next = new_sock->seq_num + 1;
                    new_sock->tx_end = new_sock->seq_num + 1;
                    new_sock->ctrl = {true, TCP_FLAG_SYN | TCP_FLAG_ACK, new_sock->seq_num, timer_get_ticks(),
                                      0,    tcpcc::INITIAL_RTO_MS};

                    tcp_transmit(new_sock, new_sock->ctrl.flags, new_sock->ctrl.seq_num, new_sock->ack_num, nullptr, 0);
                    spinlock_release_irqrestore(&new_sock->lock, new_flags);

                    DEBUG_INFO("tcp: SYN received, sent SYN-ACK");
                } else {
                    spinlock_release_irqrestore(&tcp_sockets_lock, g_flags);
                }
            }
            break;

        case TCP_SYN_SENT:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) && ack == sock->send_next) {
                sock->ack_num = seq + 1;
                sock->ctrl.pending = false;
                sock->state = TCP_ESTABLISHED;
                tcp_transmit(sock, TCP_FLAG_ACK, sock->send_next, sock->ack_num, nullptr, 0);
                DEBUG_INFO("tcp: connection established (client)");
            }
            break;

        case TCP_SYN_RECEIVED:
            if (flags & TCP_FLAG_ACK) {
                sock->ctrl.pending = false;
                sock->state = TCP_ESTABLISHED;
                tcp_try_send(sock);
                DEBUG_INFO("tcp: connection established (server)");
            }
            break;

        case TCP_ESTABLISHED:
            if (flags & TCP_FLAG_ACK)
                tcp_process_ack(sock, ack);

            // In-order receive: accept only data at the expected edge.
            // Out-of-order or duplicate segments are dropped and re-acked,
            // which produces the dup-ACKs the sender's fast retransmit uses.
            if (payload_len > 0) {
                const uint32_t expected = sock->ack_num;
                const uint32_t seg_end = seq + payload_len;
                if (!seq_after(seq, expected) && seq_after(seg_end, expected)) {
                    const uint32_t skip = expected - seq;
                    uint16_t stored = 0;
                    for (uint32_t i = skip; i < payload_len; i++) {
                        uint16_t next = (sock->rx_head + 1) % TCP_RX_BUFFER_SIZE;
                        if (next == sock->rx_tail)
                            break; // Buffer full
                        sock->rx_buffer[sock->rx_head] = payload[i];
                        sock->rx_head = next;
                        stored++;
                    }
                    sock->ack_num += stored;
                    if (stored > 0 || skip > 0)
                        sock->pending_ack = true;
                } else {
                    // Old, duplicate, or ahead of the expected edge.
                    sock->pending_ack = true;
                }
            }

            // Handle FIN: only in-order (at the expected receive edge). A
            // reordered/ahead FIN would jump ack_num past not-yet-received
            // data and silently truncate the stream.
            if (flags & TCP_FLAG_FIN) {
                if (seq + payload_len == sock->ack_num) {
                    sock->ack_num = seq + payload_len + 1;
                    sock->state = TCP_CLOSE_WAIT;
                }
                sock->pending_ack = true;
            }

            // Send ACK if needed
            if (sock->pending_ack) {
                tcp_transmit(sock, TCP_FLAG_ACK, sock->send_next, sock->ack_num, nullptr, 0);
                sock->pending_ack = false;
            }
            break;

        case TCP_FIN_WAIT_1:
            if (flags & TCP_FLAG_ACK)
                tcp_process_ack(sock, ack);
            if ((flags & TCP_FLAG_ACK) && (flags & TCP_FLAG_FIN)) {
                if (seq + payload_len == sock->ack_num) {
                    sock->ack_num = seq + payload_len + 1;
                    tcp_transmit(sock, TCP_FLAG_ACK, sock->send_next, sock->ack_num, nullptr, 0);
                    sock->state = TCP_TIME_WAIT;
                    sock->last_activity = timer_get_ticks();
                }
            } else if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_FIN_WAIT_2;
            } else if (flags & TCP_FLAG_FIN) {
                if (seq + payload_len == sock->ack_num) {
                    sock->ack_num = seq + payload_len + 1;
                    tcp_transmit(sock, TCP_FLAG_ACK, sock->send_next, sock->ack_num, nullptr, 0);
                    sock->state = TCP_CLOSING;
                }
            }
            break;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FLAG_FIN) {
                if (seq + payload_len == sock->ack_num) {
                    sock->ack_num = seq + payload_len + 1;
                    tcp_transmit(sock, TCP_FLAG_ACK, sock->send_next, sock->ack_num, nullptr, 0);
                    sock->state = TCP_TIME_WAIT;
                    sock->last_activity = timer_get_ticks();
                }
            }
            break;

        case TCP_CLOSE_WAIT:
            // Data may still flow to the peer until our FIN goes out.
            if (flags & TCP_FLAG_ACK)
                tcp_process_ack(sock, ack);
            break;

        case TCP_CLOSING:
            if (flags & TCP_FLAG_ACK) {
                tcp_process_ack(sock, ack);
                if (!sock->ctrl.pending)
                    sock->state = TCP_TIME_WAIT;
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                tcp_process_ack(sock, ack);
                if (!sock->ctrl.pending)
                    tcp_socket_reset(sock);
            }
            break;

        case TCP_TIME_WAIT:
            // Handle retransmitted FIN
            if (flags & TCP_FLAG_FIN) {
                tcp_transmit(sock, TCP_FLAG_ACK, sock->send_next, sock->ack_num, nullptr, 0);
                sock->last_activity = timer_get_ticks();
            }
            break;

        default:
            break;
    }

    spinlock_release_irqrestore(&sock->lock, sock_flags);
    (void)dst_ip;
}

int tcp_socket()
{
    uint64_t flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            uint64_t sock_flags = spinlock_acquire_irqsave(&sockets[i].lock);
            tcp_socket_reset(&sockets[i]);
            sockets[i].in_use = true;
            spinlock_release_irqrestore(&sockets[i].lock, sock_flags);
            spinlock_release_irqrestore(&tcp_sockets_lock, flags);
            return i;
        }
    }
    spinlock_release_irqrestore(&tcp_sockets_lock, flags);
    return -1;
}

bool tcp_bind(int sock, uint16_t port)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || port == 0) {
        return false;
    }
    uint64_t global_flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
    uint64_t sock_flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    if (!sockets[sock].in_use) {
        spinlock_release_irqrestore(&sockets[sock].lock, sock_flags);
        spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
        return false;
    }
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (i != sock && sockets[i].in_use && sockets[i].local_port == port && sockets[i].state != TCP_CLOSED) {
            spinlock_release_irqrestore(&sockets[sock].lock, sock_flags);
            spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
            return false;
        }
    }
    sockets[sock].local_port = port;
    spinlock_release_irqrestore(&sockets[sock].lock, sock_flags);
    spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
    return true;
}

bool tcp_listen(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) {
        return false;
    }
    uint64_t flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    if (!sockets[sock].in_use || sockets[sock].local_port == 0) {
        spinlock_release_irqrestore(&sockets[sock].lock, flags);
        return false;
    }
    sockets[sock].state = TCP_LISTEN;
    spinlock_release_irqrestore(&sockets[sock].lock, flags);
    return true;
}

int tcp_accept(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) {
        return -1;
    }
    uint64_t global_flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
    uint64_t sock_flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    if (!sockets[sock].in_use || sockets[sock].state != TCP_LISTEN) {
        spinlock_release_irqrestore(&sockets[sock].lock, sock_flags);
        spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
        return -1;
    }
    uint16_t port = sockets[sock].local_port;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (i != sock && sockets[i].in_use && sockets[i].local_port == port) {
            uint64_t client_flags = spinlock_acquire_irqsave(&sockets[i].lock);
            if (sockets[i].state == TCP_ESTABLISHED) {
                spinlock_release_irqrestore(&sockets[i].lock, client_flags);
                spinlock_release_irqrestore(&sockets[sock].lock, sock_flags);
                spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
                return i;
            }
            spinlock_release_irqrestore(&sockets[i].lock, client_flags);
        }
    }
    spinlock_release_irqrestore(&sockets[sock].lock, sock_flags);
    spinlock_release_irqrestore(&tcp_sockets_lock, global_flags);
    return -1;
}

bool tcp_connect(int sock, uint32_t dst_ip, uint16_t dst_port)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || dst_ip == 0 || dst_port == 0) {
        return false;
    }

    uint64_t flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    if (!sockets[sock].in_use) {
        spinlock_release_irqrestore(&sockets[sock].lock, flags);
        return false;
    }

    TcpSocket *s = &sockets[sock];
    s->remote_ip = dst_ip;
    s->remote_port = dst_port;

    spinlock_release_irqrestore(&s->lock, flags);
    uint16_t ephem_port = get_ephemeral_port();
    if (ephem_port == 0)
        return false;

    flags = spinlock_acquire_irqsave(&s->lock);
    s->local_port = ephem_port;
    s->seq_num = tcp_generate_isn(net_get_ip(), dst_ip, ephem_port, dst_port);
    // Data space begins after the SYN; the SYN itself rides the control
    // segment until the SYN-ACK acks it.
    s->send_una = s->seq_num + 1;
    s->send_next = s->seq_num + 1;
    s->tx_end = s->seq_num + 1;
    s->ack_num = 0;
    s->pending_ack = false;
    for (auto &d : s->tx_segments)
        d.in_flight = false;
    s->rto_retries = 0;
    s->cc = tcpcc::initial_state();
    s->rtt = tcpcc::initial_rtt();
    s->peer_window = TCP_WINDOW_SIZE;
    s->want_close = false;
    s->ctrl = {true, TCP_FLAG_SYN, s->seq_num, timer_get_ticks(), 0, tcpcc::INITIAL_RTO_MS};
    s->state = TCP_SYN_SENT;

    if (!tcp_transmit(s, TCP_FLAG_SYN, s->ctrl.seq_num, 0, nullptr, 0)) {
        tcp_socket_reset(s);
        s->in_use = true; // Keep the slot allocated so connect can be retried.
        spinlock_release_irqrestore(&s->lock, flags);
        return false;
    }

    uint64_t start = timer_get_ticks();
    uint64_t timeout = (5000 * timer_get_frequency()) / 1000; // 5 seconds

    while (s->state == TCP_SYN_SENT && (timer_get_ticks() - start) < timeout) {
        spinlock_release_irqrestore(&s->lock, flags);
        net_poll();
        scheduler_yield();
        flags = spinlock_acquire_irqsave(&s->lock);
    }

    if (s->state != TCP_ESTABLISHED) {
        tcp_socket_reset(s);
        s->in_use = true; // Keep the slot allocated so connect can be retried.
        spinlock_release_irqrestore(&s->lock, flags);
        return false;
    }
    spinlock_release_irqrestore(&s->lock, flags);
    return true;
}

// Buffers data into the send ring and transmits as much as the congestion
// window allows. Returns the number of bytes accepted by TCP.
int tcp_send(int sock, const void *data, uint16_t length)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }
    if (!data) {
        return -1;
    }

    TcpSocket *s = &sockets[sock];
    const uint8_t *src = static_cast<const uint8_t *>(data);
    uint32_t buffered = 0;

    while (buffered < length) {
        uint64_t flags = spinlock_acquire_irqsave(&s->lock);
        if (!s->in_use || (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) || s->want_close) {
            spinlock_release_irqrestore(&s->lock, flags);
            return buffered > 0 ? (int)buffered : -1;
        }

        const uint32_t space = TCP_TX_BUFFER_SIZE - tx_buffered(s);
        if (space == 0) {
            spinlock_release_irqrestore(&s->lock, flags);
            net_poll();
            scheduler_yield();
            continue;
        }

        uint32_t n = length - buffered;
        if (n > space)
            n = space;
        tx_ring_write(s, s->tx_end, src + buffered, n);
        s->tx_end += n;
        buffered += n;
        tcp_try_send(s);
        spinlock_release_irqrestore(&s->lock, flags);
    }

    return (int)buffered;
}

int tcp_recv(int sock, void *buffer, uint16_t max_len)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || max_len == 0 || !buffer) {
        return -1;
    }

    uint64_t flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    if (!sockets[sock].in_use) {
        spinlock_release_irqrestore(&sockets[sock].lock, flags);
        return -1;
    }

    TcpSocket *s = &sockets[sock];
    uint8_t *dst = (uint8_t *)buffer;
    uint16_t count = 0;

    while (count < max_len && s->rx_head != s->rx_tail) {
        dst[count++] = s->rx_buffer[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % TCP_RX_BUFFER_SIZE;
    }

    spinlock_release_irqrestore(&sockets[sock].lock, flags);
    return count;
}

void tcp_close(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) {
        return;
    }

    uint64_t flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    if (!sockets[sock].in_use) {
        spinlock_release_irqrestore(&sockets[sock].lock, flags);
        return;
    }

    TcpSocket *s = &sockets[sock];

    switch (s->state) {
        case TCP_ESTABLISHED:
        case TCP_CLOSE_WAIT:
            // Flush buffered data before the FIN goes out.
            s->want_close = true;
            tcp_send_fin_if_drained(s);
            break;
        default:
            tcp_socket_reset(s);
            break;
    }
    spinlock_release_irqrestore(&s->lock, flags);
}

TcpState tcp_get_state(int sock)
{
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) {
        return TCP_CLOSED;
    }
    uint64_t flags = spinlock_acquire_irqsave(&sockets[sock].lock);
    TcpState state = sockets[sock].state;
    spinlock_release_irqrestore(&sockets[sock].lock, flags);
    return state;
}

void tcp_poll()
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        TcpSocket *s = &sockets[i];
        uint64_t flags = spinlock_acquire_irqsave(&s->lock);
        if (!s->in_use || s->state == TCP_CLOSED || s->state == TCP_LISTEN) {
            spinlock_release_irqrestore(&s->lock, flags);
            continue;
        }

        const uint64_t now = timer_get_ticks();

        // TIME_WAIT expiry: release the socket after a quiet period.
        if (s->state == TCP_TIME_WAIT && now - s->last_activity >= ms_to_ticks(TCP_TIME_WAIT_MS)) {
            tcp_socket_reset(s);
            spinlock_release_irqrestore(&s->lock, flags);
            continue;
        }

        // Control segment (SYN / FIN) retransmission.
        if (s->ctrl.pending && now - s->ctrl.sent_time >= ms_to_ticks(s->ctrl.rto_ms)) {
            if (s->ctrl.retries >= TCP_CTRL_MAX_RETRIES) {
                DEBUG_WARN("tcp: control segment timeout on socket %d, closing", i);
                tcp_socket_reset(s);
                spinlock_release_irqrestore(&s->lock, flags);
                continue;
            }
            s->ctrl.retries++;
            s->ctrl.rto_ms = s->ctrl.rto_ms * 2 > 10000 ? 10000 : s->ctrl.rto_ms * 2;
            if (tcp_transmit(s, s->ctrl.flags, s->ctrl.seq_num, s->ack_num, nullptr, 0))
                s->ctrl.sent_time = now;
        }

        // Data RTO: retransmit the oldest unacked segment and collapse cwnd.
        if (tx_flight(s) > 0) {
            TxSegment *oldest = tx_desc_oldest(s);
            if (oldest && now - oldest->sent_time >= ms_to_ticks(s->rtt.rto_ms)) {
                if (s->rto_retries >= TCP_RTO_MAX_RETRIES) {
                    DEBUG_WARN("tcp: max retries reached for socket %d, closing", i);
                    tcp_socket_reset(s);
                    spinlock_release_irqrestore(&s->lock, flags);
                    continue;
                }
                s->rto_retries++;
                tcpcc::on_rto_timeout(s->cc, tx_flight(s));
                tcpcc::rto_backoff(s->rtt);
                DEBUG_INFO("tcp: socket %d RTO retransmit seq %u, retry %u", i, oldest->seq_num, s->rto_retries);
                tcp_retransmit_oldest(s);
            }
        }

        // Close deferred until buffered data had flushed.
        tcp_send_fin_if_drained(s);

        spinlock_release_irqrestore(&s->lock, flags);
    }
}
