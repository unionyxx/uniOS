#include "ipv4.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "net.h"
#include "debug.h"

static uint16_t ip_id_counter = 0;

void ipv4_init() {
    ip_id_counter = 0;
    DEBUG_INFO("IPv4: Layer initialized");
}

// Calculate one's complement checksum
uint16_t ipv4_checksum(const void* data, uint16_t length) {
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    
    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }
    
    // Handle odd byte
    if (length > 0) {
        sum += *(const uint8_t*)ptr;
    }
    
    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return (uint16_t)~sum;
}

// Create IP address from bytes
uint32_t ip_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

// Format IP address to string
void ip_format(uint32_t ip, char* buf) {
    int i = 0;
    
    auto append_num = [&](uint8_t n) {
        if (n >= 100) buf[i++] = '0' + (n / 100);
        if (n >= 10) buf[i++] = '0' + ((n / 10) % 10);
        buf[i++] = '0' + (n % 10);
    };
    
    append_num(ip & 0xFF);
    buf[i++] = '.';
    append_num((ip >> 8) & 0xFF);
    buf[i++] = '.';
    append_num((ip >> 16) & 0xFF);
    buf[i++] = '.';
    append_num((ip >> 24) & 0xFF);
    buf[i] = '\0';
}

// Receive IPv4 packet
void ipv4_receive(const void* data, uint16_t length) {
    if (length < IPV4_HEADER_SIZE) {
        return;
    }
    
    const IPv4Header* hdr = (const IPv4Header*)data;
    
    // Check version
    uint8_t version = (hdr->ihl_version >> 4) & 0x0F;
    if (version != 4) {
        return;
    }
    
    // Get header length
    uint8_t ihl = (hdr->ihl_version & 0x0F) * 4;
    if (ihl < 20 || ihl > length) {
        return;
    }
    
    // Verify checksum
    uint16_t orig_checksum = hdr->checksum;
    if (orig_checksum != 0) {
        // Calculate checksum over header
        uint8_t header_copy[60];
        for (int i = 0; i < ihl; i++) {
            header_copy[i] = ((const uint8_t*)data)[i];
        }
        ((IPv4Header*)header_copy)->checksum = 0;
        
        if (ipv4_checksum(header_copy, ihl) != orig_checksum) {
            DEBUG_WARN("IPv4: Bad checksum");
            return;
        }
    }
    
    // Check if packet is for us
    uint32_t our_ip = net_get_ip();
    if (hdr->dst_ip != our_ip && 
        hdr->dst_ip != 0xFFFFFFFF &&  // Broadcast
        (hdr->dst_ip & 0xFF000000) != 0xFF000000) {  // Not class E / multicast
        return;
    }
    
    // Get payload
    const uint8_t* payload = (const uint8_t*)data + ihl;
    uint16_t payload_len = ntohs(hdr->total_length) - ihl;
    
    if (payload_len > length - ihl) {
        payload_len = length - ihl;
    }
    
    // Demultiplex by protocol
    switch (hdr->protocol) {
        case IP_PROTO_ICMP:
            icmp_receive(payload, payload_len, hdr->src_ip);
            break;
        case IP_PROTO_UDP:
            udp_receive(payload, payload_len, hdr->src_ip, hdr->dst_ip);
            break;
        case IP_PROTO_TCP:
            tcp_receive(payload, payload_len, hdr->src_ip, hdr->dst_ip);
            break;
        default:
            // Unknown protocol
            break;
    }
}

// Send IPv4 packet
bool ipv4_send(uint32_t dst_ip, uint8_t protocol, const void* data, uint16_t length) {
    if (length > 1480) {  // MTU - IP header
        DEBUG_WARN("IPv4: Payload too large");
        return false;
    }
    
    // Build packet
    uint8_t packet[1500];
    IPv4Header* hdr = (IPv4Header*)packet;
    
    hdr->ihl_version = 0x45;  // Version 4, IHL 5 (20 bytes)
    hdr->tos = 0;
    hdr->total_length = htons(IPV4_HEADER_SIZE + length);
    hdr->identification = htons(ip_id_counter++);
    hdr->flags_fragment = 0;  // No fragmentation
    hdr->ttl = IPV4_DEFAULT_TTL;
    hdr->protocol = protocol;
    hdr->checksum = 0;
    hdr->src_ip = net_get_ip();
    hdr->dst_ip = dst_ip;
    
    // Calculate header checksum
    hdr->checksum = ipv4_checksum(hdr, IPV4_HEADER_SIZE);
    
    // Copy payload
    uint8_t* payload = packet + IPV4_HEADER_SIZE;
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < length; i++) {
        payload[i] = src[i];
    }
    
    // Resolve MAC address
    uint8_t dst_mac[6];
    
    // Check if destination is on local network or needs gateway
    uint32_t our_ip = net_get_ip();
    uint32_t netmask = net_get_netmask();
    uint32_t gateway = net_get_gateway();
    
    uint32_t resolve_ip;
    if ((dst_ip & netmask) == (our_ip & netmask)) {
        // Same network - resolve directly
        resolve_ip = dst_ip;
    } else if (gateway != 0) {
        // Different network - send to gateway
        resolve_ip = gateway;
    } else {
        // No gateway configured - try direct
        resolve_ip = dst_ip;
    }
    
    if (!arp_resolve(resolve_ip, dst_mac)) {
        DEBUG_WARN("IPv4: Failed to resolve MAC for %d.%d.%d.%d",
            resolve_ip & 0xFF, (resolve_ip >> 8) & 0xFF,
            (resolve_ip >> 16) & 0xFF, (resolve_ip >> 24) & 0xFF);
        return false;
    }
    
    // Send via Ethernet
    return ethernet_send(dst_mac, ETH_TYPE_IPV4, packet, IPV4_HEADER_SIZE + length);
}
