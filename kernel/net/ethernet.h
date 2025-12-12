#pragma once
#include <stdint.h>

// Ethernet frame constants
#define ETH_ALEN            6       // MAC address length
#define ETH_HLEN            14      // Ethernet header length
#define ETH_DATA_LEN        1500    // Maximum payload
#define ETH_FRAME_LEN       1514    // Maximum frame (header + payload)

// EtherTypes
#define ETH_TYPE_IPV4       0x0800
#define ETH_TYPE_ARP        0x0806
#define ETH_TYPE_IPV6       0x86DD

// Ethernet header
struct EthernetHeader {
    uint8_t dst_mac[ETH_ALEN];      // Destination MAC
    uint8_t src_mac[ETH_ALEN];      // Source MAC
    uint16_t ethertype;              // EtherType (big endian!)
} __attribute__((packed));

// Broadcast MAC address
extern const uint8_t ETH_BROADCAST_MAC[6];

// Helper functions
uint16_t htons(uint16_t value);
uint16_t ntohs(uint16_t value);
uint32_t htonl(uint32_t value);
uint32_t ntohl(uint32_t value);

// Ethernet functions
void ethernet_init();
bool ethernet_send(const uint8_t* dst_mac, uint16_t ethertype, const void* data, uint16_t length);
void ethernet_receive(const void* frame, uint16_t length);

// MAC address helpers
bool eth_mac_equals(const uint8_t* mac1, const uint8_t* mac2);
bool eth_mac_is_broadcast(const uint8_t* mac);
void eth_mac_copy(uint8_t* dst, const uint8_t* src);
