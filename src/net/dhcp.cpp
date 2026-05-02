#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/ipv4.h>
#include <kernel/net/net.h>
#include <kernel/net/udp.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/time/timer.h>

// DHCP state
static uint32_t dhcp_xid = 0;
static uint32_t dhcp_server_ip = 0;
static uint32_t dhcp_offered_ip = 0;
static uint32_t dhcp_subnet_mask = 0;
static uint32_t dhcp_gateway = 0;
static uint32_t dhcp_dns = 0;
static bool dhcp_got_offer = false;
static bool dhcp_got_ack = false;

void dhcp_init()
{
    dhcp_xid = timer_get_ticks() & 0xFFFFFFFF;
    dhcp_server_ip = 0;
    dhcp_offered_ip = 0;
    dhcp_subnet_mask = 0;
    dhcp_gateway = 0;
    dhcp_dns = 0;
    dhcp_got_offer = false;
    dhcp_got_ack = false;
}

static bool dhcp_put_option(uint8_t *opt, int *idx, int opt_capacity, uint8_t code, const uint8_t *data, uint8_t len)
{
    if (!opt || !idx || *idx < 0 || !data)
        return false;
    if (*idx + 2 + len > opt_capacity)
        return false;
    opt[(*idx)++] = code;
    opt[(*idx)++] = len;
    for (uint8_t i = 0; i < len; i++)
        opt[(*idx)++] = data[i];
    return true;
}

static bool dhcp_put_u32_le_wire(uint8_t *opt, int *idx, int opt_capacity, uint8_t code, uint32_t value)
{
    uint8_t bytes[4] = {(uint8_t)(value & 0xFF), (uint8_t)((value >> 8) & 0xFF),
                        (uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 24) & 0xFF)};
    return dhcp_put_option(opt, idx, opt_capacity, code, bytes, 4);
}

// Build DHCP packet
static uint16_t dhcp_build_packet(DhcpPacket *pkt, uint8_t msg_type)
{
    if (!pkt)
        return 0;

    // Clear packet
    uint8_t *p = (uint8_t *)pkt;
    for (int i = 0; i < (int)sizeof(DhcpPacket); i++)
        p[i] = 0;

    pkt->op = 1;    // BOOTREQUEST
    pkt->htype = 1; // Ethernet
    pkt->hlen = 6;
    pkt->hops = 0;
    pkt->xid = htonl(dhcp_xid);
    pkt->secs = 0;
    pkt->flags = htons(0x8000); // Broadcast flag

    // Client hardware address
    net_get_mac(pkt->chaddr);

    // Magic cookie
    pkt->magic = htonl(DHCP_MAGIC_COOKIE);

    // Options
    uint8_t *opt = pkt->options;
    int idx = 0;
    const int opt_capacity = (int)sizeof(pkt->options);

    uint8_t msg_data[1] = {msg_type};
    if (!dhcp_put_option(opt, &idx, opt_capacity, DHCP_OPT_MSG_TYPE, msg_data, 1))
        return 0;

    if (msg_type == DHCP_REQUEST) {
        if (!dhcp_put_u32_le_wire(opt, &idx, opt_capacity, DHCP_OPT_REQUESTED_IP, dhcp_offered_ip))
            return 0;
        if (dhcp_server_ip != 0 &&
            !dhcp_put_u32_le_wire(opt, &idx, opt_capacity, DHCP_OPT_SERVER_ID, dhcp_server_ip))
            return 0;
    }

    uint8_t params[3] = {DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS};
    if (!dhcp_put_option(opt, &idx, opt_capacity, DHCP_OPT_PARAM_REQ, params, sizeof(params)))
        return 0;

    if (idx >= opt_capacity)
        return 0;
    opt[idx++] = DHCP_OPT_END;

    return (uint16_t)(sizeof(DhcpPacket) - sizeof(pkt->options) + (size_t)idx);
}

// Send DHCP packet via UDP to broadcast
static Spinlock tx_lock = SPINLOCK_INIT;
static uint8_t tx_buffer[1600];

