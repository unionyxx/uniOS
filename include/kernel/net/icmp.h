#pragma once
#include <stdint.h>

// ICMP types
#define ICMP_TYPE_ECHO_REPLY 0
#define ICMP_TYPE_ECHO_REQUEST 8

// ICMP Header
struct IcmpHeader
{
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

#define ICMP_HEADER_SIZE 8

// ICMP functions
void icmp_init();
void icmp_receive(const void *data, uint16_t length, uint32_t src_ip);

// Ping ring backing SYS_PING. The ring holds up to 8 outstanding echo
// probes matched by (identifier, sequence); sys_ping claims a slot, sends
// the echo, and polls icmp_ping_result while pumping net_poll().
bool icmp_ping_claim(uint16_t *id, uint16_t *seq);                  // false: ring full
bool icmp_ping_result(uint16_t id, uint16_t seq, uint32_t *rtt_ms); // true: REPLIED
void icmp_ping_release(uint16_t id, uint16_t seq);                  // slot back to UNUSED
uint32_t icmp_rtt_ms(uint64_t sent_ticks, uint64_t now_ticks);

// Extended syscall implementation (user pointer validated inside):
// one blocking echo probe; 0 + *rtt_ms, or -errno.
int64_t sys_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms);

// Kernel-context variant (rtt_ms is a kernel pointer): the same probe flow
// used by the debug net self-test.
int64_t icmp_ping_probe(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms);
