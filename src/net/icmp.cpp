#include <kernel/debug.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/icmp.h>
#include <kernel/net/ipv4.h>
#include <kernel/net/net.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>
#include <kernel/user_ptr.h>
#include <uapi/syscalls_ext.h>

// Outstanding echo probes. Entries are matched by (identifier, sequence);
// state zero (ktest context: ring never initialized) reads as UNUSED.
enum PingEntryState
{
    PING_UNUSED = 0,
    PING_IN_FLIGHT,
    PING_REPLIED,
};

struct PingEntry
{
    uint16_t id;
    uint16_t seq;
    uint32_t dst_ip;
    uint64_t sent_ticks;
    uint32_t rtt_ms;
    PingEntryState state;
};

#define PING_RING_SIZE 8

static PingEntry g_ping_ring[PING_RING_SIZE];
static Spinlock g_ping_ring_lock = SPINLOCK_INIT;
static uint16_t g_ping_next_id = 0x1234; // arbitrary seed; wraps

void icmp_init()
{
    g_ping_next_id = 0x1234;
    uint64_t flags = spinlock_acquire_irqsave(&g_ping_ring_lock);
    for (auto &e : g_ping_ring)
        e.state = PING_UNUSED;
    spinlock_release_irqrestore(&g_ping_ring_lock, flags);
    DEBUG_INFO("icmp: layer initialized");
}

uint32_t icmp_rtt_ms(uint64_t sent_ticks, uint64_t now_ticks)
{
    const uint64_t freq = timer_get_frequency();
    if (freq == 0 || now_ticks < sent_ticks)
        return 0;
    return (uint32_t)((now_ticks - sent_ticks) * 1000 / freq);
}

bool icmp_ping_claim(uint16_t *id, uint16_t *seq)
{
    if (!id || !seq)
        return false;
    uint64_t flags = spinlock_acquire_irqsave(&g_ping_ring_lock);
    for (int i = 0; i < PING_RING_SIZE; i++) {
        if (g_ping_ring[i].state == PING_UNUSED) {
            g_ping_ring[i].id = g_ping_next_id++;
            g_ping_ring[i].seq = (uint16_t)i;
            g_ping_ring[i].sent_ticks = timer_get_ticks();
            g_ping_ring[i].state = PING_IN_FLIGHT;
            *id = g_ping_ring[i].id;
            *seq = g_ping_ring[i].seq;
            spinlock_release_irqrestore(&g_ping_ring_lock, flags);
            return true;
        }
    }
    spinlock_release_irqrestore(&g_ping_ring_lock, flags);
    return false;
}

bool icmp_ping_result(uint16_t id, uint16_t seq, uint32_t *rtt_ms)
{
    uint64_t flags = spinlock_acquire_irqsave(&g_ping_ring_lock);
    for (int i = 0; i < PING_RING_SIZE; i++) {
        PingEntry &e = g_ping_ring[i];
        if (e.state == PING_REPLIED && e.id == id && e.seq == seq) {
            if (rtt_ms)
                *rtt_ms = e.rtt_ms;
            spinlock_release_irqrestore(&g_ping_ring_lock, flags);
            return true;
        }
    }
    spinlock_release_irqrestore(&g_ping_ring_lock, flags);
    return false;
}

void icmp_ping_release(uint16_t id, uint16_t seq)
{
    uint64_t flags = spinlock_acquire_irqsave(&g_ping_ring_lock);
    for (int i = 0; i < PING_RING_SIZE; i++) {
        PingEntry &e = g_ping_ring[i];
        if (e.id == id && e.seq == seq && e.state != PING_UNUSED) {
            e.state = PING_UNUSED;
            break;
        }
    }
    spinlock_release_irqrestore(&g_ping_ring_lock, flags);
}

// Builds and transmits one echo request. Pure send path: no ring state.
static bool icmp_send_echo(uint32_t dst_ip, uint16_t id, uint16_t seq)
{
    if (dst_ip == 0)
        return false;
    uint8_t packet[64];
    IcmpHeader *hdr = (IcmpHeader *)packet;

    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->identifier = htons(id);
    hdr->sequence = htons(seq);

    uint8_t *payload = packet + ICMP_HEADER_SIZE;
    for (int i = 0; i < 56; i++)
        payload[i] = (uint8_t)i;

    hdr->checksum = ipv4_checksum(packet, ICMP_HEADER_SIZE + 56);
    return ipv4_send(dst_ip, IP_PROTO_ICMP, packet, ICMP_HEADER_SIZE + 56);
}

