#include <drivers/net/e1000/e1000.h>
#include <drivers/net/rtl8139/rtl8139.h>
#include <kernel/debug.h>
#include <kernel/net/arp.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/dns.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/icmp.h>
#include <kernel/net/ipv4.h>
#include <kernel/net/net.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/sync/spinlock.h>
#include <kernel/user_ptr.h>
#include <libk/kstring.h>
#include <uapi/syscalls_ext.h>

// Global network configuration
static NetConfig g_net_config = {0, 0, 0, 0, false};

// Set when the deferred net_init() task has run (even if no NIC was found).
static bool g_net_init_done = false;

// Renew-in-flight guard for SYS_NET_RENEW. dhcp_request() blocks and polls,
// so no spinlock may be held across it; the irq.cpp stop-initiated idiom
// (atomic test-and-set) serializes concurrent callers instead.
static bool g_renew_in_progress = false;

// Active NIC type
enum NicType
{
    NIC_NONE = 0,
    NIC_E1000,
    NIC_RTL8139
};
static NicType g_active_nic = NIC_NONE;

// RX buffer for polling
static uint8_t rx_buffer[2048];

// Serializes net_poll() across cores. Nothing in the poll path runs from IRQ
// context (NICs are polled), so a plain lock suffices; without it two cores
// (idle-loop pump + a syscall blocked in a network wait) raced the shared
// rx_buffer and the NIC RX rings, duplicating and wedging frame delivery.
// Re-entrancy is gone with non-blocking ARP: nothing inside net_poll can
// call back into it.
static Spinlock net_poll_lock = SPINLOCK_INIT;

// Unified NIC functions
static bool nic_send(const void *data, uint16_t length)
{
    switch (g_active_nic) {
        case NIC_E1000:
            return e1000_send(data, length);
        case NIC_RTL8139:
            return rtl8139_send(data, length);
        default:
            return false;
    }
}

static int nic_receive(void *buffer, uint16_t max_length)
{
    switch (g_active_nic) {
        case NIC_E1000:
            return e1000_receive(buffer, max_length);
        case NIC_RTL8139:
            return rtl8139_receive(buffer, max_length);
        default:
            return 0;
    }
}

static void nic_get_mac(uint8_t *out_mac)
{
    if (!out_mac)
        return;
    for (int i = 0; i < 6; i++)
        out_mac[i] = 0;
    switch (g_active_nic) {
        case NIC_E1000:
            e1000_get_mac(out_mac);
            break;
        case NIC_RTL8139:
            rtl8139_get_mac(out_mac);
            break;
        default:
            break;
    }
}

static bool nic_link_up()
{
    switch (g_active_nic) {
        case NIC_E1000:
            return e1000_link_up();
        case NIC_RTL8139:
            return rtl8139_link_up();
        default:
            return false;
    }
}

static void nic_poll()
{
    switch (g_active_nic) {
        case NIC_E1000:
            e1000_poll();
            break;
        case NIC_RTL8139:
            rtl8139_poll();
            break;
        default:
            break;
    }
}

