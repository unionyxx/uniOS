#include <kernel/debug.h>
#include <kernel/net/arp.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/net.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>

static ArpEntry arp_table[ARP_TABLE_SIZE];
// Guards the ARP table and the pending-resolution state. Resolution can be
// re-entered (net_poll() while waiting can itself trigger a send that needs a
// different address), so both the table and the waiting state are locked.
static Spinlock arp_lock = SPINLOCK_INIT;

static bool arp_waiting = false;
static uint32_t arp_waiting_ip = 0;
static uint8_t arp_waiting_mac[6];
static bool arp_resolved = false;

static bool arp_mac_is_unusable(const uint8_t *mac)
{
    if (!mac)
        return true;
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        all_zero = all_zero && mac[i] == 0;
        all_ff = all_ff && mac[i] == 0xFF;
    }
    return all_zero || all_ff;
}

void arp_init()
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        arp_table[i].valid = false;
    }
}

void arp_add_entry(uint32_t ip, const uint8_t *mac)
{
    if (ip == 0 || ip == 0xFFFFFFFF || arp_mac_is_unusable(mac))
        return;

    uint64_t flags = spinlock_acquire_irqsave(&arp_lock);

    // Check if already exists
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            eth_mac_copy(arp_table[i].mac, mac);
            spinlock_release_irqrestore(&arp_lock, flags);
            return;
        }
    }

    // Find empty slot
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip = ip;
            eth_mac_copy(arp_table[i].mac, mac);
            arp_table[i].valid = true;
            spinlock_release_irqrestore(&arp_lock, flags);
            return;
        }
    }

    // Table full, overwrite first entry (simple eviction)
    arp_table[0].ip = ip;
    eth_mac_copy(arp_table[0].mac, mac);
    arp_table[0].valid = true;
    spinlock_release_irqrestore(&arp_lock, flags);
}

bool arp_lookup(uint32_t ip, uint8_t *out_mac)
{
    if (!out_mac || ip == 0)
        return false;
    uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            eth_mac_copy(out_mac, arp_table[i].mac);
            spinlock_release_irqrestore(&arp_lock, flags);
            return true;
        }
    }
    spinlock_release_irqrestore(&arp_lock, flags);
    return false;
}

void arp_send_request(uint32_t target_ip)
{
    if (target_ip == 0 || target_ip == 0xFFFFFFFF)
        return;
    ArpPacket arp;

    arp.hw_type = htons(ARP_HW_ETHERNET);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.operation = htons(ARP_OP_REQUEST);

    // Sender info
    net_get_mac(arp.sender_mac);
    arp.sender_ip = net_get_ip(); // Already in network byte order

    // Target info (MAC is zero for request)
    for (int i = 0; i < 6; i++)
        arp.target_mac[i] = 0;
    arp.target_ip = target_ip;

    // Send as broadcast
    ethernet_send(ETH_BROADCAST_MAC, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_send_reply(uint32_t target_ip, const uint8_t *target_mac)
{
    if (target_ip == 0 || arp_mac_is_unusable(target_mac))
        return;
    ArpPacket arp;

    arp.hw_type = htons(ARP_HW_ETHERNET);
    arp.proto_type = htons(ETH_TYPE_IPV4);
    arp.hw_len = 6;
    arp.proto_len = 4;
    arp.operation = htons(ARP_OP_REPLY);

    // Sender info (us)
    net_get_mac(arp.sender_mac);
    arp.sender_ip = net_get_ip();

    // Target info
    eth_mac_copy(arp.target_mac, target_mac);
    arp.target_ip = target_ip;

    // Send directly to requester
    ethernet_send(target_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

void arp_receive(const void *data, uint16_t length, const uint8_t *src_mac)
{
    (void)src_mac;

    if (!data || length < sizeof(ArpPacket)) {
        return;
    }

    const ArpPacket *arp = (const ArpPacket *)data;

    // Validate
    if (ntohs(arp->hw_type) != ARP_HW_ETHERNET || ntohs(arp->proto_type) != ETH_TYPE_IPV4 || arp->hw_len != 6 ||
        arp->proto_len != 4) {
        return;
    }

    // Learn sender's MAC (gratuitous learning)
    arp_add_entry(arp->sender_ip, arp->sender_mac);

    // Check if this is reply to our pending request
    {
        uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
        if (arp_waiting && arp->sender_ip == arp_waiting_ip) {
            eth_mac_copy(arp_waiting_mac, arp->sender_mac);
            arp_resolved = true;
        }
        spinlock_release_irqrestore(&arp_lock, flags);
    }

    uint16_t op = ntohs(arp->operation);

    if (op == ARP_OP_REQUEST) {
        // Is this request for us?
        if (arp->target_ip == net_get_ip()) {
            arp_send_reply(arp->sender_ip, arp->sender_mac);
        }
    }
}

bool arp_resolve(uint32_t ip, uint8_t *out_mac)
{
    if (!out_mac || ip == 0)
        return false;

    // Broadcast address - use broadcast MAC
    if (ip == 0xFFFFFFFF) {
        eth_mac_copy(out_mac, ETH_BROADCAST_MAC);
        return true;
    }

    // Check cache first
    if (arp_lookup(ip, out_mac)) {
        return true;
    }

    // Claim the pending-resolution slot. Resolution is re-entrant (net_poll()
    // below can itself send a packet that needs a *different* address), so if
    // a resolution is already in flight we must not clobber its target. In
    // that case we skip sending a fresh request and simply wait for the table
    // to be populated (replies are learned gratuitously by arp_add_entry).
    bool started_wait = false;
    {
        uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
        if (!arp_waiting) {
            arp_waiting = true;
            arp_waiting_ip = ip;
            arp_resolved = false;
            started_wait = true;
        }
        spinlock_release_irqrestore(&arp_lock, flags);
    }

    if (started_wait)
        arp_send_request(ip);

    // Wait for reply (with timeout)
    uint64_t start = timer_get_ticks();
    uint64_t timeout_ticks = (ARP_TIMEOUT_MS * timer_get_frequency()) / 1000;

    while ((timer_get_ticks() - start) < timeout_ticks) {
        // Poll network
        net_poll();

        {
            uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
            bool done = arp_resolved && arp_waiting_ip == ip;
            spinlock_release_irqrestore(&arp_lock, flags);
            if (done)
                break;
        }
        // Also accept an entry learned gratuitously while we waited.
        if (arp_lookup(ip, out_mac)) {
            uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
            if (started_wait)
                arp_waiting = false;
            spinlock_release_irqrestore(&arp_lock, flags);
            return true;
        }

        // Small delay
        for (volatile int i = 0; i < 10000; i++)
            ;
    }

    {
        uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
        bool done = arp_resolved && arp_waiting_ip == ip;
        if (started_wait)
            arp_waiting = false;
        if (done)
            eth_mac_copy(out_mac, arp_waiting_mac);
        spinlock_release_irqrestore(&arp_lock, flags);
        if (done)
            return true;
    }

    // Final table check before giving up.
    if (arp_lookup(ip, out_mac))
        return true;

    DEBUG_WARN("arp: resolution timeout for %d.%d.%d.%d", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 24) & 0xFF);
    return false;
}