void icmp_receive(const void *data, uint16_t length, uint32_t src_ip)
{
    if (!data || length < ICMP_HEADER_SIZE)
        return;

    const IcmpHeader *hdr = (const IcmpHeader *)data;

    // Validate the checksum before acting on the header: corrupt frames
    // otherwise drive echo replies with garbage RTTs (IPv4/UDP/TCP all
    // validate on receive; ICMP must too).
    if (ipv4_checksum(data, length) != 0) {
        DEBUG_WARN("icmp: bad checksum");
        return;
    }

    switch (hdr->type) {
        case ICMP_TYPE_ECHO_REQUEST: {
            // Reply to ping. Limit to the IPv4 payload budget so checksum and
            // transmit lengths never exceed the local reply buffer.
            uint8_t reply[1480];
            uint16_t reply_payload_len = length - ICMP_HEADER_SIZE;
            if (reply_payload_len > sizeof(reply) - ICMP_HEADER_SIZE)
                reply_payload_len = sizeof(reply) - ICMP_HEADER_SIZE;
            IcmpHeader *reply_hdr = (IcmpHeader *)reply;

            reply_hdr->type = ICMP_TYPE_ECHO_REPLY;
            reply_hdr->code = 0;
            reply_hdr->checksum = 0;
            reply_hdr->identifier = hdr->identifier;
            reply_hdr->sequence = hdr->sequence;

            const uint8_t *payload = (const uint8_t *)data + ICMP_HEADER_SIZE;
            for (uint16_t i = 0; i < reply_payload_len; i++)
                reply[ICMP_HEADER_SIZE + i] = payload[i];

            reply_hdr->checksum = ipv4_checksum(reply, ICMP_HEADER_SIZE + reply_payload_len);

            ipv4_send(src_ip, IP_PROTO_ICMP, reply, ICMP_HEADER_SIZE + reply_payload_len);
            break;
        }

        case ICMP_TYPE_ECHO_REPLY: {
            // Match an outstanding probe. The entry stays IN_FLIGHT through
            // sys_ping's release/re-acquire wait loop; only the match under
            // the ring lock transitions it.
            const uint16_t id = ntohs(hdr->identifier);
            const uint16_t seq = ntohs(hdr->sequence);
            const uint64_t now = timer_get_ticks();
            uint64_t flags = spinlock_acquire_irqsave(&g_ping_ring_lock);
            for (int i = 0; i < PING_RING_SIZE; i++) {
                PingEntry &e = g_ping_ring[i];
                if (e.state == PING_IN_FLIGHT && e.id == id && e.seq == seq) {
                    e.rtt_ms = icmp_rtt_ms(e.sent_ticks, now);
                    e.state = PING_REPLIED;
                    DEBUG_INFO("icmp: echo reply from %d.%d.%d.%d seq=%d rtt=%ums", src_ip & 0xFF, (src_ip >> 8) & 0xFF,
                               (src_ip >> 16) & 0xFF, (src_ip >> 24) & 0xFF, seq, e.rtt_ms);
                    break;
                }
            }
            spinlock_release_irqrestore(&g_ping_ring_lock, flags);
            break;
        }
    }
}

int64_t sys_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms)
{
    if (!validate_user_ptr(rtt_ms, sizeof(uint32_t), true))
        return -14; // -EFAULT
    if (ip == 0 || net_get_nic() == NET_NIC_NONE || !net_link_up())
        return -19; // -ENODEV

    uint16_t id = 0, seq = 0;
    if (!icmp_ping_claim(&id, &seq))
        return -16; // -EBUSY: ring full

    if (!icmp_send_echo(ip, id, seq)) {
        icmp_ping_release(id, seq);
        return -19; // -ENODEV: link-level send failed
    }

    // tcp_connect-shaped wait: no ring lock held across polls/yields.
    const uint64_t start = timer_get_ticks();
    const uint64_t freq = timer_get_frequency();
    const uint64_t timeout_ticks = (freq != 0) ? ((uint64_t)timeout_ms * freq / 1000) : 0;
    for (;;) {
        uint32_t rtt = 0;
        if (icmp_ping_result(id, seq, &rtt)) {
            icmp_ping_release(id, seq);
            KSTAC();
            const bool ok = safe_copy_to_user(rtt_ms, &rtt, sizeof(rtt));
            KCLAC();
            return ok ? 0 : -14; // -EFAULT
        }
        if (timer_get_ticks() - start >= timeout_ticks)
            break;
        net_poll();
        scheduler_yield();
    }

    icmp_ping_release(id, seq);
    return -110; // -ETIMEDOUT
}
