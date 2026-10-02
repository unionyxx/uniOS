#include <kernel/debug.h>
#include <kernel/mm/heap.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/ipv4.h>
#include <kernel/net/net.h>
#include <kernel/net/udp.h>
#include <kernel/sync/spinlock.h>

static UdpSocket sockets[UDP_MAX_SOCKETS];
// Guards the socket table (alloc/bind/close) and the per-socket rx handoff
// (rx_ready + buffer). RX runs serialized inside net_poll(), but syscalls
// (socket/bind/sendto/recvfrom/close) execute on any core.
static Spinlock udp_table_lock = SPINLOCK_INIT;

void udp_init()
{
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        sockets[i].in_use = false;
        sockets[i].bound = false;
        sockets[i].rx_ready = false;
    }
    DEBUG_INFO("udp: layer initialized (%d sockets)", UDP_MAX_SOCKETS);
}

// Pick an unused ephemeral source port so request/response over an unbound
// socket can actually receive its replies (a hardcoded 49152 for every
// unbound sender made replies undeliverable and let two senders collide).
static uint16_t udp_ephemeral_port()
{
    for (uint16_t candidate = 49152; candidate < 65535; candidate++) {
        bool taken = false;
        for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
            if (sockets[i].in_use && sockets[i].bound && sockets[i].port == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken)
            return candidate;
    }
    return 0;
}

// Pseudo-header for checksum
struct UdpPseudoHeader
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t zero;
    uint8_t protocol;
    uint16_t udp_length;
} __attribute__((packed));

// Calculate UDP checksum with pseudo-header
static Spinlock tx_lock = SPINLOCK_INIT;
static uint8_t tx_buffer[1600];
static Spinlock chk_lock = SPINLOCK_INIT;
static uint8_t chk_buffer[1600];

static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *udp_data, uint16_t length)
{
    // Allocate buffer on heap to avoid stack overflow
    uint64_t flags = spinlock_acquire_irqsave(&chk_lock);
    uint8_t *buffer = chk_buffer;

    UdpPseudoHeader *pseudo = reinterpret_cast<UdpPseudoHeader *>(buffer);

    pseudo->src_ip = src_ip;
    pseudo->dst_ip = dst_ip;
    pseudo->zero = 0;
    pseudo->protocol = IP_PROTO_UDP;
    pseudo->udp_length = htons(length);

    // Copy UDP header and data
    const uint8_t *src = (const uint8_t *)udp_data;
    for (uint16_t i = 0; i < length; i++) {
        buffer[sizeof(UdpPseudoHeader) + i] = src[i];
    }

    uint16_t result = ipv4_checksum(buffer, sizeof(UdpPseudoHeader) + length);
    spinlock_release_irqrestore(&chk_lock, flags);
    return result;
}

void udp_receive(const void *data, uint16_t length, uint32_t src_ip, uint32_t dst_ip)
{
    if (!data || length < UDP_HEADER_SIZE) {
        return;
    }

    const UdpHeader *hdr = (const UdpHeader *)data;
    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t udp_len = ntohs(hdr->length);

    if (udp_len < UDP_HEADER_SIZE || udp_len > length) {
        return;
    }

    // Validate the UDP checksum when present. RFC 768 permits a zero value
    // meaning "no checksum"; any non-zero value must match.
    if (hdr->checksum != 0 && udp_checksum(src_ip, dst_ip, data, udp_len) != 0) {
        DEBUG_WARN("udp: bad checksum");
        return;
    }

    // Find socket bound to this port
    uint64_t tbl_flags = spinlock_acquire_irqsave(&udp_table_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].bound && sockets[i].port == dst_port) {
            // Store in receive buffer
            const uint8_t *payload = (const uint8_t *)data + UDP_HEADER_SIZE;
            uint16_t payload_len = udp_len - UDP_HEADER_SIZE;

            uint16_t stored_len = payload_len;
            if (stored_len > sizeof(sockets[i].rx_buffer))
                stored_len = sizeof(sockets[i].rx_buffer);
            for (uint16_t j = 0; j < stored_len; j++) {
                sockets[i].rx_buffer[j] = payload[j];
            }
            sockets[i].rx_length = stored_len;
            sockets[i].rx_src_ip = src_ip;
            sockets[i].rx_src_port = src_port;
            sockets[i].rx_ready = true;

            spinlock_release_irqrestore(&udp_table_lock, tbl_flags);
            return;
        }
    }
    spinlock_release_irqrestore(&udp_table_lock, tbl_flags);

    // Also handle DHCP (port 68) specially
    if (dst_port == 68) {
        extern void dhcp_receive(const void *data, uint16_t length, uint32_t src_ip);
        const uint8_t *payload = (const uint8_t *)data + UDP_HEADER_SIZE;
        uint16_t payload_len = udp_len - UDP_HEADER_SIZE;
        dhcp_receive(payload, payload_len, src_ip);
    }

    (void)dst_ip; // Unused
}

