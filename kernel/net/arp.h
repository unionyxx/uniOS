#pragma once
#include <stdint.h>
#include "ethernet.h"

// ARP constants
#define ARP_HW_ETHERNET     1
#define ARP_OP_REQUEST      1
#define ARP_OP_REPLY        2

// ARP Packet structure
struct ArpPacket {
    uint16_t hw_type;           // Hardware type (1 = Ethernet)
    uint16_t proto_type;        // Protocol type (0x0800 = IPv4)
    uint8_t hw_len;             // Hardware address length (6 for Ethernet)
    uint8_t proto_len;          // Protocol address length (4 for IPv4)
    uint16_t operation;         // Operation (1=request, 2=reply)
    uint8_t sender_mac[6];      // Sender MAC address
    uint32_t sender_ip;         // Sender IP address
    uint8_t target_mac[6];      // Target MAC address
    uint32_t target_ip;         // Target IP address
} __attribute__((packed));

// ARP table entry
struct ArpEntry {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
    uint32_t timestamp;         // For aging (not implemented yet)
};

#define ARP_TABLE_SIZE 32
#define ARP_TIMEOUT_MS 5000     // Timeout waiting for ARP reply

// ARP functions
void arp_init();
void arp_receive(const void* data, uint16_t length, const uint8_t* src_mac);
bool arp_resolve(uint32_t ip, uint8_t* out_mac);
void arp_send_request(uint32_t target_ip);
void arp_add_entry(uint32_t ip, const uint8_t* mac);
bool arp_lookup(uint32_t ip, uint8_t* out_mac);
