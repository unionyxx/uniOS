#pragma once
#include <stdint.h>

// DNS constants
#define DNS_PORT            53
#define DNS_MAX_NAME_LEN    256
#define DNS_TIMEOUT_MS      5000

// DNS header flags
#define DNS_FLAG_QR         0x8000  // Query/Response
#define DNS_FLAG_OPCODE     0x7800  // Opcode
#define DNS_FLAG_AA         0x0400  // Authoritative Answer
#define DNS_FLAG_TC         0x0200  // Truncated
#define DNS_FLAG_RD         0x0100  // Recursion Desired
#define DNS_FLAG_RA         0x0080  // Recursion Available
#define DNS_FLAG_RCODE      0x000F  // Response Code

// DNS record types
#define DNS_TYPE_A          1       // IPv4 address
#define DNS_TYPE_AAAA       28      // IPv6 address
#define DNS_TYPE_CNAME      5       // Canonical name

// DNS record classes
#define DNS_CLASS_IN        1       // Internet

// DNS header structure
struct DnsHeader {
    uint16_t id;            // Transaction ID
    uint16_t flags;         // Flags
    uint16_t qdcount;       // Question count
    uint16_t ancount;       // Answer count
    uint16_t nscount;       // Authority count
    uint16_t arcount;       // Additional count
} __attribute__((packed));

#define DNS_HEADER_SIZE 12

// DNS functions
void dns_init();
uint32_t dns_resolve(const char* hostname);  // Returns IP or 0 on failure
bool dns_is_ip_address(const char* str);     // Check if string is already an IP
uint32_t dns_parse_ip(const char* str);      // Parse IP string to uint32_t