bool udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const void *data, uint16_t length)
{
    if ((!data && length > 0) || dst_ip == 0 || src_port == 0 || dst_port == 0)
        return false;
    if (length > 1472) { // MTU - IP - UDP headers
        return false;
    }

    // Allocate packet buffer on heap to avoid stack overflow
    uint64_t flags = spinlock_acquire_irqsave(&tx_lock);
    uint8_t *packet = tx_buffer;

    UdpHeader *hdr = reinterpret_cast<UdpHeader *>(packet);

    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dst_port);
    hdr->length = htons(UDP_HEADER_SIZE + length);
    hdr->checksum = 0;

    // Copy payload
    uint8_t *payload = packet + UDP_HEADER_SIZE;
    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < length; i++) {
        payload[i] = src[i];
    }

    // Calculate checksum
    hdr->checksum = udp_checksum(net_get_ip(), dst_ip, packet, UDP_HEADER_SIZE + length);
    if (hdr->checksum == 0) {
        hdr->checksum = 0xFFFF; // 0 means no checksum, use 0xFFFF instead
    }

    bool result = ipv4_send(dst_ip, IP_PROTO_UDP, packet, UDP_HEADER_SIZE + length);
    spinlock_release_irqrestore(&tx_lock, flags);
    return result;
}

int udp_socket()
{
    uint64_t flags = spinlock_acquire_irqsave(&udp_table_lock);
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            sockets[i].in_use = true;
            sockets[i].bound = false; // Created but not bound
            sockets[i].rx_ready = false;
            spinlock_release_irqrestore(&udp_table_lock, flags);
            return i;
        }
    }
    spinlock_release_irqrestore(&udp_table_lock, flags);
    return -1;
}

bool udp_bind(int sock, uint16_t port)
{
    if (sock < 0 || sock >= UDP_MAX_SOCKETS) {
        DEBUG_ERROR("udp: bind failed, invalid socket %d", sock);
        return false;
    }

    uint64_t flags = spinlock_acquire_irqsave(&udp_table_lock);
    if (!sockets[sock].in_use) {
        spinlock_release_irqrestore(&udp_table_lock, flags);
        DEBUG_ERROR("udp: bind failed, invalid socket %d", sock);
        return false;
    }

    // Check if port already in use
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].bound && sockets[i].port == port) {
            spinlock_release_irqrestore(&udp_table_lock, flags);
            DEBUG_ERROR("udp: bind failed, port %d already in use by socket %d", port, i);
            return false;
        }
    }

    sockets[sock].port = port;
    sockets[sock].bound = true;
    sockets[sock].rx_ready = false;
    spinlock_release_irqrestore(&udp_table_lock, flags);
    DEBUG_INFO("udp: socket %d bound to port %d", sock, port);
    return true;
}

bool udp_sendto(int sock, uint32_t dst_ip, uint16_t dst_port, const void *data, uint16_t length)
{
    if (sock < 0 || sock >= UDP_MAX_SOCKETS) {
        return false;
    }

    uint64_t flags = spinlock_acquire_irqsave(&udp_table_lock);
    if (!sockets[sock].in_use) {
        spinlock_release_irqrestore(&udp_table_lock, flags);
        return false;
    }
    uint16_t src_port = sockets[sock].port;
    if (!sockets[sock].bound) {
        // Auto-bind an ephemeral source port so replies are deliverable.
        src_port = udp_ephemeral_port();
        if (src_port == 0) {
            spinlock_release_irqrestore(&udp_table_lock, flags);
            return false;
        }
        sockets[sock].port = src_port;
        sockets[sock].bound = true;
    }
    spinlock_release_irqrestore(&udp_table_lock, flags);

    return udp_send(dst_ip, src_port, dst_port, data, length);
}

int udp_recvfrom(int sock, void *buffer, uint16_t max_len, uint32_t *src_ip, uint16_t *src_port)
{
    if (!buffer && max_len > 0)
        return -1;
    if (sock < 0 || sock >= UDP_MAX_SOCKETS) {
        return -1;
    }

    uint64_t flags = spinlock_acquire_irqsave(&udp_table_lock);
    if (!sockets[sock].in_use || !sockets[sock].bound) {
        spinlock_release_irqrestore(&udp_table_lock, flags);
        return -1;
    }

    if (!sockets[sock].rx_ready) {
        spinlock_release_irqrestore(&udp_table_lock, flags);
        return 0; // No data
    }

    uint16_t len = sockets[sock].rx_length;
    if (len > max_len)
        len = max_len;

    uint8_t *dst = (uint8_t *)buffer;
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = sockets[sock].rx_buffer[i];
    }

    if (src_ip)
        *src_ip = sockets[sock].rx_src_ip;
    if (src_port)
        *src_port = sockets[sock].rx_src_port;

    sockets[sock].rx_ready = false;
    spinlock_release_irqrestore(&udp_table_lock, flags);
    return len;
}

void udp_close(int sock)
{
    if (sock >= 0 && sock < UDP_MAX_SOCKETS) {
        uint64_t flags = spinlock_acquire_irqsave(&udp_table_lock);
        sockets[sock].in_use = false;
        sockets[sock].bound = false;
        sockets[sock].rx_ready = false;
        spinlock_release_irqrestore(&udp_table_lock, flags);
    }
}
