#pragma once
#include <stdint.h>

// Network configuration
struct NetConfig {
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    bool configured;
};

// Network initialization
bool net_init();
void net_poll();

// Configuration getters/setters
uint32_t net_get_ip();
uint32_t net_get_netmask();
uint32_t net_get_gateway();
uint32_t net_get_dns();

void net_set_ip(uint32_t ip);
void net_set_netmask(uint32_t mask);
void net_set_gateway(uint32_t gw);
void net_set_dns(uint32_t dns);

// Status
bool net_is_configured();
bool net_link_up();

// Unified NIC access (for lower layers)
bool net_send_raw(const void* data, uint16_t length);
void net_get_mac(uint8_t* out_mac);
