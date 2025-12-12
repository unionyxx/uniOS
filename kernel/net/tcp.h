#pragma once
#include <stdint.h>

// TCP Header flags
#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

// TCP connection states
enum TcpState {
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
struct TcpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t data_offset;    // (data_offset >> 4) * 4 = header length
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

#define TCP_HEADER_SIZE 20
#define TCP_MAX_SOCKETS 16
#define TCP_WINDOW_SIZE 4096
#define TCP_RX_BUFFER_SIZE 4096

// TCP Control Block (connection state)
struct TcpSocket {
    bool in_use;
    TcpState state;
    
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;
    
    uint32_t seq_num;       // Our sequence number
    uint32_t ack_num;       // Remote's sequence we've acked
    
    uint32_t send_next;     // Next seq to send
    uint32_t send_una;      // Oldest unacked seq
    
    // Receive buffer
    uint8_t rx_buffer[TCP_RX_BUFFER_SIZE];
    uint16_t rx_head;
    uint16_t rx_tail;
    
    // Connection tracking
    bool pending_ack;
    uint64_t last_activity;
};

// TCP functions
void tcp_init();
void tcp_receive(const void* data, uint16_t length, uint32_t src_ip, uint32_t dst_ip);

// Socket-like API
int tcp_socket();
bool tcp_bind(int sock, uint16_t port);
bool tcp_listen(int sock);
int tcp_accept(int sock);
bool tcp_connect(int sock, uint32_t dst_ip, uint16_t dst_port);
int tcp_send(int sock, const void* data, uint16_t length);
int tcp_recv(int sock, void* buffer, uint16_t max_len);
void tcp_close(int sock);
TcpState tcp_get_state(int sock);
