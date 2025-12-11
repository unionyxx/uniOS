#pragma once
#include <stdint.h>

// DHCP message types
#define DHCP_DISCOVER   1
#define DHCP_OFFER      2
#define DHCP_REQUEST    3
#define DHCP_DECLINE    4
#define DHCP_ACK        5
#define DHCP_NAK        6
#define DHCP_RELEASE    7

// DHCP option codes
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_REQ      55
#define DHCP_OPT_END            255

#define DHCP_SERVER_PORT    67
#define DHCP_CLIENT_PORT    68

#define DHCP_MAGIC_COOKIE   0x63825363

// DHCP packet structure
struct DhcpPacket {
    uint8_t op;             // 1 = request, 2 = reply
    uint8_t htype;          // Hardware type (1 = Ethernet)
    uint8_t hlen;           // Hardware address length (6)
    uint8_t hops;           // Hops
    uint32_t xid;           // Transaction ID
    uint16_t secs;          // Seconds elapsed
    uint16_t flags;         // Flags
    uint32_t ciaddr;        // Client IP address
    uint32_t yiaddr;        // Your IP address
    uint32_t siaddr;        // Server IP address
    uint32_t giaddr;        // Gateway IP address
    uint8_t chaddr[16];     // Client hardware address
    uint8_t sname[64];      // Server name
    uint8_t file[128];      // Boot filename
    uint32_t magic;         // Magic cookie
    uint8_t options[308];   // Options
} __attribute__((packed));

// DHCP functions
void dhcp_init();
bool dhcp_request();      // Request IP via DHCP (blocking)
void dhcp_receive(const void* data, uint16_t length, uint32_t src_ip);
