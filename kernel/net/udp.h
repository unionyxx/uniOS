#pragma once
#include <stdint.h>

// UDP Header
struct UdpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;        // Header + data
    uint16_t checksum;
} __attribute__((packed));

#define UDP_HEADER_SIZE 8
#define UDP_MAX_SOCKETS 16

// UDP Socket (simplified)
struct UdpSocket {
    uint16_t port;
    bool bound;
    
    // Receive buffer (simple single packet)
    uint8_t rx_buffer[1500];
    uint16_t rx_length;
    uint32_t rx_src_ip;
    uint16_t rx_src_port;
    bool rx_ready;
};

// UDP functions
void udp_init();
void udp_receive(const void* data, uint16_t length, uint32_t src_ip, uint32_t dst_ip);
bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void* data, uint16_t length);

// Socket-like API
int udp_socket();
bool udp_bind(int sock, uint16_t port);
bool udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void* data, uint16_t length);
int udp_recvfrom(int sock, void* buffer, uint16_t max_len, uint32_t* src_ip, uint16_t* src_port);
void udp_close(int sock);
