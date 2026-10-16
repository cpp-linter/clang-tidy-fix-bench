// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Ruixiang Du
//
// The state-machine engine. States are value types held in a preallocated
// tuple; the transition table is types verified at compile time; dispatch is
// allocation-free, RTTI-free, exception-free, and bounded.
//
// Contract: single-threaded and non-reentrant. A state hook (OnEnter / Update /
// OnExit) must NOT call Dispatch/Update/Start/Stop on its own machine — to
// request a transition from inside a state, set a flag in the context and let
// the next tick handle it (or use a Completion row). Re-entrant calls are
// detected, refused (no state change), and reported via OnReentrancyBlocked —
// never aborting, since dropping a control loop is worse than refusing a call.
#ifndef CTFSM_STATE_MACHINE_HPP_
#define CTFSM_STATE_MACHINE_HPP_

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "ctfsm/observer.hpp"
#include "ctfsm/traits.hpp"
#include "ctfsm/transition_table.hpp"

namespace ctfsm {

template <class Context, class States, class Transitions>
class StateMachine;  // primary template left undefined

template <class Context, class... States, class... Rows>
class StateMachine<Context, StateList<States...>, Table<Rows...>> {
  static_assert(sizeof...(States) > 0,
                "StateList must list at least one state");

  using InitialState = std::tuple_element_t<0, std::tuple<States...>>;

  template <class T>
  static constexpr bool is_state_v = (std::is_same_v<T, States> || ...);
  template <class T>
  static constexpr bool is_from_v =
      std::is_same_v<T, AnyState> || is_state_v<T>;

  // Reachability: a state must be the initial state or some row's target.
  template <class S>
  static constexpr bool reachable_v =
      std::is_same_v<S, InitialState> ||
      (std::is_same_v<S, typename Rows::to> || ...);

  // Ambiguity: how many rows share this row's (From, Event, Guard) key.
  template <class R>
  static constexpr int key_multiplicity =
      ((std::is_same_v<typename R::from, typename Rows::from> &&
        std::is_same_v<typename R::event, typename Rows::event> &&
        std::is_same_v<typename R::guard, typename Rows::guard>)+...);

  static_assert((is_from_v<typename Rows::from> && ...),
                "every Row 'From' must be a state in StateList, or AnyState");
  static_assert((is_state_v<typename Rows::to> && ...),
                "every Row 'To' must be a state in StateList");
  static_assert((reachable_v<States> && ...),
                "unreachable state: every state must be the initial state or "
                "the target ('To') of some transition");
  static_assert(((key_multiplicity<Rows> <= 1) && ...),
                "ambiguous transition table: two rows share the same "
                "(From, Event, Guard) — the second could never fire");

 public:
  explicit StateMachine(FsmObserver* observer = nullptr) noexcept
      : observer_(observer) {}

  // Enter the initial state (the first in StateList). Call once before Update.
  void Start(Context& ctx) noexcept {
    if (in_dispatch_) return ReportReentrancy();
    in_dispatch_ = true;
    current_ = 0;
    started_ = true;
    CallOnEnter(ctx);
    if (observer_ != nullptr) observer_->OnEnter(CurrentName());
    in_dispatch_ = false;
  }

  // Run the current state's OnExit (e.g. ramp torques to zero) and leave the
  // machine stopped. The documented shutdown path — call before teardown so the
  // active state's cleanup actually runs. Idempotent.
  void Stop(Context& ctx) noexcept {
    if (!started_) return;
    if (in_dispatch_) return ReportReentrancy();
    in_dispatch_ = true;
    CallOnExit(ctx);
    if (observer_ != nullptr) observer_->OnExit(CurrentName());
    started_ = false;
    in_dispatch_ = false;
  }

  // One control tick: run the current state's action, then take an auto
  // (Completion) transition if one is enabled. Silent if none applies.
  void Update(Context& ctx) noexcept {
    if (in_dispatch_) return ReportReentrancy();
    in_dispatch_ = true;
    CallUpdate(ctx);
    bool refused = false;
    std::string_view guard{};
    (void)TryDispatch<Completion>(ctx, refused, guard);  // refusals are silent
    in_dispatch_ = false;
  }

  // Process an external event. Returns whether a transition was taken.
  template <class Event>
  bool Dispatch(const Event&, Context& ctx) noexcept {
    if (in_dispatch_) {
      ReportReentrancy();
      return false;
    }
    in_dispatch_ = true;
    bool refused = false;
    std::string_view guard{};
    const bool taken = TryDispatch<Event>(ctx, refused, guard);
    in_dispatch_ = false;
    if (!taken && observer_ != nullptr) {
      if (refused) {
        observer_->OnRefused(CurrentName(), name<Event>(), guard);
      } else {
        observer_->OnIgnored(CurrentName(), name<Event>());
      }
    }
    return taken;
  }