bool net_init()
{
    // Try Intel e1000 first (most common in VMs and laptops)
    if (e1000_init()) {
        g_active_nic = NIC_E1000;
        DEBUG_INFO("net: using Intel e1000/e1000e driver");
    }
    // Try Realtek RTL8139 (common in older hardware)
    else if (rtl8139_init()) {
        g_active_nic = NIC_RTL8139;
        DEBUG_INFO("net: using Realtek RTL8139 driver");
    } else {
        DEBUG_WARN("net: no supported NIC found, network disabled");
        g_net_init_done = true;
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
    g_net_config.dns = 0;
    g_net_config.configured = false;

    if (net_link_up()) {
        if (dhcp_request()) {
            DEBUG_INFO("net: configured by DHCP");
        } else {
            DEBUG_WARN("net: DHCP did not configure an address");
        }
    }

    g_net_init_done = true;
    return true;
}

void net_poll()
{
    if (g_active_nic == NIC_NONE)
        return;

    spinlock_acquire(&net_poll_lock);

    // Poll the active NIC
    nic_poll();

    // Receive packets, but bound the batch to avoid starving the scheduler if a driver
    // keeps reporting packets continuously.
    int len;
    int budget = 32;
    while (budget-- > 0 && (len = nic_receive(rx_buffer, sizeof(rx_buffer))) > 0) {
        ethernet_receive(rx_buffer, (uint16_t)len);
    }

    // Process TCP retransmissions
    tcp_poll();

    // Periodic DHCP lease renewal (no-op unless a lease is live and at T1).
    dhcp_tick();

    spinlock_release(&net_poll_lock);
}

// Configuration getters
uint32_t net_get_ip()
{
    return g_net_config.ip;
}

uint32_t net_get_netmask()
{
    return g_net_config.netmask;
}

uint32_t net_get_gateway()
{
    return g_net_config.gateway;
}

uint32_t net_get_dns()
{
    return g_net_config.dns;
}

// Configuration setters
void net_set_ip(uint32_t ip)
{
    g_net_config.ip = ip;
    g_net_config.configured = (ip != 0);
    if (ip == 0) {
        g_net_config.netmask = 0;
        g_net_config.gateway = 0;
        g_net_config.dns = 0;
    }
}

void net_set_netmask(uint32_t mask)
{
    g_net_config.netmask = mask;
}

void net_set_gateway(uint32_t gw)
{
    g_net_config.gateway = gw;
}

void net_set_dns(uint32_t dns)
{
    g_net_config.dns = dns;
}

// Status
bool net_is_configured()
{
    return g_net_config.configured;
}

bool net_link_up()
{
    return nic_link_up();
}

uint8_t net_get_nic()
{
    switch (g_active_nic) {
        case NIC_E1000:
            return NET_NIC_E1000;
        case NIC_RTL8139:
            return NET_NIC_RTL8139;
        default:
            return NET_NIC_NONE;
    }
}

int64_t sys_net_status(NetStatus *out)
{
    if (!validate_user_ptr(out, sizeof(NetStatus), true))
        return -14; // -EFAULT

    // Best-effort snapshot: a concurrent DHCP renew may change fields
    // mid-read (each getter reads a single aligned word).
    NetStatus status = {};
    status.ip = g_net_config.ip;
    status.netmask = g_net_config.netmask;
    status.gateway = g_net_config.gateway;
    status.dns = g_net_config.dns;
    status.link_up = net_link_up() ? 1 : 0;
    status.configured = g_net_config.configured ? 1 : 0;
    status.nic = net_get_nic();

    KSTAC();
    const bool ok = safe_copy_to_user(out, &status, sizeof(status));
    KCLAC();
    return ok ? 0 : -14; // -EFAULT
}

bool net_init_done()
{
    return g_net_init_done;
}

bool net_renew_begin()
{
    bool expected = false;
    return __atomic_compare_exchange_n(&g_renew_in_progress, &expected, true, false, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

void net_renew_end()
{
    __atomic_store_n(&g_renew_in_progress, false, __ATOMIC_RELEASE);
}

bool net_renew_in_progress()
{
    return __atomic_load_n(&g_renew_in_progress, __ATOMIC_ACQUIRE);
}

int64_t sys_net_renew(void)
{
    if (!g_net_init_done)
        return -11; // -EAGAIN: net_init() has not completed
    if (g_active_nic == NIC_NONE)
        return -19; // -ENODEV
    if (!net_renew_begin())
        return -16; // -EBUSY: a renew is already in flight

    const bool ok = dhcp_request();
    net_renew_end();
    return ok ? 0 : -100; // -ENETDOWN: DHCP exchange failed
}

// Export unified NIC functions for use by other modules
bool net_send_raw(const void *data, uint16_t length)
{
    if (g_active_nic == NIC_NONE || !data || length == 0)
        return false;
    return nic_send(data, length);
}

void net_get_mac(uint8_t *out_mac)
{
    nic_get_mac(out_mac);
}
