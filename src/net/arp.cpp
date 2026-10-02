#include <kernel/debug.h>
#include <kernel/net/arp.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/net.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>

static ArpEntry arp_table[ARP_TABLE_SIZE];
// Guards the ARP table and the request-rate state.
static Spinlock arp_lock = SPINLOCK_INIT;

// Request rate limiting: one outstanding request per target per second, so
// TX paths calling arp_resolve() from under their locks cannot storm the
// link while a resolution is pending.
static uint32_t arp_last_request_ip = 0;
static uint64_t arp_last_request_ticks = 0;
// Round-robin eviction cursor: never evict a fixed slot (the old "always
// slot 0" could drop the default gateway every time the table filled).
static uint8_t arp_evict_cursor = 0;

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

    // Update in place
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

    // Table full: round-robin eviction
    arp_table[arp_evict_cursor].ip = ip;
    eth_mac_copy(arp_table[arp_evict_cursor].mac, mac);
    arp_table[arp_evict_cursor].valid = true;
    arp_evict_cursor = (arp_evict_cursor + 1) % ARP_TABLE_SIZE;
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

    // Learn sender's MAC (gratuitous learning: replies to our requests and
    // announcements both populate the cache)
    arp_add_entry(arp->sender_ip, arp->sender_mac);

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
    if (arp_lookup(ip, out_mac))
        return true;

    // Miss: emit (rate-limited) one request and report "pending". This must
    // never block or poll: callers reach here from under TX locks where a
    // nested net_poll() would re-enter those same locks and self-deadlock,
    // and where an interrupts-off busy-wait would stall the local timer.
    // Callers treat false as "send failed, retry later".
    bool send = false;
    uint64_t flags = spinlock_acquire_irqsave(&arp_lock);
    const uint64_t now = timer_get_ticks();
    const uint64_t freq = timer_get_frequency() ? timer_get_frequency() : 1000;
    if (arp_last_request_ip != ip || (now - arp_last_request_ticks) >= freq) { // 1 request / second / target
        arp_last_request_ip = ip;
        arp_last_request_ticks = now;
        send = true;
    }
    spinlock_release_irqrestore(&arp_lock, flags);

    if (send)
        arp_send_request(ip);
    return false;
}

bool arp_resolve_blocking(uint32_t ip, uint8_t *out_mac, uint64_t timeout_ms)
{
    if (!out_mac || ip == 0)
        return false;

    if (ip == 0xFFFFFFFF) {
        eth_mac_copy(out_mac, ETH_BROADCAST_MAC);
        return true;
    }

    if (arp_lookup(ip, out_mac))
        return true;

    // Only for callers that hold no network locks: net_poll() here can enter
    // tcp_poll/udp receive paths that take those locks.
    const uint64_t start = timer_get_ticks();
    const uint64_t freq = timer_get_frequency() ? timer_get_frequency() : 1000;
    const uint64_t timeout_ticks = (timeout_ms * freq) / 1000;

    while ((timer_get_ticks() - start) < timeout_ticks) {
        // Emit / refresh the request (rate-limited inside arp_resolve).
        arp_resolve(ip, out_mac);

        net_poll();
        scheduler_yield();

        if (arp_lookup(ip, out_mac))
            return true;
    }

    DEBUG_WARN("arp: resolution timeout for %d.%d.%d.%d", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF,
               (ip >> 24) & 0xFF);
    return false;
}
