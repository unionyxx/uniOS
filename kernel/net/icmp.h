#pragma once
#include <stdint.h>

// ICMP types
#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8

// ICMP Header
struct IcmpHeader {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

#define ICMP_HEADER_SIZE 8

// ICMP functions
void icmp_init();
void icmp_receive(const void* data, uint16_t length, uint32_t src_ip);
bool icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq);

// Ping callback
typedef void (*ping_callback_t)(uint32_t src_ip, uint16_t seq, uint16_t rtt_ms, bool success);
void icmp_set_ping_callback(ping_callback_t callback);