  template <class State>
  bool IsIn() const noexcept {
    return current_ == IndexOf<State>();
  }
  bool started() const noexcept { return started_; }
  std::size_t CurrentId() const noexcept { return current_; }
  std::string_view CurrentName() const noexcept {
    std::string_view n{};
    VisitCurrent([&](const auto& s) { n = name<std::decay_t<decltype(s)>>(); });
    return n;
  }

 private:
  // A re-entrant call is a logic error, but we refuse it (no state change) and
  // report it rather than abort — dropping a control loop is worse than
  // refusing one nested call. Make OnReentrancyBlocked loud (log/alarm/assert)
  // in your app.
  void ReportReentrancy() noexcept {
    if (observer_ != nullptr) observer_->OnReentrancyBlocked(CurrentName());
  }

  template <class T>
  static constexpr std::size_t IndexOf() noexcept {
    const bool m[] = {std::is_same_v<T, States>...};
    for (std::size_t i = 0; i < sizeof...(States); ++i) {
      if (m[i]) return i;
    }
    return sizeof...(States);
  }

  template <std::size_t I = 0, class F>
  void VisitCurrent(F&& f) noexcept {
    if constexpr (I < sizeof...(States)) {
      if (current_ == I) {
        f(std::get<I>(states_));
        return;
      }
      VisitCurrent<I + 1>(std::forward<F>(f));
    }
  }
  template <std::size_t I = 0, class F>
  void VisitCurrent(F&& f) const noexcept {
    if constexpr (I < sizeof...(States)) {
      if (current_ == I) {
        f(std::get<I>(states_));
        return;
      }
      VisitCurrent<I + 1>(std::forward<F>(f));
    }
  }

  void CallOnEnter(Context& ctx) noexcept {
    VisitCurrent([&](auto& s) {
      using S = std::decay_t<decltype(s)>;
      if constexpr (detail::has_on_enter<S, Context>::value) s.OnEnter(ctx);
    });
  }
  void CallOnExit(Context& ctx) noexcept {
    VisitCurrent([&](auto& s) {
      using S = std::decay_t<decltype(s)>;
      if constexpr (detail::has_on_exit<S, Context>::value) s.OnExit(ctx);
    });
  }
  void CallUpdate(Context& ctx) noexcept {
    VisitCurrent([&](auto& s) {
      using S = std::decay_t<decltype(s)>;
      if constexpr (detail::has_update<S, Context>::value) s.Update(ctx);
    });
  }

  template <class To, class Action, class Event>
  void TransitionTo(Context& ctx) noexcept {
    const std::string_view from = CurrentName();
    CallOnExit(ctx);
    if constexpr (!std::is_same_v<Action, NoAction>) Action{}(ctx);
    current_ = IndexOf<To>();
    CallOnEnter(ctx);
    if (observer_ != nullptr) {
      observer_->OnTransition(from, name<Event>(), name<To>());
    }
  }

  // Resolution: try concrete-From (exact) rows first. If an exact row matched
  // the event but its guard refused, STOP — a declared, guard-blocked
  // transition does NOT silently fall through to a wildcard. Only when no exact
  // row matches the (state, event) at all do we consider AnyState rows.
  template <class Event>
  bool TryDispatch(Context& ctx, bool& refused,
                   std::string_view& guard) noexcept {
    refused = false;
    guard = {};
    if (DispatchPass<Event, true>(ctx, refused, guard)) return true;
    if (refused) return false;  // exact match, guard-blocked -> no fall-through
    return DispatchPass<Event, false>(ctx, refused, guard);  // wildcards
  }

  template <class Event, bool Exact>
  bool DispatchPass(Context& ctx, bool& refused,
                    std::string_view& guard) noexcept {
    return (TryRow<Event, Exact, Rows>(ctx, refused, guard) || ...);
  }

  template <class Event, bool Exact, class R>
  bool TryRow(Context& ctx, bool& refused, std::string_view& guard) noexcept {
    using From = typename R::from;
    using Ev = typename R::event;
    // The two early returns are distinct compile-time rejection reasons (event
    // mismatch vs. wrong pass of the exact-then-wildcard two-pass dispatch),
    // kept separate for traceability — not a copy-paste clone.
    // NOLINTNEXTLINE(bugprone-branch-clone)
    if constexpr (!std::is_same_v<Ev, Event>) {
      return false;  // event does not match this row
    } else if constexpr (std::is_same_v<From, AnyState> == Exact) {
      return false;  // wrong specificity for this pass
    } else {
      if constexpr (!std::is_same_v<From, AnyState>) {
        if (current_ != IndexOf<From>()) return false;  // not the current state
      }
      if (typename R::guard{}(static_cast<const Context&>(ctx))) {
        TransitionTo<typename R::to, typename R::action, Event>(ctx);
        return true;
      }
      if (!refused) {  // record the first refusing guard
        refused = true;
        guard = name<typename R::guard>();
      }
      return false;
    }
  }

  std::tuple<States...> states_{};
  std::size_t current_{0};
  FsmObserver* observer_{nullptr};
  bool in_dispatch_{false};
  bool started_{false};
};

}  // namespace ctfsm

#endif  // CTFSM_STATE_MACHINE_HPP_
