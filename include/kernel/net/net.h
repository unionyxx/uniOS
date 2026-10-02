#pragma once
#include <stdint.h>

struct NetStatus;

// Network configuration
struct NetConfig
{
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
uint8_t net_get_nic(); // NET_NIC_* from the uapi
bool net_init_done();

// Renew guard: test-and-set pair around the blocking dhcp_request() call.
// No lock is held across the call itself (it polls and yields). dhcp_tick()
// reads net_renew_in_progress() so the poll-driven T1 renewal never runs
// while a manual renew exchange is mid-flight.
bool net_renew_begin();
void net_renew_end();
bool net_renew_in_progress();

// Extended syscall implementations (user pointers validated inside):
// fills *out from the live configuration; returns 0 or -errno.
int64_t sys_net_status(NetStatus *out);
int64_t sys_net_renew(void);

// Unified NIC access (for lower layers)
bool net_send_raw(const void *data, uint16_t length);
void net_get_mac(uint8_t *out_mac);
