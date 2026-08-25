#include <kernel/ktest.h>
#include <kernel/net/tcp_congestion.h>

using namespace tcpcc;

KTEST(tcpcc_slow_start_then_congestion_avoidance)
{
    State s = initial_state();
    KTEST_EXPECT_EQ(s.cwnd, INITIAL_CWND_SEGMENTS * MSS);
    KTEST_EXPECT_EQ(s.ssthresh, INITIAL_SSTHRESH);

    // Slow start: each ACK adds one full segment while below ssthresh.
    s.ssthresh = 8 * MSS;
    on_ack(s);
    KTEST_EXPECT_EQ(s.cwnd, 3 * MSS);
    on_ack(s);
    KTEST_EXPECT_EQ(s.cwnd, 4 * MSS);

    // Reach ssthresh, then congestion avoidance growth must be sub-linear.
    while (s.cwnd < s.ssthresh)
        on_ack(s);
    KTEST_EXPECT(s.cwnd >= s.ssthresh);
    const uint32_t at_threshold = s.cwnd;
    on_ack(s);
    KTEST_EXPECT(s.cwnd > at_threshold);
    KTEST_EXPECT(s.cwnd < at_threshold + MSS); // +MSS*MSS/cwnd, not +MSS
}

KTEST(tcpcc_dup_acks_trigger_fast_retransmit)
{
    State s = initial_state();
    s.cwnd = 8 * MSS;

    KTEST_EXPECT(!on_dup_ack(s));
    KTEST_EXPECT(!on_dup_ack(s));
    KTEST_EXPECT_EQ(s.dup_acks, 2u);
    KTEST_EXPECT(on_dup_ack(s)); // 3rd dup ACK
    KTEST_EXPECT(s.in_fast_recovery);

    const uint32_t flight = 8 * MSS;
    on_fast_retransmit(s, flight);
    KTEST_EXPECT_EQ(s.ssthresh, flight / 2u);
    KTEST_EXPECT_EQ(s.cwnd, s.ssthresh + DUP_ACK_FAST_RETRANSMIT * MSS);

    // Further dup ACKs inflate the window but never re-trigger.
    const uint32_t before = s.cwnd;
    KTEST_EXPECT(!on_dup_ack(s));
    KTEST_EXPECT_EQ(s.cwnd, before + MSS);
}

KTEST(tcpcc_new_ack_exits_fast_recovery)
{
    State s = initial_state();
    s.cwnd = 8 * MSS;
    on_dup_ack(s);
    on_dup_ack(s);
    KTEST_EXPECT(on_dup_ack(s));
    on_fast_retransmit(s, 8 * MSS);
    KTEST_EXPECT(s.in_fast_recovery);

    on_ack(s);
    KTEST_EXPECT(!s.in_fast_recovery);
    KTEST_EXPECT_EQ(s.dup_acks, 0u);
    KTEST_EXPECT_EQ(s.cwnd, s.ssthresh);
}

KTEST(tcpcc_rto_collapses_window)
{
    State s = initial_state();
    s.cwnd = 16 * MSS;
    s.dup_acks = 2;
    s.in_fast_recovery = true;

    on_rto_timeout(s, 16 * MSS);
    KTEST_EXPECT_EQ(s.cwnd, MSS);
    KTEST_EXPECT_EQ(s.ssthresh, 8 * MSS);
    KTEST_EXPECT_EQ(s.dup_acks, 0u);
    KTEST_EXPECT(!s.in_fast_recovery);

    // Tiny flight must not push ssthresh below the floor.
    on_rto_timeout(s, 100);
    KTEST_EXPECT_EQ(s.ssthresh, MIN_SSTHRESH_SEGMENTS * MSS);
}

KTEST(tcpcc_flight_limit_is_tightest_bound)
{
    State s = initial_state();
    s.cwnd = 10 * MSS;
    const uint32_t capacity = 64 * MSS;
    KTEST_EXPECT_EQ(flight_limit(s, 65535, capacity), 10 * MSS);
    KTEST_EXPECT_EQ(flight_limit(s, 4 * MSS, capacity), 4 * MSS);
    KTEST_EXPECT_EQ(flight_limit(s, 65535, 2 * MSS), 2 * MSS);
}

KTEST(tcpcc_rtt_estimate_converges_and_backs_off)
{
    RttEstimate r = initial_rtt();
    KTEST_EXPECT_EQ(r.rto_ms, INITIAL_RTO_MS);
    KTEST_EXPECT(!r.has_sample);

    rtt_sample(r, 100);
    KTEST_EXPECT(r.has_sample);
    KTEST_EXPECT_EQ(r.srtt_ms, 100);
    // RTO = srtt + 4*rttvar = 100 + 200 = 300
    KTEST_EXPECT_EQ(r.rto_ms, 300u);

    // Repeated stable samples tighten the estimate toward the sample.
    for (int i = 0; i < 20; i++)
        rtt_sample(r, 100);
    KTEST_EXPECT(r.rto_ms >= MIN_RTO_MS);
    KTEST_EXPECT(r.rto_ms < 300u);

    // A jittery sample widens the variance; RTO stays within bounds.
    rtt_sample(r, 500);
    KTEST_EXPECT(r.rto_ms > 100u);
    KTEST_EXPECT(r.rto_ms <= MAX_RTO_MS);

    const uint32_t rto_before = r.rto_ms;
    rto_backoff(r);
    KTEST_EXPECT_EQ(r.rto_ms, rto_before * 2u);
    for (int i = 0; i < 20; i++)
        rto_backoff(r);
    KTEST_EXPECT_EQ(r.rto_ms, MAX_RTO_MS);
}

KTEST(tcpcc_rto_floor_on_zero_sample)
{
    RttEstimate r = initial_rtt();
    rtt_sample(r, 0); // clamped to 1ms internally
    KTEST_EXPECT_EQ(r.srtt_ms, 1);
    KTEST_EXPECT_EQ(r.rto_ms, MIN_RTO_MS); // 1 + 4*0 below the floor
}
