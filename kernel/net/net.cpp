#include "net.h"
#include "e1000.h"
#include "rtl8139.h"
#include "ethernet.h"
#include "arp.h"
#include "ipv4.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "dhcp.h"
#include "dns.h"
#include "debug.h"

// Global network configuration
static NetConfig g_net_config = {0, 0, 0, 0, false};

// Active NIC type
enum NicType {
    NIC_NONE = 0,
    NIC_E1000,
    NIC_RTL8139
};
static NicType g_active_nic = NIC_NONE;

// RX buffer for polling
static uint8_t rx_buffer[2048];

// Unified NIC functions
static bool nic_send(const void* data, uint16_t length) {
    switch (g_active_nic) {
        case NIC_E1000:   return e1000_send(data, length);
        case NIC_RTL8139: return rtl8139_send(data, length);
        default:          return false;
    }
}

static int nic_receive(void* buffer, uint16_t max_length) {
    switch (g_active_nic) {
        case NIC_E1000:   return e1000_receive(buffer, max_length);
        case NIC_RTL8139: return rtl8139_receive(buffer, max_length);
        default:          return 0;
    }
}

static void nic_get_mac(uint8_t* out_mac) {
    switch (g_active_nic) {
        case NIC_E1000:   e1000_get_mac(out_mac); break;
        case NIC_RTL8139: rtl8139_get_mac(out_mac); break;
        default:          break;
    }
}

static bool nic_link_up() {
    switch (g_active_nic) {
        case NIC_E1000:   return e1000_link_up();
        case NIC_RTL8139: return rtl8139_link_up();
        default:          return false;
    }
}

static void nic_poll() {
    switch (g_active_nic) {
        case NIC_E1000:   e1000_poll(); break;
        case NIC_RTL8139: rtl8139_poll(); break;
        default:          break;
    }
}

bool net_init() {

    
    // Try Intel e1000 first (most common in VMs and laptops)
    if (e1000_init()) {
        g_active_nic = NIC_E1000;
        DEBUG_INFO("Net: Using Intel e1000/e1000e driver");
    }
    // Try Realtek RTL8139 (common in older hardware)
    else if (rtl8139_init()) {
        g_active_nic = NIC_RTL8139;
        DEBUG_INFO("Net: Using Realtek RTL8139 driver");
    }
    else {
        DEBUG_WARN("Net: No supported NIC found, network disabled");
        return false;
    }
    
    // Initialize protocol layers
    ethernet_init();
    arp_init();
    ipv4_init();
    icmp_init();
    udp_init();
    tcp_init();
    dhcp_init();
    dns_init();
    
    // Set default IP (can be overridden by DHCP)
    g_net_config.ip = 0;
    g_net_config.netmask = 0;
    g_net_config.gateway = 0;
    g_net_config.configured = false;
    

    return true;
}

void net_poll() {
    // Poll the active NIC
    nic_poll();
    
    // Receive packets
    int len;
    while ((len = nic_receive(rx_buffer, sizeof(rx_buffer))) > 0) {
        ethernet_receive(rx_buffer, len);
    }
}

// Configuration getters
uint32_t net_get_ip() {
    return g_net_config.ip;
}

uint32_t net_get_netmask() {
    return g_net_config.netmask;
}

uint32_t net_get_gateway() {
    return g_net_config.gateway;
}

uint32_t net_get_dns() {
    return g_net_config.dns;
}

// Configuration setters
void net_set_ip(uint32_t ip) {
    g_net_config.ip = ip;
    g_net_config.configured = (ip != 0);
}

void net_set_netmask(uint32_t mask) {
    g_net_config.netmask = mask;
}

void net_set_gateway(uint32_t gw) {
    g_net_config.gateway = gw;
}

void net_set_dns(uint32_t dns) {
    g_net_config.dns = dns;
}

// Status
bool net_is_configured() {
    return g_net_config.configured;
}

bool net_link_up() {
    return nic_link_up();
}

// Export unified NIC functions for use by other modules
bool net_send_raw(const void* data, uint16_t length) {
    return nic_send(data, length);
}

void net_get_mac(uint8_t* out_mac) {
    nic_get_mac(out_mac);
}
