#include "arp.h"
#include "ethernet.h"
#include "net.h"
#include "debug.h"
#include "timer.h"

// ARP table
static ArpEntry arp_table[ARP_TABLE_SIZE];

// Pending ARP request
static bool arp_waiting = false;
static uint32_t arp_waiting_ip = 0;
static uint8_t arp_waiting_mac[6];
static bool arp_resolved = false;

void arp_init() {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        arp_table[i].valid = false;
    }

}

// Add entry to ARP table
void arp_add_entry(uint32_t ip, const uint8_t* mac) {
    // Check if already exists
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            eth_mac_copy(arp_table[i].mac, mac);
            return;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip = ip;
            eth_mac_copy(arp_table[i].mac, mac);
            arp_table[i].valid = true;

            return;
        }
    }
    
    // Table full, overwrite first entry (simple eviction)
    arp_table[0].ip = ip;
    eth_mac_copy(arp_table[0].mac, mac);
    arp_table[0].valid = true;
}

// Lookup IP in ARP table
bool arp_lookup(uint32_t ip, uint8_t* out_mac) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            eth_mac_copy(out_mac, arp_table[i].mac);
            return true;
        }
    }
    return false;
}

// Send ARP request
void arp_send_request(uint32_t target_ip) {
    ArpPacket arp;
    
    arp.hw_type = htons(ARP_HW_ETHERNET);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.operation = htons(ARP_OP_REQUEST);
    
    // Sender info
    net_get_mac(arp.sender_mac);
    arp.sender_ip = net_get_ip();  // Already in network byte order
    
    // Target info (MAC is zero for request)
    for (int i = 0; i < 6; i++) arp.target_mac[i] = 0;
    arp.target_ip = target_ip;
    

    
    // Send as broadcast
    ethernet_send(ETH_BROADCAST_MAC, ETH_TYPE_ARP, &arp, sizeof(arp));
}

// Send ARP reply
static void arp_send_reply(uint32_t target_ip, const uint8_t* target_mac) {
    ArpPacket arp;
    
    arp.hw_type = htons(ARP_HW_ETHERNET);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.operation = htons(ARP_OP_REPLY);
    
    // Sender info (us)
    net_get_mac(arp.sender_mac);
    arp.sender_ip = net_get_ip();
    
    // Target info
    eth_mac_copy(arp.target_mac, target_mac);
    arp.target_ip = target_ip;
    
    // Send directly to requester
    ethernet_send(target_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

// Receive ARP packet
void arp_receive(const void* data, uint16_t length, const uint8_t* src_mac) {
    (void)src_mac;
    
    if (length < sizeof(ArpPacket)) {
        return;
    }
    
    const ArpPacket* arp = (const ArpPacket*)data;
    
    // Validate
    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET ||
        ntohs(arp->proto_type) != ETH_TYPE_IPV4 ||
        arp->hw_len != 6 || arp->proto_len != 4) {
        return;
    }
    
    // Learn sender's MAC (gratuitous learning)
    arp_add_entry(arp->sender_ip, arp->sender_mac);
    
    // Check if this is reply to our pending request
    if (arp_waiting && arp->sender_ip == arp_waiting_ip) {
        eth_mac_copy(arp_waiting_mac, arp->sender_mac);
        arp_resolved = true;
    }
    
    uint16_t op = ntohs(arp->operation);
    
    if (op == ARP_OP_REQUEST) {
        // Is this request for us?
        if (arp->target_ip == net_get_ip()) {

            arp_send_reply(arp->sender_ip, arp->sender_mac);
        }
    }
}

// Resolve IP to MAC (blocking with timeout)
bool arp_resolve(uint32_t ip, uint8_t* out_mac) {
    // Check cache first
    if (arp_lookup(ip, out_mac)) {
        return true;
    }
    
    // Broadcast address - use broadcast MAC
    if (ip == 0xFFFFFFFF) {
        eth_mac_copy(out_mac, ETH_BROADCAST_MAC);
        return true;
    }
    
    // Set up pending request
    arp_waiting = true;
    arp_waiting_ip = ip;
    arp_resolved = false;
    
    // Send request
    arp_send_request(ip);
    
    // Wait for reply (with timeout)
    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (ARP_TIMEOUT_MS * timer_get_frequency()) / 1000;
    
    while (!arp_resolved && (timer_get_ticks() - start) < timeout_ticks) {
        // Poll network
        net_poll();
        
        // Small delay
        for (volatile int i = 0; i < 10000; i++);
    }
    
    arp_waiting = false;
    
    if (arp_resolved) {
        eth_mac_copy(out_mac, arp_waiting_mac);
        return true;
    }
    
    DEBUG_WARN("ARP: Resolution timeout for %d.%d.%d.%d",
        ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    return false;
}
