#include <kernel/ktest.h>
#include <kernel/net/ethernet.h>
#include <kernel/net/icmp.h>
#include <kernel/net/ipv4.h>
#include <kernel/time/timer.h>
#include <libk/kstring.h>

namespace {

// Builds an echo-reply packet for (id, seq) with a valid checksum and the
// standard 56-byte ping payload.
static uint16_t build_echo_reply(uint8_t *buf, uint16_t id, uint16_t seq)
{
    kstring::zero_memory(buf, ICMP_HEADER_SIZE + 56);
    IcmpHeader *hdr = reinterpret_cast<IcmpHeader *>(buf);
    hdr->type = ICMP_TYPE_ECHO_REPLY;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->identifier = htons(id);
    hdr->sequence = htons(seq);
    for (int i = 0; i < 56; i++)
        buf[ICMP_HEADER_SIZE + i] = static_cast<uint8_t>(i);
    hdr->checksum = ipv4_checksum(buf, ICMP_HEADER_SIZE + 56);
    return ICMP_HEADER_SIZE + 56;
}

} // namespace

KTEST(icmp_rtt_math_is_exact)
{
    const uint64_t freq = timer_get_frequency();
    if (freq == 0) {
        KTEST_EXPECT(freq != 0); // timer must be up before ktests
        return;
    }
    KTEST_EXPECT_EQ(icmp_rtt_ms(0, freq), 1000u);
    KTEST_EXPECT_EQ(icmp_rtt_ms(freq / 2, freq), 500u);
    KTEST_EXPECT_EQ(icmp_rtt_ms(freq, freq), 0u);
}

KTEST(icmp_reply_completes_matching_entry)
{
    uint16_t id = 0, seq = 0;
    KTEST_EXPECT(icmp_ping_claim(&id, &seq));

    uint8_t packet[ICMP_HEADER_SIZE + 56];
    const uint16_t len = build_echo_reply(packet, id, seq);
    icmp_receive(packet, len, 0x0A000202);

    uint32_t rtt = 0xFFFFFFFFu;
    KTEST_EXPECT(icmp_ping_result(id, seq, &rtt));
    // Claim and inject happen within the same tick window: sub-second.
    KTEST_EXPECT(rtt < 1000);

    icmp_ping_release(id, seq);
    uint32_t rtt_after = 0;
    KTEST_EXPECT(!icmp_ping_result(id, seq, &rtt_after));
}

KTEST(icmp_reply_ignores_mismatched_id)
{
    uint16_t id = 0, seq = 0;
    KTEST_EXPECT(icmp_ping_claim(&id, &seq));

    uint8_t packet[ICMP_HEADER_SIZE + 56];
    const uint16_t len = build_echo_reply(packet, static_cast<uint16_t>(id + 1), seq);
    icmp_receive(packet, len, 0x0A000202);

    uint32_t rtt = 0;
    KTEST_EXPECT(!icmp_ping_result(id, seq, &rtt)); // still in flight

    // The correct reply still completes the entry afterwards.
    const uint16_t len2 = build_echo_reply(packet, id, seq);
    icmp_receive(packet, len2, 0x0A000202);
    KTEST_EXPECT(icmp_ping_result(id, seq, &rtt));

    icmp_ping_release(id, seq);
}

KTEST(icmp_reply_rejects_bad_checksum)
{
    uint16_t id = 0, seq = 0;
    KTEST_EXPECT(icmp_ping_claim(&id, &seq));

    uint8_t packet[ICMP_HEADER_SIZE + 56];
    const uint16_t len = build_echo_reply(packet, id, seq);
    packet[ICMP_HEADER_SIZE] ^= 0xFF; // corrupt payload after checksumming
    icmp_receive(packet, len, 0x0A000202);

    uint32_t rtt = 0;
    KTEST_EXPECT(!icmp_ping_result(id, seq, &rtt)); // still in flight

    icmp_ping_release(id, seq);
}

KTEST(icmp_ring_full_rejects_ninth_claim)
{
    uint16_t ids[8], seqs[8];
    for (int i = 0; i < 8; i++)
        KTEST_EXPECT(icmp_ping_claim(&ids[i], &seqs[i]));

    uint16_t extra_id = 0, extra_seq = 0;
    KTEST_EXPECT(!icmp_ping_claim(&extra_id, &extra_seq));

    for (int i = 0; i < 8; i++)
        icmp_ping_release(ids[i], seqs[i]);

    // All slots released: a claim succeeds again.
    KTEST_EXPECT(icmp_ping_claim(&extra_id, &extra_seq));
    icmp_ping_release(extra_id, extra_seq);
}
