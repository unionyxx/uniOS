#include "dhcp.h"
#include "udp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "net.h"
#include "timer.h"
#include "debug.h"

// DHCP state
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_server_ip = 0;
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_subnet_mask = 0;
static uint32_t dhcp_gateway = 0;
static uint32_t dhcp_dns = 0;
static bool dhcp_got_offer = false;
static bool dhcp_got_ack = false;

void dhcp_init() {
    dhcp_xid = timer_get_ticks() & 0xFFFFFFFF;
    dhcp_got_offer = false;
    dhcp_got_ack = false;

}

// Build DHCP packet
static uint16_t dhcp_build_packet(DhcpPacket* pkt, uint8_t msg_type) {
    // Clear packet
    uint8_t* p = (uint8_t*)pkt;
    for (int i = 0; i < (int)sizeof(DhcpPacket); i++) p[i] = 0;
    
    pkt->op = 1;  // BOOTREQUEST
    pkt->htype = 1;  // Ethernet
    pkt->hlen = 6;
    pkt->hops = 0;
    pkt->xid = htonl(dhcp_xid);
    pkt->secs = 0;
    pkt->flags = htons(0x8000);  // Broadcast flag
    
    // Client hardware address
    net_get_mac(pkt->chaddr);
    
    // Magic cookie
    pkt->magic = htonl(DHCP_MAGIC_COOKIE);
    
    // Options
    uint8_t* opt = pkt->options;
    int idx = 0;
    
    // Message type
    opt[idx++] = DHCP_OPT_MSG_TYPE;
    opt[idx++] = 1;
    opt[idx++] = msg_type;
    
    if (msg_type == DHCP_REQUEST) {
        // Requested IP
        opt[idx++] = DHCP_OPT_REQUESTED_IP;
        opt[idx++] = 4;
        opt[idx++] = dhcp_offered_ip & 0xFF;
        opt[idx++] = (dhcp_offered_ip >> 8) & 0xFF;
        opt[idx++] = (dhcp_offered_ip >> 16) & 0xFF;
        opt[idx++] = (dhcp_offered_ip >> 24) & 0xFF;
        
        // Server ID
        opt[idx++] = DHCP_OPT_SERVER_ID;
        opt[idx++] = 4;
        opt[idx++] = dhcp_server_ip & 0xFF;
        opt[idx++] = (dhcp_server_ip >> 8) & 0xFF;
        opt[idx++] = (dhcp_server_ip >> 16) & 0xFF;
        opt[idx++] = (dhcp_server_ip >> 24) & 0xFF;
    }
    
    // Parameter request list
    opt[idx++] = DHCP_OPT_PARAM_REQ;
    opt[idx++] = 3;
    opt[idx++] = DHCP_OPT_SUBNET_MASK;
    opt[idx++] = DHCP_OPT_ROUTER;
    opt[idx++] = DHCP_OPT_DNS;
    
    // End
    opt[idx++] = DHCP_OPT_END;
    
    return sizeof(DhcpPacket) - sizeof(pkt->options) + idx;
}

// Send DHCP packet via UDP to broadcast
static bool dhcp_send(DhcpPacket* pkt, uint16_t length) {
    // Build UDP + IP packet manually since we don't have an IP yet
    // Actually, we need to send with src_ip=0 and dst_ip=broadcast
    
    // For simplicity, use a temporary zero IP and send via Ethernet broadcast
    uint8_t frame[1500];
    
    // Build UDP header
    struct {
        uint16_t src_port;
        uint16_t dst_port;
        uint16_t length;
        uint16_t checksum;
    } __attribute__((packed)) udp;
    
    udp.src_port = htons(DHCP_CLIENT_PORT);
    udp.dst_port = htons(DHCP_SERVER_PORT);
    udp.length = htons(8 + length);
    udp.checksum = 0;  // Optional for UDP
    
    // Build IP header
    struct {
        uint8_t ihl_version;
        uint8_t tos;
        uint16_t total_length;
        uint16_t identification;
        uint16_t flags_fragment;
        uint8_t ttl;
        uint8_t protocol;
        uint16_t checksum;
        uint32_t src_ip;
        uint32_t dst_ip;
    } __attribute__((packed)) ip;
    
    ip.ihl_version = 0x45;
    ip.tos = 0;
    ip.total_length = htons(20 + 8 + length);
    ip.identification = 0;
    ip.flags_fragment = 0;
    ip.ttl = 64;
    ip.protocol = 17;  // UDP
    ip.checksum = 0;
    ip.src_ip = 0;
    ip.dst_ip = 0xFFFFFFFF;  // Broadcast
    
    // Calculate IP checksum
    ip.checksum = ipv4_checksum(&ip, 20);
    
    // Assemble frame
    uint8_t* p = frame;
    for (int i = 0; i < 20; i++) p[i] = ((uint8_t*)&ip)[i];
    p += 20;
    for (int i = 0; i < 8; i++) p[i] = ((uint8_t*)&udp)[i];
    p += 8;
    for (int i = 0; i < length; i++) p[i] = ((uint8_t*)pkt)[i];
    
    // Send via Ethernet broadcast
    return ethernet_send(ETH_BROADCAST_MAC, ETH_TYPE_IPV4, frame, 20 + 8 + length);
}


