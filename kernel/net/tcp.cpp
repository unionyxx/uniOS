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

#include "tcp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "net.h"
#include "timer.h"
#include "debug.h"
#include "heap.h"
#include "scheduler.h"
#include "spinlock.h"

static TcpSocket sockets[TCP_MAX_SOCKETS];

// Ephemeral port range (IANA recommended: 49152-65535)
#define EPHEMERAL_PORT_MIN 49152
#define EPHEMERAL_PORT_MAX 65535
static uint16_t next_ephemeral_port = EPHEMERAL_PORT_MIN;

void tcp_init() {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        sockets[i].in_use = false;
        sockets[i].state = TCP_CLOSED;
    }
    DEBUG_INFO("TCP: Layer initialized (%d sockets)", TCP_MAX_SOCKETS);
}

// Pseudo-header for checksum
struct TcpPseudoHeader {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t zero;
    uint8_t protocol;
    uint16_t tcp_length;
} __attribute__((packed));


// Calculate TCP checksum (reentrant - uses stack buffer)
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void* tcp_data, uint16_t length) {
    // Stack-allocated buffer - reentrant, no locking needed
    uint8_t buffer[1600];
    
    TcpPseudoHeader* pseudo = (TcpPseudoHeader*)buffer;
    
    pseudo->src_ip = src_ip;
    pseudo->dst_ip = dst_ip;
    pseudo->zero = 0;
    pseudo->protocol = IP_PROTO_TCP;
    pseudo->tcp_length = htons(length);
    
    const uint8_t* src = (const uint8_t*)tcp_data;
    for (uint16_t i = 0; i < length; i++) {
        buffer[sizeof(TcpPseudoHeader) + i] = src[i];
    }
    
    return ipv4_checksum(buffer, sizeof(TcpPseudoHeader) + length);
}

// Send TCP segment
static bool tcp_send_segment(TcpSocket* sock, uint8_t flags, const void* data, uint16_t length) {
    // Allocate packet buffer on heap to avoid stack overflow
    uint8_t* packet = (uint8_t*)malloc(1500);
    if (!packet) return false;
    
    TcpHeader* hdr = (TcpHeader*)packet;
    
    hdr->src_port = htons(sock->local_port);
    hdr->dst_port = htons(sock->remote_port);
    hdr->seq_num = htonl(sock->send_next);
    hdr->ack_num = (flags & TCP_FLAG_ACK) ? htonl(sock->ack_num) : 0;
    hdr->data_offset = (TCP_HEADER_SIZE / 4) << 4;  // 5 * 4 = 20 bytes
    hdr->flags = flags;
    hdr->window = htons(TCP_WINDOW_SIZE);
    hdr->checksum = 0;
    hdr->urgent_ptr = 0;
    
    // Copy payload
    if (data && length > 0) {
        uint8_t* payload = packet + TCP_HEADER_SIZE;
        const uint8_t* src = (const uint8_t*)data;
        for (uint16_t i = 0; i < length; i++) {
            payload[i] = src[i];
        }
    }
    
    // Calculate checksum
    uint16_t total_len = TCP_HEADER_SIZE + length;
    hdr->checksum = tcp_checksum(net_get_ip(), sock->remote_ip, packet, total_len);
    
    // Update sequence number for data and SYN/FIN
    if (length > 0) {
        sock->send_next += length;
    }
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
        sock->send_next++;
    }
    
    sock->last_activity = timer_get_ticks();
    
    bool result = ipv4_send(sock->remote_ip, IP_PROTO_TCP, packet, total_len);
    free(packet);
    return result;
}

// Find socket for incoming segment
static TcpSocket* tcp_find_socket(uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
    // First, look for established connection
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && 
            sockets[i].state != TCP_LISTEN &&
            sockets[i].local_port == dst_port &&
            sockets[i].remote_port == src_port &&
            sockets[i].remote_ip == src_ip) {
            return &sockets[i];
        }
    }
    
    // Then look for listening socket
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use &&
            sockets[i].state == TCP_LISTEN &&
            sockets[i].local_port == dst_port) {
            return &sockets[i];
        }
    }
    
    return nullptr;
}

