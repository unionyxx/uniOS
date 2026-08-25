#include <kernel/ktest.h>
#include <kernel/net/dns.h>
#include <kernel/net/ethernet.h>
#include <libk/kstring.h>

namespace {

constexpr uint16_t TEST_ID = 0x1234;

// Builds a response with one question ("example") and one compressed
// A-record answer carrying 10.0.2.3.
static uint16_t build_valid_response(uint8_t *buf, uint16_t capacity)
{
    kstring::zero_memory(buf, capacity);
    DnsHeader *hdr = reinterpret_cast<DnsHeader *>(buf);
    hdr->id = htons(TEST_ID);
    hdr->flags = htons(DNS_FLAG_QR | DNS_FLAG_RD);
    hdr->qdcount = htons(1);
    hdr->ancount = htons(1);

    uint16_t pos = DNS_HEADER_SIZE;
    const char *label = "example";
    buf[pos++] = 7;
    for (int i = 0; i < 7; i++)
        buf[pos++] = static_cast<uint8_t>(label[i]);
    buf[pos++] = 0;    // name terminator
    buf[pos++] = 0x00; // type A
    buf[pos++] = 0x01;
    buf[pos++] = 0x00; // class IN
    buf[pos++] = 0x01;

    buf[pos++] = 0xC0; // compression pointer to the question name
    buf[pos++] = 0x0C;
    buf[pos++] = 0x00; // type A
    buf[pos++] = 0x01;
    buf[pos++] = 0x00; // class IN
    buf[pos++] = 0x01;
    buf[pos++] = 0x00; // TTL
    buf[pos++] = 0x00;
    buf[pos++] = 0x00;
    buf[pos++] = 0x3C;
    buf[pos++] = 0x00; // RDLENGTH
    buf[pos++] = 0x04;
    buf[pos++] = 10; // 10.0.2.3
    buf[pos++] = 0;
    buf[pos++] = 2;
    buf[pos++] = 3;
    return pos;
}

} // namespace

KTEST(dns_parse_response_accepts_valid_answer)
{
    uint8_t buf[128];
    const uint16_t len = build_valid_response(buf, sizeof(buf));
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, len, TEST_ID), dns_parse_ip("10.0.2.3"));
}

KTEST(dns_parse_response_rejects_header_problems)
{
    uint8_t buf[128];
    const uint16_t len = build_valid_response(buf, sizeof(buf));

    KTEST_EXPECT_EQ(dns_parse_response_id(buf, len, TEST_ID + 1), 0u); // wrong id

    DnsHeader *hdr = reinterpret_cast<DnsHeader *>(buf);
    hdr->flags = htons(DNS_FLAG_RD); // QR cleared: not a response
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, len, TEST_ID), 0u);

    hdr->flags = htons(DNS_FLAG_QR | 3); // SERVFAIL rcode
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, len, TEST_ID), 0u);

    hdr->flags = htons(DNS_FLAG_QR);
    hdr->ancount = 0; // no answers
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, len, TEST_ID), 0u);

    KTEST_EXPECT_EQ(dns_parse_response_id(buf, DNS_HEADER_SIZE - 1, TEST_ID), 0u);
    KTEST_EXPECT_EQ(dns_parse_response_id(nullptr, len, TEST_ID), 0u);
}

KTEST(dns_parse_response_rejects_truncated_bodies)
{
    uint8_t buf[128];
    const uint16_t len = build_valid_response(buf, sizeof(buf));

    // Cut inside the answer RDATA.
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, static_cast<uint16_t>(len - 2), TEST_ID), 0u);
    // Cut inside the answer fixed fields.
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, DNS_HEADER_SIZE + 14, TEST_ID), 0u);
    // Cut inside the question name.
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, DNS_HEADER_SIZE + 3, TEST_ID), 0u);
}

KTEST(dns_parse_response_rejects_hostile_names)
{
    uint8_t buf[128];
    build_valid_response(buf, sizeof(buf));

    // Question label length that runs past the end of the packet.
    DnsHeader *hdr = reinterpret_cast<DnsHeader *>(buf);
    hdr->qdcount = htons(1);
    hdr->ancount = htons(1);
    buf[DNS_HEADER_SIZE] = 100; // label claims 100 bytes
    KTEST_EXPECT_EQ(dns_parse_response_id(buf, DNS_HEADER_SIZE + 16, TEST_ID), 0u);

    // Compression pointer whose second byte is past the end of the packet.
    uint8_t tiny[DNS_HEADER_SIZE + 1];
    kstring::zero_memory(tiny, sizeof(tiny));
    DnsHeader *thdr = reinterpret_cast<DnsHeader *>(tiny);
    thdr->id = htons(TEST_ID);
    thdr->flags = htons(DNS_FLAG_QR);
    thdr->qdcount = htons(1);
    thdr->ancount = htons(1);
    tiny[DNS_HEADER_SIZE] = 0xC0; // pointer with missing second byte
    KTEST_EXPECT_EQ(dns_parse_response_id(tiny, sizeof(tiny), TEST_ID), 0u);
}