// Parse DHCP options
void dhcp_parse_options(const uint8_t* options, uint16_t length) {
    uint16_t i = 0;
    
    while (i < length) {
        uint8_t opt = options[i++];
        
        if (opt == DHCP_OPT_PAD) continue;
        if (opt == DHCP_OPT_END) break;
        
        if (i >= length) break;
        uint8_t len = options[i++];
        if (i + len > length) break;
        
        switch (opt) {
            case DHCP_OPT_SUBNET_MASK:
                if (len >= 4) {
                    dhcp_subnet_mask = options[i] | (options[i+1] << 8) |
                                       (options[i+2] << 16) | (options[i+3] << 24);
                }
                break;
            case DHCP_OPT_ROUTER:
                if (len >= 4) {
                    dhcp_gateway = options[i] | (options[i+1] << 8) |
                                  (options[i+2] << 16) | (options[i+3] << 24);
                }
                break;
            case DHCP_OPT_DNS:
                if (len >= 4) {
                    dhcp_dns = options[i] | (options[i+1] << 8) |
                              (options[i+2] << 16) | (options[i+3] << 24);
                }
                break;
            case DHCP_OPT_SERVER_ID:
                if (len >= 4) {
                    dhcp_server_ip = options[i] | (options[i+1] << 8) |
                                    (options[i+2] << 16) | (options[i+3] << 24);
                }
                break;
        }
        
        i += len;
    }
}

// Receive DHCP packet
void dhcp_receive(const void* data, uint16_t length, uint32_t src_ip) {
    (void)src_ip;
    
    if (length < sizeof(DhcpPacket) - 308) {  // Minimum DHCP size
        return;
    }
    
    const DhcpPacket* pkt = (const DhcpPacket*)data;
    
    // Verify this is for us
    if (pkt->op != 2) return;  // Must be BOOTREPLY
    if (ntohl(pkt->xid) != dhcp_xid) return;  // Transaction ID mismatch
    
    // Check magic cookie
    if (ntohl(pkt->magic) != DHCP_MAGIC_COOKIE) return;
    
    // Find message type
    uint8_t msg_type = 0;
    const uint8_t* opt = pkt->options;
    uint16_t opt_len = length - ((uint8_t*)pkt->options - (uint8_t*)pkt);
    
    for (uint16_t i = 0; i < opt_len;) {
        if (opt[i] == DHCP_OPT_PAD) { i++; continue; }
        if (opt[i] == DHCP_OPT_END) break;
        if (i + 1 >= opt_len) break;
        
        uint8_t code = opt[i++];
        uint8_t len = opt[i++];
        if (i + len > opt_len) break;
        
        if (code == DHCP_OPT_MSG_TYPE && len >= 1) {
            msg_type = opt[i];
        }
        
        i += len;
    }
    
    if (msg_type == DHCP_OFFER) {
        dhcp_offered_ip = pkt->yiaddr;
        dhcp_parse_options(pkt->options, opt_len);
        dhcp_got_offer = true;
        
    }
    else if (msg_type == DHCP_ACK) {
        dhcp_offered_ip = pkt->yiaddr;
        dhcp_parse_options(pkt->options, opt_len);
        dhcp_got_ack = true;
        
    }
}

// Request IP via DHCP (blocking)
bool dhcp_request() {
    DhcpPacket pkt;
    
    // Reset state
    dhcp_got_offer = false;
    dhcp_got_ack = false;
    dhcp_xid = timer_get_ticks() & 0xFFFFFFFF;
    

    
    // Send DISCOVER
    uint16_t len = dhcp_build_packet(&pkt, DHCP_DISCOVER);
    if (!dhcp_send(&pkt, len)) {
        DEBUG_ERROR("DHCP: Failed to send DISCOVER");
        return false;
    }
    
    // Wait for OFFER (5 second timeout)
    uint64_t start = timer_get_ticks();
    uint64_t timeout = (5000 * timer_get_frequency()) / 1000;
    
    while (!dhcp_got_offer && (timer_get_ticks() - start) < timeout) {
        net_poll();
        for (volatile int i = 0; i < 10000; i++);
    }
    
    if (!dhcp_got_offer) {
        DEBUG_WARN("DHCP: No OFFER received");
        return false;
    }
    

    
    // Send REQUEST
    len = dhcp_build_packet(&pkt, DHCP_REQUEST);
    if (!dhcp_send(&pkt, len)) {
        DEBUG_ERROR("DHCP: Failed to send REQUEST");
        return false;
    }
    
    // Wait for ACK
    start = timer_get_ticks();
    while (!dhcp_got_ack && (timer_get_ticks() - start) < timeout) {
        net_poll();
        for (volatile int i = 0; i < 10000; i++);
    }
    
    if (!dhcp_got_ack) {
        DEBUG_WARN("DHCP: No ACK received");
        return false;
    }
    
    // Configure network with received parameters
    net_set_ip(dhcp_offered_ip);
    net_set_netmask(dhcp_subnet_mask);
    net_set_gateway(dhcp_gateway);
    
    return true;
}
