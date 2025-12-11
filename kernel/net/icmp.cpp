#include "icmp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "timer.h"
#include "debug.h"

// Ping tracking
static ping_callback_t g_ping_callback = nullptr;
static uint16_t g_ping_id = 0;
static uint16_t g_ping_seq = 0;
static uint64_t g_ping_sent_time = 0;

void icmp_init() {
    g_ping_callback = nullptr;
    g_ping_id = 1234;  // Arbitrary ID
    g_ping_seq = 0;
    DEBUG_INFO("ICMP: Layer initialized");
}

void icmp_set_ping_callback(ping_callback_t callback) {
    g_ping_callback = callback;
}

// Receive ICMP packet
void icmp_receive(const void* data, uint16_t length, uint32_t src_ip) {
    if (length < ICMP_HEADER_SIZE) {
        return;
    }
    
    const IcmpHeader* hdr = (const IcmpHeader*)data;
    const uint8_t* payload = (const uint8_t*)data + ICMP_HEADER_SIZE;
    uint16_t payload_len = length - ICMP_HEADER_SIZE;
    
    switch (hdr->type) {
        case ICMP_TYPE_ECHO_REQUEST: {
            // Reply to ping
            uint8_t reply[1500];
            IcmpHeader* reply_hdr = (IcmpHeader*)reply;
            
            reply_hdr->type = ICMP_TYPE_ECHO_REPLY;
            reply_hdr->code = 0;
            reply_hdr->checksum = 0;
            reply_hdr->identifier = hdr->identifier;
            reply_hdr->sequence = hdr->sequence;
            
            // Copy payload
            for (uint16_t i = 0; i < payload_len && i < 1500 - ICMP_HEADER_SIZE; i++) {
                reply[ICMP_HEADER_SIZE + i] = payload[i];
            }
            
            // Calculate checksum
            reply_hdr->checksum = ipv4_checksum(reply, ICMP_HEADER_SIZE + payload_len);
            
            // Send reply
            ipv4_send(src_ip, IP_PROTO_ICMP, reply, ICMP_HEADER_SIZE + payload_len);
            break;
        }
        
        case ICMP_TYPE_ECHO_REPLY: {
            // Check if this is our ping reply
            if (ntohs(hdr->identifier) == g_ping_id) {
                uint16_t seq = ntohs(hdr->sequence);
                uint64_t now = timer_get_ticks();
                uint64_t rtt_ticks = now - g_ping_sent_time;
                uint16_t rtt_ms = (uint16_t)((rtt_ticks * 1000) / timer_get_frequency());
                
                DEBUG_INFO("ICMP: Echo reply from %d.%d.%d.%d seq=%d rtt=%dms",
                    src_ip & 0xFF, (src_ip >> 8) & 0xFF,
                    (src_ip >> 16) & 0xFF, (src_ip >> 24) & 0xFF,
                    seq, rtt_ms);
                
                if (g_ping_callback) {
                    g_ping_callback(src_ip, seq, rtt_ms, true);
                }
            }
            break;
        }
    }
}

// Send echo request (ping)
bool icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq) {
    uint8_t packet[64];
    IcmpHeader* hdr = (IcmpHeader*)packet;
    
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->identifier = htons(id);
    hdr->sequence = htons(seq);
    
    // Add some payload data
    uint8_t* payload = packet + ICMP_HEADER_SIZE;
    for (int i = 0; i < 56; i++) {
        payload[i] = (uint8_t)i;
    }
    
    // Calculate checksum
    hdr->checksum = ipv4_checksum(packet, ICMP_HEADER_SIZE + 56);
    
    // Track for RTT calculation
    g_ping_id = id;
    g_ping_seq = seq;
    g_ping_sent_time = timer_get_ticks();
    
    return ipv4_send(dst_ip, IP_PROTO_ICMP, packet, ICMP_HEADER_SIZE + 56);
}
