#include "dns.h"
#include "udp.h"
#include "net.h"
#include "ethernet.h"
#include "timer.h"
#include "debug.h"

// DNS query state
static uint16_t dns_transaction_id = 0;
static bool dns_response_received = false;
static uint32_t dns_resolved_ip = 0;

// DNS receive buffer
static uint8_t dns_rx_buffer[512];
static uint16_t dns_rx_length = 0;

void dns_init() {
    dns_transaction_id = timer_get_ticks() & 0xFFFF;

}

// Check if string is an IP address (contains only digits and dots)
bool dns_is_ip_address(const char* str) {
    if (!str || !*str) return false;
    
    int dots = 0;
    bool has_digit = false;
    
    while (*str) {
        if (*str >= '0' && *str <= '9') {
            has_digit = true;
        } else if (*str == '.') {
            dots++;
            if (!has_digit) return false;  // Empty octet
            has_digit = false;
        } else {
            return false;  // Invalid character
        }
        str++;
    }
    
    return dots == 3 && has_digit;
}

// Parse IP address string to uint32_t
uint32_t dns_parse_ip(const char* str) {
    uint32_t ip = 0;
    uint8_t octets[4] = {0, 0, 0, 0};
    int octet_idx = 0;
    
    while (*str && octet_idx < 4) {
        if (*str >= '0' && *str <= '9') {
            octets[octet_idx] = octets[octet_idx] * 10 + (*str - '0');
        } else if (*str == '.') {
            octet_idx++;
        }
        str++;
    }
    
    ip = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
    return ip;
}

// Encode hostname into DNS name format (e.g., "www.google.com" -> "\3www\6google\3com\0")
static int dns_encode_name(const char* hostname, uint8_t* out) {
    int pos = 0;
    int label_pos = 0;
    int label_len = 0;
    
    // Reserve space for first label length
    label_pos = pos++;
    
    while (*hostname) {
        if (*hostname == '.') {
            // Write label length at label_pos
            out[label_pos] = label_len;
            label_pos = pos++;
            label_len = 0;
        } else {
            out[pos++] = *hostname;
            label_len++;
        }
        hostname++;
    }
    
    // Write final label length
    out[label_pos] = label_len;
    
    // Null terminator
    out[pos++] = 0;
    
    return pos;
}

// Build DNS query packet
static int dns_build_query(const char* hostname, uint8_t* buffer) {
    DnsHeader* hdr = (DnsHeader*)buffer;
    
    // Transaction ID
    dns_transaction_id++;
    hdr->id = htons(dns_transaction_id);
    
    // Flags: Standard query, recursion desired
    hdr->flags = htons(DNS_FLAG_RD);
    
    // One question
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;
    
    // Encode hostname
    int name_len = dns_encode_name(hostname, buffer + DNS_HEADER_SIZE);
    int pos = DNS_HEADER_SIZE + name_len;
    
    // Query type (A record)
    buffer[pos++] = 0;
    buffer[pos++] = DNS_TYPE_A;
    
    // Query class (Internet)
    buffer[pos++] = 0;
    buffer[pos++] = DNS_CLASS_IN;
    
    return pos;
}

