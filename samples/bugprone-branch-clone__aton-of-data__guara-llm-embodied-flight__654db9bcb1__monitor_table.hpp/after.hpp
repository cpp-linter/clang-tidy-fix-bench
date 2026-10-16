// SPDX-License-Identifier: Apache-2.0
//
// MonitorTable: aggregation of MonitorVerdict samples into the signals M(k), select(k) and the
// monitor share of V(k) (SPEC §3.1; ADR 0002 rules 2-3).
//
// The table is deliberately fail-closed (review 2026-09-11 H-4, H-6): the monitor channel protects
// the vehicle against the Complex Function, so an absent, stale, incomplete, malformed or
// unrepresentable verdict is an invalid input, never "no violation". Fixed capacity, no dynamic
// allocation, bounded time per call.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "guara_rta/types.hpp"

namespace guara_rta
{

inline constexpr std::size_t kMaxMonitors = 16;
inline constexpr std::size_t kMonitorIdCapacity = 47;  // excluding the terminator

// Encodings of guara_msgs/MonitorVerdict; duplicated here to keep the table ROS-independent and
// unit-testable. The static assertions live in arbiter_node.cpp.
namespace monitor
{
constexpr std::uint8_t kClassLog = 0;
constexpr std::uint8_t kClassSwitch = 1;
constexpr std::uint8_t kClassMax = kClassSwitch;
constexpr std::uint8_t kActionHold = 0;
constexpr std::uint8_t kActionRtl = 1;
constexpr std::uint8_t kActionLand = 2;
constexpr std::uint8_t kActionMax = kActionLand;
}  // namespace monitor

struct MonitorVerdictSample
{
  const char * id{nullptr};
  std::uint8_t monitor_class{monitor::kClassLog};
  std::uint8_t action{monitor::kActionHold};
  bool violated{false};
  bool inputs_complete{false};
};

enum class MonitorAccept : std::uint8_t
{
  kAccepted = 0,
  kInvalidField = 1,  // unusable identifier, class or action
  kTableFull = 2,     // no slot left for a new monitor
};

struct MonitorEvaluation
{
  bool violation{false};                     // M(k)
  Recovery action{Recovery::kHold};          // select(k) contribution
  bool invalid{false};                       // contributes to V(k)
  const char * first_violating_id{nullptr};
  const char * first_invalid_id{nullptr};
};

class MonitorTable
{
public:
  void configure(double max_age_s) noexcept {max_age_s_ = max_age_s;}

  // Declares a monitor that must be present in flight. Returns false if the table is full.
  bool expect(const char * id) noexcept
  {
    Slot * slot = slotFor(id, /*create=*/true);
    if (slot == nullptr) {
      return false;
    }
    slot->expected = true;
    return true;
  }

  MonitorAccept observe(const MonitorVerdictSample & sample, double t_recv_s) noexcept
  {
    if (sample.id == nullptr || !fits(sample.id)) {
      malformed_ = true;
      return MonitorAccept::kInvalidField;
    }
    Slot * slot = slotFor(sample.id, /*create=*/true);
    if (slot == nullptr) {
      overflow_ = true;
      return MonitorAccept::kTableFull;
    }
    slot->t_recv_s = t_recv_s;
    if (sample.monitor_class > monitor::kClassMax || sample.action > monitor::kActionMax) {
      // A publisher outside the message contract is not trusted for any of its fields.
      slot->malformed = true;
      slot->violated = false;
      slot->inputs_complete = false;
      return MonitorAccept::kInvalidField;
    }
    slot->malformed = false;
    slot->monitor_class = sample.monitor_class;
    slot->action = static_cast<Recovery>(sample.action + 1U);  // ACTION_HOLD=0 -> Recovery::kHold=1
    slot->violated = sample.violated;
    slot->inputs_complete = sample.inputs_complete;
    return MonitorAccept::kAccepted;
  }

  MonitorEvaluation evaluate(double t_s) const noexcept
  {
    MonitorEvaluation out;
    out.invalid = overflow_ || malformed_;
    for (const auto & s : slots_) {
      if (!s.used) {
        continue;
      }
      const bool seen = s.t_recv_s > -std::numeric_limits<double>::infinity();
      const bool stale = !seen || t_s - s.t_recv_s > max_age_s_;
      if ((s.expected && (stale || !s.inputs_complete)) || s.malformed) {
        out.invalid = true;
        if (out.first_invalid_id == nullptr) {
          out.first_invalid_id = s.id.data();
        }
      }
      if (!stale && !s.malformed && s.violated && s.monitor_class == monitor::kClassSwitch) {
        if (!out.violation) {
          out.first_violating_id = s.id.data();
          out.action = s.action;
        } else {
          out.action = maxRank(out.action, s.action);
        }
        out.violation = true;
      }
    }
    return out;
  }

  std::size_t size() const noexcept
  {
    std::size_t n = 0;
    for (const auto & s : slots_) {
      n += s.used ? 1U : 0U;
    }
    return n;
  }

private:
  struct Slot
  {
    std::array<char, kMonitorIdCapacity + 1U> id{};
    bool used{false};
    bool expected{false};
    bool malformed{false};
    bool violated{false};
    bool inputs_complete{false};
    std::uint8_t monitor_class{monitor::kClassLog};
    Recovery action{Recovery::kHold};
    double t_recv_s{-std::numeric_limits<double>::infinity()};
  };

  static bool fits(const char * id) noexcept
  {
    if (id[0] == '\0') {
      return false;
    }
    for (std::size_t i = 0; i <= kMonitorIdCapacity; ++i) {
      if (id[i] == '\0') {
        return true;
      }
    }
    return false;
  }

  Slot * slotFor(const char * id, bool create) noexcept
  {
    if (id == nullptr || !fits(id)) {
      return nullptr;
    }
    for (auto & s : slots_) {
      if (s.used && std::strncmp(s.id.data(), id, s.id.size()) == 0) {
        return &s;
      }
    }
    if (!create) {
      return nullptr;
    }
    for (auto & s : slots_) {
      if (!s.used) {
        s.used = true;
        s.id.fill('\0');
        for (std::size_t i = 0; i < kMonitorIdCapacity && id[i] != '\0'; ++i) {
          s.id[i] = id[i];
        }
        return &s;
      }
    }
    return nullptr;
  }

  std::array<Slot, kMaxMonitors> slots_{};
  double max_age_s_{0.5};
  bool overflow_{false};
  bool malformed_{false};
};

}  // namespace guara_rta
