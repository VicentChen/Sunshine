/**
 * @file src/platform/linux/input_state_machine.cpp
 * @brief HDMI RX live-frame and capture-recovery state implementation.
 */

#include "src/platform/linux/input_state_machine.h"

#include <utility>

namespace platf::input_sm {

  bool state_machine_t::transition_locked(state_e next, std::string_view reason, bool request_idr) {
    if (is_terminal_state(state_) || state_ == next) {
      return false;
    }
    state_ = next;
    last_reason_ = reason;
    idr_pending_ = idr_pending_ || request_idr;
    ++transitions_;
    return true;
  }

  state_e state_machine_t::state() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
  }

  bool state_machine_t::enter_no_signal(std::string_view reason) {
    std::lock_guard lock(mutex_);
    return transition_locked(state_e::no_signal, reason, true);
  }

  bool state_machine_t::enter_streaming(bool needs_rga, std::string_view reason) {
    std::lock_guard lock(mutex_);
    return transition_locked(needs_rga ? state_e::streaming_rga : state_e::streaming_direct, reason, true);
  }

  bool state_machine_t::enter_reconfiguring(std::string_view reason) {
    std::lock_guard lock(mutex_);
    return transition_locked(state_e::reconfiguring, reason, true);
  }

  bool state_machine_t::enter_shutdown(std::string_view reason) {
    std::lock_guard lock(mutex_);
    if (!transition_locked(state_e::shutdown, reason, false)) {
      return false;
    }
    shutdown_requested_ = true;
    shutdown_cv_.notify_all();
    return true;
  }

  bool state_machine_t::enter_fatal(std::string_view reason) {
    std::lock_guard lock(mutex_);
    return transition_locked(state_e::fatal, reason, false);
  }

  bool state_machine_t::consume_idr_request() noexcept {
    std::lock_guard lock(mutex_);
    return std::exchange(idr_pending_, false);
  }

  bool state_machine_t::wait_for_interval() {
    std::unique_lock lock(mutex_);
    if (shutdown_requested_) {
      return false;
    }
    return !shutdown_cv_.wait_for(lock, k_placeholder_interval, [this] {
      return shutdown_requested_;
    });
  }

  std::string state_machine_t::last_reason() const {
    std::lock_guard lock(mutex_);
    return last_reason_;
  }

  std::uint64_t state_machine_t::transition_count() const noexcept {
    std::lock_guard lock(mutex_);
    return transitions_;
  }

}  // namespace platf::input_sm
