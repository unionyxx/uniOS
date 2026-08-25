#pragma once
#include <kernel/net/tcp_congestion.h>
#include <kernel/sync/spinlock.h>
#include <stdint.h>

// TCP Header flags
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

// TCP connection states
enum TcpState
{
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

// TCP Header
struct TcpHeader
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t data_offset; // (data_offset >> 4) * 4 = header length
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

#define TCP_HEADER_SIZE 20
#define TCP_MAX_SOCKETS 32
#define TCP_WINDOW_SIZE 65535
#define TCP_RX_BUFFER_SIZE 65536
#define TCP_MSS 1460

// Send window: ring of TCP_TX_BUFFER_SIZE bytes plus one descriptor per
// segment that may be in flight. flight_limit() additionally bounds
// outstanding data by cwnd, the peer window and the ring capacity.
#define TCP_TX_SEGMENTS 4
#define TCP_TX_BUFFER_SIZE (TCP_TX_SEGMENTS * TCP_MSS)
#define TCP_MAX_FLIGHT_SEGMENTS 8

// In-flight data segment metadata. Ring bytes for `seq_num` live at
// tx_buffer[seq_num % TCP_TX_BUFFER_SIZE]; the flight cap keeps regions
// from overlapping.
struct TxSegment
{
    bool in_flight;
    bool retransmitted; // Karn: never sample RTT from this segment
    uint32_t seq_num;
    uint16_t length;
    uint64_t sent_time;
};

// SYN/FIN travel outside the data ring but still consume sequence space.
struct ControlSegment
{
    bool pending;
    uint8_t flags;
    uint32_t seq_num;
    uint64_t sent_time;
    uint32_t retries;
    uint32_t rto_ms;
};

struct TcpSocket
{
    Spinlock lock;
    bool in_use;
    TcpState state;

    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;

    uint32_t seq_num; // Our ISN (SYN consumes it; data starts at ISN+1)
    uint32_t ack_num; // Remote's sequence we've acked

    uint32_t send_next; // Next data seq to transmit
    uint32_t send_una;  // Oldest unacked data seq
    uint32_t tx_end;    // Highest data seq accepted from the app (>= send_next)

    // Receive buffer
    uint8_t rx_buffer[TCP_RX_BUFFER_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    // Send buffer + in-flight segment descriptors
    uint8_t tx_buffer[TCP_TX_BUFFER_SIZE];
    TxSegment tx_segments[TCP_MAX_FLIGHT_SEGMENTS];
    uint32_t rto_retries; // RTO retries since last forward progress

    // One segment-sized scratch area per socket: transmit paths build packets
    // here under the socket lock instead of malloc/free on every segment.
    uint8_t tx_scratch[TCP_HEADER_SIZE + TCP_MSS];

    // Congestion control / recovery / RTT (tcp_congestion.h)
    tcpcc::State cc;
    tcpcc::RttEstimate rtt;
    uint32_t peer_window;

    // Zero-window persist state: while the peer advertises window 0 we probe
    // with one byte on a backed-off timer instead of hanging forever.
    uint32_t persist_ms;
    uint64_t last_probe_ticks;

    // Control segment (SYN / FIN) and close sequencing
    ControlSegment ctrl;
    bool want_close;

    // Connection tracking
    bool pending_ack;
    uint64_t last_activity;
};

// TCP functions
void tcp_init();
void tcp_receive(const void *data, uint16_t length, uint32_t src_ip, uint32_t dst_ip);
void tcp_poll();

// Socket-like API
int tcp_socket();
bool tcp_bind(int sock, uint16_t port);
bool tcp_listen(int sock);
int tcp_accept(int sock);
bool tcp_connect(int sock, uint32_t dst_ip, uint16_t dst_port);
int tcp_send(int sock, const void *data, uint16_t length);
int tcp_recv(int sock, void *buffer, uint16_t max_len);
void tcp_close(int sock);
TcpState tcp_get_state(int sock);
