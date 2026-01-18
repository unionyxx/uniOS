#pragma once
#include <stdint.h>

// IP Protocol numbers
#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

// IPv4 Header (no options)
struct IPv4Header {
    uint8_t ihl_version;        // Version (4 bits) + IHL (4 bits)
    uint8_t tos;                // Type of Service
    uint16_t total_length;      // Total Length (header + data)
    uint16_t identification;    // Identification
    uint16_t flags_fragment;    // Flags (3 bits) + Fragment Offset (13 bits)
    uint8_t ttl;                // Time To Live
    uint8_t protocol;           // Protocol (ICMP=1, TCP=6, UDP=17)
    uint16_t checksum;          // Header Checksum
    uint32_t src_ip;            // Source IP Address
    uint32_t dst_ip;            // Destination IP Address
} __attribute__((packed));

#define IPV4_HEADER_SIZE 20
#define IPV4_DEFAULT_TTL 64

// IPv4 functions
void ipv4_init();
void ipv4_receive(const void* data, uint16_t length);
bool ipv4_send(uint32_t dst_ip, uint8_t protocol, const void* data, uint16_t length);
uint16_t ipv4_checksum(const void* data, uint16_t length);

// IP address helpers
uint32_t ip_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void ip_format(uint32_t ip, char* buf);
