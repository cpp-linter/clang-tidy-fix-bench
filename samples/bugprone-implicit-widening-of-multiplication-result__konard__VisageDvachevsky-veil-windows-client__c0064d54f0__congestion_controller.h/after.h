#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace veil::mux {

// Congestion control state.
enum class CongestionState : std::uint8_t {
  kSlowStart = 0,          // Exponential increase phase
  kCongestionAvoidance = 1, // Linear increase phase (AIMD)
  kFastRecovery = 2         // After fast retransmit, before full recovery
};

// Configuration for congestion control behavior.
struct CongestionConfig {
  // Initial congestion window in bytes.
  std::size_t initial_cwnd{static_cast<std::size_t>(10) * 1400};  // 10 MSS (RFC 6928)

  // Minimum congestion window in bytes (1 MSS).
  std::size_t min_cwnd{1400};

  // Maximum congestion window in bytes.
  std::size_t max_cwnd{static_cast<std::size_t>(64) * 1024 * 1024};  // 64 MB

  // Initial slow start threshold (large value = always start in slow start).
  std::size_t initial_ssthresh{static_cast<std::size_t>(64) * 1024 * 1024};

  // Maximum Segment Size (MSS) - typical MTU minus IP/UDP headers.
  std::size_t mss{1400};

  // Duplicate ACK threshold for fast retransmit (RFC 5681).
  std::uint32_t fast_retransmit_threshold{3};

  // Enable pacing to spread packets over time.
  bool enable_pacing{true};

  // Pacing gain (pacing_rate = cwnd / srtt * pacing_gain).
  double pacing_gain{1.25};

  // Minimum pacing interval in microseconds.
  std::chrono::microseconds min_pacing_interval{100};

  // Maximum pacing burst (packets sent without delay).
  std::size_t max_pacing_burst{10};

  // Alpha for AIMD decrease (cwnd *= alpha on loss).
  double aimd_alpha{0.5};
};

// Statistics for congestion control observability.
struct CongestionStats {
  // Window tracking.
  std::uint64_t cwnd_increases{0};
  std::uint64_t cwnd_decreases{0};
  std::uint64_t slow_start_exits{0};

  // Loss detection.
  std::uint64_t fast_retransmits{0};
  std::uint64_t timeout_retransmits{0};
  std::uint64_t duplicate_acks{0};

  // State transitions.
  std::uint64_t state_transitions{0};

  // Pacing statistics.
  std::uint64_t pacing_delays{0};
  std::uint64_t pacing_tokens_granted{0};

  // Peak values.
  std::size_t peak_cwnd{0};
  std::size_t peak_bytes_in_flight{0};
};

/**
 * Implements TCP-like congestion control (AIMD) for reliable UDP transport.
 *
 * Features:
 * - Slow start: Exponential growth until ssthresh or loss
 * - Congestion avoidance: Linear growth (AIMD)
 * - Fast retransmit: Retransmit on 3 duplicate ACKs
 * - Fast recovery: Avoid slow start after fast retransmit
 * - Pacing: Spread packets to avoid bursts
 *
 * Thread Safety:
 *   This class is NOT thread-safe. All methods must be called from a single
 *   thread (typically the event loop thread). The controller contains internal
 *   state that is not protected by locks.
 *
 * References:
 *   - RFC 5681: TCP Congestion Control
 *   - RFC 6928: Increasing TCP's Initial Window
 */
class CongestionController {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Duration = std::chrono::microseconds;

  explicit CongestionController(CongestionConfig config = {},
                                 std::function<TimePoint()> now_fn = Clock::now);

  // ========== Congestion Window Management ==========

  // Called when an ACK is received for acked_bytes of data.
  // Updates congestion window based on current state.
  void on_ack(std::size_t acked_bytes);

  // Called when a duplicate ACK is received.
  // Returns true if fast retransmit should be triggered.
  bool on_duplicate_ack();

  // Called when packet loss is detected via timeout.
  void on_timeout_loss();

  // Called when packet loss is detected via fast retransmit.
  void on_fast_retransmit_loss();

  // Called when exiting fast recovery.
  void on_recovery_complete();

  // ========== Send Permission ==========

  // Check if we can send more data given current bytes in flight.
  bool can_send(std::size_t bytes_in_flight) const;

  // Get the number of bytes that can be sent now.
  std::size_t sendable_bytes(std::size_t bytes_in_flight) const;

  // ========== Pacing ==========

  // Check if a packet can be sent now according to pacing.
  // Returns true if the packet can be sent, false if it should be delayed.
  bool check_pacing();

  // Get the time to wait before sending the next packet.
  // Returns nullopt if a packet can be sent immediately.
  std::optional<std::chrono::microseconds> time_until_next_send() const;

  // Update pacing rate based on current RTT.
  void update_pacing_rate(std::chrono::milliseconds rtt);

  // ========== State Queries ==========

  // Current congestion window in bytes.
  std::size_t cwnd() const { return cwnd_; }

  // Current slow start threshold.
  std::size_t ssthresh() const { return ssthresh_; }

  // Current state.
  CongestionState state() const { return state_; }

  // Current pacing rate in bytes per second.
  std::size_t pacing_rate() const { return pacing_rate_; }

  // Get statistics.
  const CongestionStats& stats() const { return stats_; }

  // Reset controller to initial state.
  void reset();

  // ========== RTT Integration ==========

  // Set the current smoothed RTT (used for pacing calculations).
  void set_srtt(std::chrono::milliseconds srtt);

 private:
  // Internal state transitions.
  void enter_slow_start();
  void enter_congestion_avoidance();
  void enter_fast_recovery();

  // Calculate pacing interval for given rate.
  std::chrono::microseconds calculate_pacing_interval() const;

  CongestionConfig config_;
  std::function<TimePoint()> now_fn_;

  // Congestion window state.
  std::size_t cwnd_;
  std::size_t ssthresh_;
  CongestionState state_{CongestionState::kSlowStart};

  // Duplicate ACK tracking.
  std::uint32_t dup_ack_count_{0};

  // Pacing state.
  std::size_t pacing_rate_{0};
  TimePoint last_send_time_;
  std::size_t pacing_burst_remaining_{0};
  std::chrono::milliseconds srtt_{100};

  // Statistics.
  CongestionStats stats_;
};

}  // namespace veil::mux
