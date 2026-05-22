/**
 * @file tcp.cpp
 * @brief TCP (Transmission Control Protocol) Implementation for uniOS
 *
 * This module implements RFC 793 TCP with simplified state management.
 * It provides reliable, ordered, connection-oriented data delivery.
 *
 * Features:
 *   - Connection establishment (3-way handshake)
 *   - Data transmission with sequence numbers
 *   - Acknowledgement and basic retransmission
 *   - Connection teardown
 *
 * TCP State Machine:
 *   CLOSED → LISTEN (passive open)
 *   CLOSED → SYN_SENT → ESTABLISHED (active open)
 *   ESTABLISHED → FIN_WAIT → CLOSED (active close)
 *
 * Limitations:
 *   - No congestion control (window is fixed)
 *   - Basic retransmission (no RTT estimation)
 *   - Maximum 8 concurrent sockets
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

static inline uint64_t read_tsc() {
    uint32_t low, high;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static uint32_t tcp_generate_isn(uint32_t local_ip, uint32_t remote_ip, uint16_t local_port, uint16_t remote_port)
{
    struct {
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

void tcp_init()
{
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        spinlock_init(&sockets[i].lock);
        sockets[i].in_use = false;
        sockets[i].state = TCP_CLOSED;
        sockets[i].retransmit.in_use = false;
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
    TcpPseudoHeader *pseudo = (TcpPseudoHeader *)buffer;

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

// Send TCP segment. Must be called with sock->lock held.
static bool tcp_send_segment(TcpSocket *sock, uint8_t flags, const void *data, uint16_t length)
{
    if (!sock || (!data && length > 0) || length > 1460)
        return false;
    uint8_t *packet = static_cast<uint8_t *>(malloc(TCP_HEADER_SIZE + length));
    if (!packet)
        return false;

    TcpHeader *hdr = (TcpHeader *)packet;

    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    
    uint32_t packet_seq = sock->send_next;
    hdr->seq_num = htonl(packet_seq);
    hdr->ack_num = (flags & TCP_FLAG_ACK) ? htonl(sock->ack_num) : 0;
    hdr->data_offset = (TCP_HEADER_SIZE / 4) << 4; // 5 * 4 = 20 bytes
    hdr->flags = flags;
    hdr->window = htons(TCP_WINDOW_SIZE);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;

    // Copy payload
    if (data && length > 0) {
        uint8_t *payload = packet + TCP_HEADER_SIZE;
        const uint8_t *src = (const uint8_t *)data;
        for (uint16_t i = 0; i < length; i++) {
            payload[i] = src[i];
        }
    }

    // Calculate checksum
    uint16_t total_len = TCP_HEADER_SIZE + length;
    hdr->checksum = tcp_checksum(net_get_ip(), sock->remote_ip, packet, total_len);

    bool result = ipv4_send(sock->remote_ip, IP_PROTO_TCP, packet, total_len);
    if (result) {
        sock->send_next += length;
        if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
            sock->send_next++;
        }
        sock->last_activity = timer_get_ticks();

        // Store for retransmission if this segment consumes a sequence number
        if (length > 0 || (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN))) {
            sock->retransmit.in_use = true;
            sock->retransmit.seq_num = packet_seq;
            sock->retransmit.length = length;
            sock->retransmit.flags = flags;
            sock->retransmit.sent_time = timer_get_ticks();
            sock->retransmit.retries = 0;
            sock->retransmit.rto_ms = 500;
            if (data && length > 0) {
                memcpy(sock->retransmit.data, data, length);
            }
        }
    }
    free(packet);
    return result;
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

    if (hdr->checksum != 0 && tcp_checksum(src_ip, dst_ip, data, length) != 0) {
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

    if (flags & TCP_FLAG_RST) {
        sock->state = TCP_CLOSED;
        sock->in_use = false;
        sock->retransmit.in_use = false;
        spinlock_release_irqrestore(&sock->lock, sock_flags);
        return;
    }

    // Acknowledge outstanding retransmit packets centrally
    if (flags & TCP_FLAG_ACK) {
        sock->send_una = ack;
        if (sock->retransmit.in_use) {
            int32_t diff = (int32_t)(ack - (sock->retransmit.seq_num + sock->retransmit.length + ((sock->retransmit.flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1 : 0)));
            if (diff >= 0) {
                sock->retransmit.in_use = false;
            }
        }
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

                    new_sock->in_use = true;
                    new_sock->state = TCP_SYN_RECEIVED;
                    new_sock->local_port = dst_port;
                    new_sock->remote_port = src_port;
                    new_sock->remote_ip = src_ip;
                    new_sock->ack_num = seq + 1;
                    new_sock->seq_num = tcp_generate_isn(dst_ip, src_ip, dst_port, src_port);
                    new_sock->send_next = new_sock->seq_num;
                    new_sock->rx_head = new_sock->rx_tail = 0;
                    new_sock->retransmit.in_use = false;

                    // Send SYN-ACK
                    tcp_send_segment(new_sock, TCP_FLAG_SYN | TCP_FLAG_ACK, nullptr, 0);
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
                sock->state = TCP_ESTABLISHED;
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
                DEBUG_INFO("tcp: connection established (client)");
            }
            break;

        case TCP_SYN_RECEIVED:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_ESTABLISHED;
                DEBUG_INFO("tcp: connection established (server)");
            }
            break;

        case TCP_ESTABLISHED:
            // Handle data
            if (payload_len > 0) {
                // Store in receive buffer
                uint16_t stored = 0;
                for (uint16_t i = 0; i < payload_len; i++) {
                    uint16_t next = (sock->rx_head + 1) % TCP_RX_BUFFER_SIZE;
                    if (next == sock->rx_tail)
                        break; // Buffer full
                    sock->rx_buffer[sock->rx_head] = payload[i];
                    sock->rx_head = next;
                    stored++;
                }
                if (stored > 0) {
                    sock->ack_num = seq + stored;
                    sock->pending_ack = true;
                }
            }

            // Handle FIN
            if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + payload_len + 1;
                sock->state = TCP_CLOSE_WAIT;
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
            }

            // Send ACK if needed
            if (sock->pending_ack) {
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
                sock->pending_ack = false;
            }
            break;

        case TCP_FIN_WAIT_1:
            if ((flags & TCP_FLAG_ACK) && (flags & TCP_FLAG_FIN)) {
                sock->ack_num = seq + 1;
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
                sock->state = TCP_TIME_WAIT;
            } else if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_FIN_WAIT_2;
            } else if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + 1;
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
                sock->state = TCP_CLOSING;
            }
            break;

        case TCP_FIN_WAIT_2:
            if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + 1;
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
                sock->state = TCP_TIME_WAIT;
            }
            break;

        case TCP_CLOSE_WAIT:
            // Wait for app to close
            break;

        case TCP_CLOSING:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_TIME_WAIT;
            }
            break;

        case TCP_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_CLOSED;
                sock->in_use = false;
            }
            break;

        case TCP_TIME_WAIT:
            // Handle retransmitted FIN
            if (flags & TCP_FLAG_FIN) {
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
            }
            break;

        default:
            break;
    }

    spinlock_release_irqrestore(&sock->lock, sock_flags);
    (void)dst_ip;
    (void)ack;
}

int tcp_socket()
{
    uint64_t flags = spinlock_acquire_irqsave(&tcp_sockets_lock);
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            uint64_t sock_flags = spinlock_acquire_irqsave(&sockets[i].lock);
            sockets[i].in_use = true;
            sockets[i].state = TCP_CLOSED;
            sockets[i].rx_head = sockets[i].rx_tail = 0;
            sockets[i].pending_ack = false;
            sockets[i].retransmit.in_use = false;
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
    s->send_next = s->seq_num;
    s->send_una = s->seq_num;
    s->ack_num = 0;
    s->pending_ack = false;
    s->state = TCP_SYN_SENT;
    s->retransmit.in_use = false;

    if (!tcp_send_segment(s, TCP_FLAG_SYN, nullptr, 0)) {
        s->state = TCP_CLOSED;
        s->local_port = 0;
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
        s->state = TCP_CLOSED;
        s->local_port = 0;
        s->retransmit.in_use = false;
        spinlock_release_irqrestore(&s->lock, flags);
        return false;
    }
    spinlock_release_irqrestore(&s->lock, flags);
    return true;
}

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
    uint32_t sent = 0;

    while (sent < length) {
        uint64_t flags = spinlock_acquire_irqsave(&s->lock);
        if (!s->in_use || s->state != TCP_ESTABLISHED) {
            spinlock_release_irqrestore(&s->lock, flags);
            return sent > 0 ? sent : -1;
        }

        while (s->retransmit.in_use) {
            spinlock_release_irqrestore(&s->lock, flags);
            net_poll();
            scheduler_yield();
            flags = spinlock_acquire_irqsave(&s->lock);
            if (!s->in_use || s->state != TCP_ESTABLISHED) {
                spinlock_release_irqrestore(&s->lock, flags);
                return sent > 0 ? sent : -1;
            }
        }

        uint16_t chunk_len = length - sent;
        if (chunk_len > 1400) {
            chunk_len = 1400; // MSS
        }

        if (!tcp_send_segment(s, TCP_FLAG_ACK | TCP_FLAG_PSH, src + sent, chunk_len)) {
            spinlock_release_irqrestore(&s->lock, flags);
            return sent > 0 ? sent : -1;
        }

        sent += chunk_len;
        spinlock_release_irqrestore(&s->lock, flags);
    }

    return sent;
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

    spinlock_release_irqrestore(&s->lock, flags);
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
            s->state = TCP_FIN_WAIT_1;
            tcp_send_segment(s, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0);
            break;
        case TCP_CLOSE_WAIT:
            s->state = TCP_LAST_ACK;
            tcp_send_segment(s, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0);
            break;
        default:
            s->state = TCP_CLOSED;
            s->in_use = false;
            s->retransmit.in_use = false;
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
        uint64_t flags = spinlock_acquire_irqsave(&sockets[i].lock);
        if (!sockets[i].in_use || sockets[i].state == TCP_CLOSED || sockets[i].state == TCP_LISTEN) {
            spinlock_release_irqrestore(&sockets[i].lock, flags);
            continue;
        }

        if (sockets[i].retransmit.in_use) {
            uint64_t now = timer_get_ticks();
            uint64_t elapsed = now - sockets[i].retransmit.sent_time;
            uint64_t timeout = (sockets[i].retransmit.rto_ms * timer_get_frequency()) / 1000;

            if (elapsed >= timeout) {
                if (sockets[i].retransmit.retries >= 5) {
                    DEBUG_WARN("tcp: max retries reached for socket %d, closing", i);
                    sockets[i].state = TCP_CLOSED;
                    sockets[i].in_use = false;
                    sockets[i].retransmit.in_use = false;
                } else {
                    sockets[i].retransmit.retries++;
                    sockets[i].retransmit.rto_ms *= 2;
                    if (sockets[i].retransmit.rto_ms > 10000) {
                        sockets[i].retransmit.rto_ms = 10000;
                    }
                    sockets[i].retransmit.sent_time = now;

                    DEBUG_INFO("tcp: socket %d retransmitting seq %u, retry %d", i, 
                               sockets[i].retransmit.seq_num, sockets[i].retransmit.retries);

                    uint32_t remote_ip = sockets[i].remote_ip;
                    uint16_t remote_port = sockets[i].remote_port;
                    uint16_t local_port = sockets[i].local_port;
                    uint32_t seq_num = sockets[i].retransmit.seq_num;
                    uint32_t ack_num = sockets[i].ack_num;
                    uint8_t pkt_flags = sockets[i].retransmit.flags;
                    uint16_t payload_len = sockets[i].retransmit.length;

                    uint8_t *packet = static_cast<uint8_t *>(malloc(TCP_HEADER_SIZE + payload_len));
                    if (packet) {
                        TcpHeader *hdr = (TcpHeader *)packet;
                        hdr->src_port = htons(local_port);
                        hdr->dst_port = htons(remote_port);
                        hdr->seq_num = htonl(seq_num);
                        hdr->ack_num = (pkt_flags & TCP_FLAG_ACK) ? htonl(ack_num) : 0;
                        hdr->data_offset = (TCP_HEADER_SIZE / 4) << 4;
                        hdr->flags = pkt_flags;
                        hdr->window = htons(TCP_WINDOW_SIZE);
                        hdr->checksum = 0;
                        hdr->urgent_ptr = 0;

                        if (payload_len > 0) {
                            memcpy(packet + TCP_HEADER_SIZE, sockets[i].retransmit.data, payload_len);
                        }

                        uint16_t total_len = TCP_HEADER_SIZE + payload_len;
                        hdr->checksum = tcp_checksum(net_get_ip(), remote_ip, packet, total_len);

                        spinlock_release_irqrestore(&sockets[i].lock, flags);
                        ipv4_send(remote_ip, IP_PROTO_TCP, packet, total_len);
                        free(packet);
                        flags = spinlock_acquire_irqsave(&sockets[i].lock);
                    }
                }
            }
        }
        spinlock_release_irqrestore(&sockets[i].lock, flags);
    }
}
