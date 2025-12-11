#include "ethernet.h"
#include "net.h"
#include "arp.h"
#include "ipv4.h"
#include "debug.h"

// Broadcast MAC
const uint8_t ETH_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Byte order conversion
uint16_t htons(uint16_t value) {
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
}

uint16_t ntohs(uint16_t value) {
    return htons(value);
}

uint32_t htonl(uint32_t value) {
    return ((value & 0xFF) << 24) | 
           ((value & 0xFF00) << 8) | 
           ((value >> 8) & 0xFF00) |
           ((value >> 24) & 0xFF);
}

uint32_t ntohl(uint32_t value) {
    return htonl(value);
}

// MAC address helpers
bool eth_mac_equals(const uint8_t* mac1, const uint8_t* mac2) {
    for (int i = 0; i < 6; i++) {
        if (mac1[i] != mac2[i]) return false;
    }
    return true;
}

bool eth_mac_is_broadcast(const uint8_t* mac) {
    return eth_mac_equals(mac, ETH_BROADCAST_MAC);
}

void eth_mac_copy(uint8_t* dst, const uint8_t* src) {
    for (int i = 0; i < 6; i++) {
        dst[i] = src[i];
    }
}

void ethernet_init() {

}

// Send Ethernet frame
bool ethernet_send(const uint8_t* dst_mac, uint16_t ethertype, const void* data, uint16_t length) {
    if (length > ETH_DATA_LEN) {
        DEBUG_WARN("Ethernet: Payload too large (%d > %d)", length, ETH_DATA_LEN);
        return false;
    }
    
    // Build frame
    uint8_t frame[ETH_FRAME_LEN];
    EthernetHeader* hdr = (EthernetHeader*)frame;
    
    // Set destination MAC
    eth_mac_copy(hdr->dst_mac, dst_mac);
    
    // Set source MAC (via unified NIC layer)
    net_get_mac(hdr->src_mac);
    
    // Set EtherType (network byte order)
    hdr->ethertype = htons(ethertype);
    
    // Copy payload
    uint8_t* payload = frame + ETH_HLEN;
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < length; i++) {
        payload[i] = src[i];
    }
    
    // Send via unified NIC layer
    return net_send_raw(frame, ETH_HLEN + length);
}

// Process received Ethernet frame
void ethernet_receive(const void* frame, uint16_t length) {
    if (length < ETH_HLEN) {
        return; // Too short
    }
    
    const EthernetHeader* hdr = (const EthernetHeader*)frame;
    const uint8_t* payload = (const uint8_t*)frame + ETH_HLEN;
    uint16_t payload_len = length - ETH_HLEN;
    
    // Check if frame is for us
    uint8_t our_mac[6];
    net_get_mac(our_mac);
    
    if (!eth_mac_equals(hdr->dst_mac, our_mac) && 
        !eth_mac_is_broadcast(hdr->dst_mac)) {
        return; // Not for us
    }
    
    // Demultiplex by EtherType
    uint16_t ethertype = ntohs(hdr->ethertype);
    
    switch (ethertype) {
        case ETH_TYPE_ARP:
            arp_receive(payload, payload_len, hdr->src_mac);
            break;
        case ETH_TYPE_IPV4:
            ipv4_receive(payload, payload_len);
            break;
        default:
            // Unknown protocol, ignore
            break;
    }
}