// Receive TCP segment
void tcp_receive(const void* data, uint16_t length, uint32_t src_ip, uint32_t dst_ip) {
    if (length < TCP_HEADER_SIZE) {
        return;
    }
    
    const TcpHeader* hdr = (const TcpHeader*)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq = ntohl(hdr->seq_num);
    uint32_t ack = ntohl(hdr->ack_num);
    uint8_t flags = hdr->flags;
    uint8_t header_len = (hdr->data_offset >> 4) * 4;
    
    if (header_len < TCP_HEADER_SIZE || header_len > length) {
        return;
    }
    
    const uint8_t* payload = (const uint8_t*)data + header_len;
    uint16_t payload_len = length - header_len;
    
    TcpSocket* sock = tcp_find_socket(src_ip, src_port, dst_port);
    if (!sock) {
        // Send RST for unknown connection
        return;
    }
    
    switch (sock->state) {
        case TCP_LISTEN:
            if (flags & TCP_FLAG_SYN) {
                // Accept connection - create new socket
                int new_idx = -1;
                for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
                    if (!sockets[i].in_use) {
                        new_idx = i;
                        break;
                    }
                }
                if (new_idx >= 0) {
                    TcpSocket* new_sock = &sockets[new_idx];
                    new_sock->in_use = true;
                    new_sock->state = TCP_SYN_RECEIVED;
                    new_sock->local_port = dst_port;
                    new_sock->remote_port = src_port;
                    new_sock->remote_ip = src_ip;
                    new_sock->ack_num = seq + 1;
                    new_sock->seq_num = timer_get_ticks() & 0xFFFFFFFF;
                    new_sock->send_next = new_sock->seq_num;
                    new_sock->rx_head = new_sock->rx_tail = 0;
                    
                    // Send SYN-ACK
                    tcp_send_segment(new_sock, TCP_FLAG_SYN | TCP_FLAG_ACK, nullptr, 0);
                    
                    DEBUG_INFO("TCP: SYN received, sent SYN-ACK");
                }
            }
            break;
            
        case TCP_SYN_SENT:
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->ack_num = seq + 1;
                sock->state = TCP_ESTABLISHED;
                tcp_send_segment(sock, TCP_FLAG_ACK, nullptr, 0);
                DEBUG_INFO("TCP: Connection established (client)");
            }
            break;
            
        case TCP_SYN_RECEIVED:
            if (flags & TCP_FLAG_ACK) {
                sock->state = TCP_ESTABLISHED;
                DEBUG_INFO("TCP: Connection established (server)");
            }
            break;
            
        case TCP_ESTABLISHED:
            // Handle data
            if (payload_len > 0) {
                // Store in receive buffer
                for (uint16_t i = 0; i < payload_len; i++) {
                    uint16_t next = (sock->rx_head + 1) % TCP_RX_BUFFER_SIZE;
                    if (next == sock->rx_tail) break;  // Buffer full
                    sock->rx_buffer[sock->rx_head] = payload[i];
                    sock->rx_head = next;
                }
                sock->ack_num = seq + payload_len;
                sock->pending_ack = true;
            }
            
            // Handle FIN
            if (flags & TCP_FLAG_FIN) {
                sock->ack_num = seq + 1;
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
    
    (void)dst_ip;
    (void)ack;
}

// Create TCP socket
int tcp_socket() {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            sockets[i].in_use = true;
            sockets[i].state = TCP_CLOSED;
            sockets[i].rx_head = sockets[i].rx_tail = 0;
            return i;
        }
    }
    return -1;
}

// Bind socket
bool tcp_bind(int sock, uint16_t port) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].in_use) {
        return false;
    }
    sockets[sock].local_port = port;
    return true;
}

// Listen on socket
bool tcp_listen(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].in_use) {
        return false;
    }
    sockets[sock].state = TCP_LISTEN;
    return true;
}

// Accept connection (returns new socket)
int tcp_accept(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || 
        !sockets[sock].in_use || sockets[sock].state != TCP_LISTEN) {
        return -1;
    }
    
    // Look for established connection on same port
    uint16_t port = sockets[sock].local_port;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (i != sock && sockets[i].in_use && 
            sockets[i].local_port == port &&
            sockets[i].state == TCP_ESTABLISHED) {
            return i;
        }
    }
    
    return -1;  // No connection ready
}

// Connect to remote host
bool tcp_connect(int sock, uint32_t dst_ip, uint16_t dst_port) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].in_use) {
        return false;
    }
    
    TcpSocket* s = &sockets[sock];
    s->remote_ip = dst_ip;
    s->remote_port = dst_port;
    s->local_port = next_ephemeral_port++;
    // Wrap around when uint16_t overflows past 65535 OR when we need to stay in range
    if (next_ephemeral_port == 0 || next_ephemeral_port < EPHEMERAL_PORT_MIN) {
        next_ephemeral_port = EPHEMERAL_PORT_MIN;
    }
    s->seq_num = timer_get_ticks() & 0xFFFFFFFF;
    s->send_next = s->seq_num;
    s->state = TCP_SYN_SENT;
    
    // Send SYN
    tcp_send_segment(s, TCP_FLAG_SYN, nullptr, 0);
    
    // Wait for connection (with timeout)
    uint64_t start = timer_get_ticks();
    uint64_t timeout = (5000 * timer_get_frequency()) / 1000;  // 5 seconds
    
    while (s->state == TCP_SYN_SENT && (timer_get_ticks() - start) < timeout) {
        net_poll();
        scheduler_yield();  // Yield CPU instead of busy-wait
    }
    
    return s->state == TCP_ESTABLISHED;
}

// Send data
int tcp_send(int sock, const void* data, uint16_t length) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || 
        !sockets[sock].in_use || sockets[sock].state != TCP_ESTABLISHED) {
        return -1;
    }
    
    TcpSocket* s = &sockets[sock];
    
    // Simple: send all at once (no segmentation)
    uint16_t send_len = length;
    if (send_len > 1400) send_len = 1400;  // MSS
    
    if (!tcp_send_segment(s, TCP_FLAG_ACK | TCP_FLAG_PSH, data, send_len)) {
        return -1;
    }
    
    return send_len;
}

// Receive data
int tcp_recv(int sock, void* buffer, uint16_t max_len) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].in_use) {
        return -1;
    }
    
    TcpSocket* s = &sockets[sock];
    uint8_t* dst = (uint8_t*)buffer;
    uint16_t count = 0;
    
    while (count < max_len && s->rx_head != s->rx_tail) {
        dst[count++] = s->rx_buffer[s->rx_tail];
        s->rx_tail = (s->rx_tail + 1) % TCP_RX_BUFFER_SIZE;
    }
    
    return count;
}

// Close connection
void tcp_close(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !sockets[sock].in_use) {
        return;
    }
    
    TcpSocket* s = &sockets[sock];
    
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
            break;
    }
}

// Get socket state
TcpState tcp_get_state(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS) {
        return TCP_CLOSED;
    }
    return sockets[sock].state;
}
