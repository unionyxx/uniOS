#pragma once

#include <stdint.h>

// Pure congestion-control policy (RFC 5681-style slow start, congestion
// avoidance, fast retransmit and fast recovery) plus Jacobson/Karels RTT
// estimation. Kept free of socket state so ktest can exercise every
// transition directly; the transport owns sequence numbers and segments.
namespace tcpcc {

constexpr uint32_t MSS = 1460u;
constexpr uint32_t INITIAL_CWND_SEGMENTS = 2u;
constexpr uint32_t INITIAL_SSTHRESH = 65535u;
constexpr uint32_t MIN_SSTHRESH_SEGMENTS = 2u;
constexpr uint32_t DUP_ACK_FAST_RETRANSMIT = 3u;
constexpr uint32_t INITIAL_RTO_MS = 500u;
constexpr uint32_t MIN_RTO_MS = 200u;
constexpr uint32_t MAX_RTO_MS = 10000u;

struct State
{
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t dup_acks;
    bool in_fast_recovery;
};

constexpr State initial_state()
{
    return {INITIAL_CWND_SEGMENTS * MSS, INITIAL_SSTHRESH, 0u, false};
}

// Outstanding-data cap is the tightest of congestion window, the peer's
// advertised receive window, and the transport's own send buffer.
constexpr uint32_t flight_limit(const State &s, uint32_t peer_window, uint32_t send_buffer_capacity)
{
    uint32_t limit = s.cwnd;
    if (peer_window < limit)
        limit = peer_window;
    if (send_buffer_capacity < limit)
        limit = send_buffer_capacity;
    return limit;
}

constexpr uint32_t loss_reduction(uint32_t flight)
{
    const uint32_t floor = MIN_SSTHRESH_SEGMENTS * MSS;
    const uint32_t half = flight / 2u;
    return half > floor ? half : floor;
}

// A cumulative ACK advanced send_una (forward progress).
constexpr void on_ack(State &s)
{
    s.dup_acks = 0u;
    if (s.in_fast_recovery) {
        s.in_fast_recovery = false;
        s.cwnd = s.ssthresh;
        return;
    }
    if (s.cwnd < s.ssthresh) {
        s.cwnd += MSS; // slow start: +1 segment per ACK
        return;
    }
    // Congestion avoidance: +MSS per RTT, spread across the window.
    // cwnd >= MSS always holds, so the division never degenerates.
    s.cwnd += (MSS * MSS) / s.cwnd;
}

// A duplicate ACK arrived (cumulative ACK unchanged while data is in flight).
// Returns true exactly when the caller must fast-retransmit the oldest
// unacked segment.
constexpr bool on_dup_ack(State &s)
{
    if (s.in_fast_recovery) {
        s.cwnd += MSS; // inflate while the peer keeps dup-acking
        return false;
    }
    s.dup_acks++;
    if (s.dup_acks < DUP_ACK_FAST_RETRANSMIT)
        return false;
    s.in_fast_recovery = true;
    return true;
}

// Entered fast recovery: shrink the slow-start threshold based on the flight
// observed at loss detection and deflate the window.
constexpr void on_fast_retransmit(State &s, uint32_t flight)
{
    s.ssthresh = loss_reduction(flight);
    s.cwnd = s.ssthresh + DUP_ACK_FAST_RETRANSMIT * MSS;
}

// Retransmission timer expired: collapse to one segment and back off.
constexpr void on_rto_timeout(State &s, uint32_t flight)
{
    s.ssthresh = loss_reduction(flight);
    s.cwnd = MSS;
    s.dup_acks = 0u;
    s.in_fast_recovery = false;
}

struct RttEstimate
{
    int64_t srtt_ms;
    int64_t rttvar_ms;
    uint32_t rto_ms;
    bool has_sample;
};

constexpr RttEstimate initial_rtt()
{
    return {0, 0, INITIAL_RTO_MS, false};
}

// Jacobson/Karels update. Callers must apply Karn's rule and only feed
// samples taken from segments that were never retransmitted.
constexpr void rtt_sample(RttEstimate &r, uint64_t sample_ms)
{
    if (sample_ms == 0)
        sample_ms = 1;
    if (!r.has_sample) {
        r.srtt_ms = static_cast<int64_t>(sample_ms);
        r.rttvar_ms = static_cast<int64_t>(sample_ms) / 2;
        r.has_sample = true;
    } else {
        int64_t delta = static_cast<int64_t>(sample_ms) - r.srtt_ms;
        if (delta < 0)
            delta = -delta;
        r.rttvar_ms = (3 * r.rttvar_ms + delta) / 4;                       // beta = 1/4
        r.srtt_ms = (7 * r.srtt_ms + static_cast<int64_t>(sample_ms)) / 8; // alpha = 1/8
    }
    int64_t rto = r.srtt_ms + 4 * r.rttvar_ms;
    if (rto < static_cast<int64_t>(MIN_RTO_MS))
        rto = MIN_RTO_MS;
    if (rto > static_cast<int64_t>(MAX_RTO_MS))
        rto = MAX_RTO_MS;
    r.rto_ms = static_cast<uint32_t>(rto);
}

// Karn: back off exponentially on timeout without learning from the sample.
constexpr void rto_backoff(RttEstimate &r)
{
    const uint64_t doubled = static_cast<uint64_t>(r.rto_ms) * 2u;
    r.rto_ms = doubled > MAX_RTO_MS ? MAX_RTO_MS : static_cast<uint32_t>(doubled);
}

} // namespace tcpcc
