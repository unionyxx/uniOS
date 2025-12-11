#include "udp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "net.h"
#include "debug.h"

static UdpSocket sockets[UDP_MAX_SOCKETS];

void udp_init() {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        sockets[i].bound = false;
        sockets[i].rx_ready = false;
    }
    DEBUG_INFO("UDP: Layer initialized (%d sockets)", UDP_MAX_SOCKETS);
}

// Pseudo-header for checksum
struct UdpPseudoHeader {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t zero;
    uint8_t protocol;
    uint16_t udp_length;
} __attribute__((packed));

// Calculate UDP checksum with pseudo-header
static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip, const void* udp_data, uint16_t length) {
    uint8_t buffer[1600];
    UdpPseudoHeader* pseudo = (UdpPseudoHeader*)buffer;
    
    pseudo->src_ip = src_ip;
    pseudo->dst_ip = dst_ip;
    pseudo->zero = 0;
    pseudo->protocol = IP_PROTO_UDP;
    pseudo->udp_length = htons(length);
    
    // Copy UDP header and data
    const uint8_t* src = (const uint8_t*)udp_data;
    for (uint16_t i = 0; i < length; i++) {
        buffer[sizeof(UdpPseudoHeader) + i] = src[i];
    }
    
    return ipv4_checksum(buffer, sizeof(UdpPseudoHeader) + length);
}

// Receive UDP packet
void udp_receive(const void* data, uint16_t length, uint32_t src_ip, uint32_t dst_ip) {
    if (length < UDP_HEADER_SIZE) {
        return;
    }
    
    const UdpHeader* hdr = (const UdpHeader*)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t udp_len = ntohs(hdr->length);
    
    if (udp_len < UDP_HEADER_SIZE || udp_len > length) {
        return;
    }
    
    // Find socket bound to this port
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].bound && sockets[i].port == dst_port) {
            // Store in receive buffer
            const uint8_t* payload = (const uint8_t*)data + UDP_HEADER_SIZE;
            uint16_t payload_len = udp_len - UDP_HEADER_SIZE;
            
            for (uint16_t j = 0; j < payload_len && j < sizeof(sockets[i].rx_buffer); j++) {
                sockets[i].rx_buffer[j] = payload[j];
            }
            sockets[i].rx_length = payload_len;
            sockets[i].rx_src_ip = src_ip;
            sockets[i].rx_src_port = src_port;
            sockets[i].rx_ready = true;
            

            return;
        }
    }
    
    // Also handle DHCP (port 68) specially
    if (dst_port == 68) {
        extern void dhcp_receive(const void* data, uint16_t length, uint32_t src_ip);
        const uint8_t* payload = (const uint8_t*)data + UDP_HEADER_SIZE;
        uint16_t payload_len = udp_len - UDP_HEADER_SIZE;
        dhcp_receive(payload, payload_len, src_ip);
    }
    
    (void)dst_ip; // Unused
}

// Send UDP packet
bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void* data, uint16_t length) {
    if (length > 1472) {  // MTU - IP - UDP headers
        return false;
    }
    
    uint8_t packet[1500];
    UdpHeader* hdr = (UdpHeader*)packet;
    
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(UDP_HEADER_SIZE + length);
    hdr->checksum = 0;
    
    // Copy payload
    uint8_t* payload = packet + UDP_HEADER_SIZE;
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < length; i++) {
        payload[i] = src[i];
    }
    
    // Calculate checksum
    hdr->checksum = udp_checksum(net_get_ip(), dst_ip, packet, UDP_HEADER_SIZE + length);
    if (hdr->checksum == 0) {
        hdr->checksum = 0xFFFF;  // 0 means no checksum, use 0xFFFF instead
    }
    
    return ipv4_send(dst_ip, IP_PROTO_UDP, packet, UDP_HEADER_SIZE + length);
}

// Create UDP socket
int udp_socket() {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].bound) {
            sockets[i].bound = false;  // Created but not bound
            sockets[i].rx_ready = false;
            return i;
        }
    }
    return -1;
}

// Bind socket to port
bool udp_bind(int sock, uint16_t port) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS) {
        return false;
    }
    
    // Check if port already in use
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].bound && sockets[i].port == port) {
            return false;
        }
    }
    
    sockets[sock].port = port;
    sockets[sock].bound = true;
    sockets[sock].rx_ready = false;
    return true;
}

// Send via socket
bool udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void* data, uint16_t length) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS) {
        return false;
    }
    
    uint16_t src_port = sockets[sock].bound ? sockets[sock].port : 49152;  // Ephemeral
    return udp_send(dst_ip, src_port, dst_port, data, length);
}

// Receive into socket
int udp_recvfrom(int sock, void* buffer, uint16_t max_len, uint32_t* src_ip, uint16_t* src_port) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS || !sockets[sock].bound) {
        return -1;
    }
    
    if (!sockets[sock].rx_ready) {
        return 0;  // No data
    }
    
    uint16_t len = sockets[sock].rx_length;
    if (len > max_len) len = max_len;
    
    uint8_t* dst = (uint8_t*)buffer;
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = sockets[sock].rx_buffer[i];
    }
    
    if (src_ip) *src_ip = sockets[sock].rx_src_ip;
    if (src_port) *src_port = sockets[sock].rx_src_port;
    
    sockets[sock].rx_ready = false;
    return len;
}

// Close socket
void udp_close(int sock) {
    if (sock >= 0 && sock < UDP_MAX_SOCKETS) {
        sockets[sock].bound = false;
        sockets[sock].rx_ready = false;
    }
}