static bool dhcp_send(DhcpPacket *pkt, uint16_t length)
{
    if (!pkt || length == 0 || 20u + 8u + length > ETH_DATA_LEN)
        return false;

    // Build UDP + IP packet manually since we don't have an IP yet
    // Actually, we need to send with src_ip=0 and dst_ip=broadcast

    // Allocate frame on heap to prevent stack overflow
    spinlock_acquire(&tx_lock);
    uint8_t *frame = tx_buffer;

    // Build UDP header
    struct
    {
        uint16_t src_port;
        uint16_t dst_port;
        uint16_t length;
        uint16_t checksum;
    } __attribute__((packed)) udp;

    udp.src_port = htons(DHCP_CLIENT_PORT);
    udp.dst_port = htons(DHCP_SERVER_PORT);
    udp.length = htons(8 + length);
    udp.checksum = 0; // Optional for UDP

    // Build IP header
    struct
    {
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
    ip.protocol = 17; // UDP
    ip.checksum = 0;
    ip.src_ip = 0;
    ip.dst_ip = 0xFFFFFFFF; // Broadcast

    // Calculate IP checksum
    ip.checksum = ipv4_checksum(&ip, 20);

    // Assemble frame
    uint8_t *p = frame;
    for (int i = 0; i < 20; i++)
        p[i] = ((uint8_t *)&ip)[i];
    p += 20;
    for (int i = 0; i < 8; i++)
        p[i] = ((uint8_t *)&udp)[i];
    p += 8;
    for (int i = 0; i < length; i++)
        p[i] = ((uint8_t *)pkt)[i];

    // Send via Ethernet broadcast
    bool result = ethernet_send(ETH_BROADCAST_MAC, ETH_TYPE_IPV4, frame, 20 + 8 + length);
    spinlock_release(&tx_lock);
    return result;
}

// Parse DHCP options
void dhcp_parse_options(const uint8_t *options, uint16_t length)
{
    if (!options)
        return;
    uint16_t i = 0;

    while (i < length) {
        uint8_t opt = options[i++];

        if (opt == DHCP_OPT_PAD)
            continue;
        if (opt == DHCP_OPT_END)
            break;

        if (i >= length)
            break;
        uint8_t len = options[i++];
        if ((uint16_t)(length - i) < len)
            break;

        switch (opt) {
            case DHCP_OPT_SUBNET_MASK:
                if (len >= 4) {
                    dhcp_subnet_mask =
                        options[i] | (options[i + 1] << 8) | (options[i + 2] << 16) | (options[i + 3] << 24);
                }
                break;
            case DHCP_OPT_ROUTER:
                if (len >= 4) {
                    dhcp_gateway = options[i] | (options[i + 1] << 8) | (options[i + 2] << 16) | (options[i + 3] << 24);
                }
                break;
            case DHCP_OPT_DNS:
                if (len >= 4) {
                    dhcp_dns = options[i] | (options[i + 1] << 8) | (options[i + 2] << 16) | (options[i + 3] << 24);
                }
                break;
            case DHCP_OPT_SERVER_ID:
                if (len >= 4) {
                    dhcp_server_ip =
                        options[i] | (options[i + 1] << 8) | (options[i + 2] << 16) | (options[i + 3] << 24);
                }
                break;
        }

        i += len;
    }
}

// Receive DHCP packet
void dhcp_receive(const void *data, uint16_t length, uint32_t src_ip)
{
    (void)src_ip;

    if (!data || length < sizeof(DhcpPacket) - 308) { // Minimum DHCP size
        return;
    }

    const DhcpPacket *pkt = (const DhcpPacket *)data;

    // Verify this is for us
    if (pkt->op != 2)
        return; // Must be BOOTREPLY
    if (ntohl(pkt->xid) != dhcp_xid)
        return; // Transaction ID mismatch
    uint8_t our_mac[6];
    net_get_mac(our_mac);
    for (int i = 0; i < 6; i++) {
        if (pkt->chaddr[i] != our_mac[i])
            return;
    }

    // Check magic cookie
    if (ntohl(pkt->magic) != DHCP_MAGIC_COOKIE)
        return;

    // Find message type
    uint8_t msg_type = 0;
    const uint8_t *opt = pkt->options;
    uint16_t opt_offset = (uint16_t)((const uint8_t *)pkt->options - (const uint8_t *)pkt);
    if (length < opt_offset)
        return;
    uint16_t opt_len = length - opt_offset;

    for (uint16_t i = 0; i < opt_len;) {
        if (opt[i] == DHCP_OPT_PAD) {
            i++;
            continue;
        }
        if (opt[i] == DHCP_OPT_END)
            break;
        if (i + 1 >= opt_len)
            break;

        uint8_t code = opt[i++];
        uint8_t len = opt[i++];
        if ((uint16_t)(opt_len - i) < len)
            break;

        if (code == DHCP_OPT_MSG_TYPE && len >= 1) {
            msg_type = opt[i];
        }

        i += len;
    }

    if (msg_type == DHCP_OFFER) {
        dhcp_offered_ip = pkt->yiaddr;
        dhcp_parse_options(pkt->options, opt_len);
        dhcp_got_offer = true;

    } else if (msg_type == DHCP_ACK) {
        dhcp_offered_ip = pkt->yiaddr;
        dhcp_parse_options(pkt->options, opt_len);
        dhcp_got_ack = true;
    }
}

// Request IP via DHCP (blocking)
bool dhcp_request()
{
    DhcpPacket pkt;

    // Reset state
    dhcp_got_offer = false;
    dhcp_got_ack = false;
    dhcp_server_ip = 0;
    dhcp_offered_ip = 0;
    dhcp_subnet_mask = 0;
    dhcp_gateway = 0;
    dhcp_dns = 0;
    dhcp_xid = timer_get_ticks() & 0xFFFFFFFF;

    // Send DISCOVER
    uint16_t len = dhcp_build_packet(&pkt, DHCP_DISCOVER);
    if (len == 0 || !dhcp_send(&pkt, len)) {
        DEBUG_ERROR("dhcp: failed to send DISCOVER");
        return false;
    }

    // Wait for OFFER (5 second timeout)
    uint64_t start = timer_get_ticks();
    uint64_t timeout = (5000 * timer_get_frequency()) / 1000;

    while (!dhcp_got_offer && (timer_get_ticks() - start) < timeout) {
        net_poll();
        scheduler_yield(); // Yield CPU instead of busy-wait
    }

    if (!dhcp_got_offer) {
        DEBUG_WARN("dhcp: no OFFER received");
        return false;
    }

    // Send REQUEST
    len = dhcp_build_packet(&pkt, DHCP_REQUEST);
    if (len == 0 || !dhcp_send(&pkt, len)) {
        DEBUG_ERROR("dhcp: failed to send REQUEST");
        return false;
    }

    // Wait for ACK
    start = timer_get_ticks();
    while (!dhcp_got_ack && (timer_get_ticks() - start) < timeout) {
        net_poll();
        scheduler_yield(); // Yield CPU instead of busy-wait
    }

    if (!dhcp_got_ack) {
        DEBUG_WARN("dhcp: no ACK received");
        return false;
    }

    // Configure network with received parameters
    net_set_ip(dhcp_offered_ip);
    net_set_netmask(dhcp_subnet_mask);
    net_set_gateway(dhcp_gateway);
    net_set_dns(dhcp_dns);

    return true;
}