// Parse DNS response and extract IP
static uint32_t dns_parse_response(const uint8_t* buffer, uint16_t length) {
    if (length < DNS_HEADER_SIZE) {
        return 0;
    }
    
    const DnsHeader* hdr = (const DnsHeader*)buffer;
    
    // Check transaction ID
    if (ntohs(hdr->id) != dns_transaction_id) {
        DEBUG_WARN("DNS: Transaction ID mismatch");
        return 0;
    }
    
    // Check flags
    uint16_t flags = ntohs(hdr->flags);
    if (!(flags & DNS_FLAG_QR)) {
        DEBUG_WARN("DNS: Not a response");
        return 0;
    }
    
    // Check response code
    uint8_t rcode = flags & DNS_FLAG_RCODE;
    if (rcode != 0) {
        DEBUG_WARN("DNS: Error response code %d", rcode);
        return 0;
    }
    
    uint16_t qdcount = ntohs(hdr->qdcount);
    uint16_t ancount = ntohs(hdr->ancount);
    
    if (ancount == 0) {
        DEBUG_WARN("DNS: No answers");
        return 0;
    }
    
    // Skip past header
    int pos = DNS_HEADER_SIZE;
    
    // Skip questions
    for (int i = 0; i < qdcount && pos < length; i++) {
        // Skip name
        while (pos < length) {
            uint8_t len = buffer[pos];
            if (len == 0) {
                pos++;
                break;
            }
            if ((len & 0xC0) == 0xC0) {
                // Compression pointer
                pos += 2;
                break;
            }
            pos += len + 1;
        }
        // Skip type and class
        pos += 4;
    }
    
    // Parse answers
    for (int i = 0; i < ancount && pos < length; i++) {
        // Skip name (possibly compressed)
        while (pos < length) {
            uint8_t len = buffer[pos];
            if (len == 0) {
                pos++;
                break;
            }
            if ((len & 0xC0) == 0xC0) {
                // Compression pointer
                pos += 2;
                break;
            }
            pos += len + 1;
        }
        
        if (pos + 10 > length) break;
        
        // Read type and class
        uint16_t type = (buffer[pos] << 8) | buffer[pos + 1];
        // uint16_t class_ = (buffer[pos + 2] << 8) | buffer[pos + 3];
        // uint32_t ttl = (buffer[pos + 4] << 24) | (buffer[pos + 5] << 16) |
        //                (buffer[pos + 6] << 8) | buffer[pos + 7];
        uint16_t rdlength = (buffer[pos + 8] << 8) | buffer[pos + 9];
        pos += 10;
        
        if (type == DNS_TYPE_A && rdlength == 4 && pos + 4 <= length) {
            // Found an A record!
            uint32_t ip = buffer[pos] | (buffer[pos + 1] << 8) |
                         (buffer[pos + 2] << 16) | (buffer[pos + 3] << 24);
            return ip;
        }
        
        // Skip RDATA
        pos += rdlength;
    }
    
    return 0;
}

// DNS receive callback (called from UDP layer)
void dns_receive(const void* data, uint16_t length) {
    if (length > sizeof(dns_rx_buffer)) {
        length = sizeof(dns_rx_buffer);
    }
    
    const uint8_t* src = (const uint8_t*)data;
    for (uint16_t i = 0; i < length; i++) {
        dns_rx_buffer[i] = src[i];
    }
    dns_rx_length = length;
    dns_response_received = true;
}

// Resolve hostname to IP address
uint32_t dns_resolve(const char* hostname) {
    // Check if it's already an IP
    if (dns_is_ip_address(hostname)) {
        return dns_parse_ip(hostname);
    }
    
    // Get DNS server
    uint32_t dns_server = net_get_dns();
    if (dns_server == 0) {
        // Use Google DNS as fallback
        dns_server = dns_parse_ip("8.8.8.8");
    }
    

    
    // Build query
    uint8_t query[512];
    int query_len = dns_build_query(hostname, query);
    
    // Create UDP socket
    int sock = udp_socket();
    if (sock < 0) {
        DEBUG_ERROR("DNS: Failed to create socket");
        return 0;
    }
    
    // Bind to ephemeral port
    udp_bind(sock, 50000 + (dns_transaction_id % 1000));
    
    // Reset state
    dns_response_received = false;
    dns_resolved_ip = 0;
    
    // Send query
    if (!udp_sendto(sock, dns_server, DNS_PORT, query, query_len)) {
        DEBUG_ERROR("DNS: Failed to send query");
        udp_close(sock);
        return 0;
    }
    
    // Wait for response
    uint64_t start = timer_get_ticks();
    uint64_t timeout = (DNS_TIMEOUT_MS * timer_get_frequency()) / 1000;
    
    while (!dns_response_received && (timer_get_ticks() - start) < timeout) {
        // Poll network
        net_poll();
        
        // Check for UDP data
        uint8_t buffer[512];
        uint32_t src_ip;
        uint16_t src_port;
        int len = udp_recvfrom(sock, buffer, sizeof(buffer), &src_ip, &src_port);
        
        if (len > 0) {
            dns_resolved_ip = dns_parse_response(buffer, len);
            if (dns_resolved_ip != 0) {
                dns_response_received = true;
            }
        }
        
        for (volatile int i = 0; i < 1000; i++);
    }
    
    udp_close(sock);
    
    if (dns_resolved_ip != 0) {

    } else {
        DEBUG_WARN("DNS: Resolution failed for %s", hostname);
    }
    
    return dns_resolved_ip;
}
