/**
 * @file src/platform/linux/input_state_machine.cpp
 * @brief Implementation of the HDMI RX input state machine.
 */

#include "src/platform/linux/input_state_machine.h"

#include <algorithm>
#include <iterator>

namespace platf::input_sm {

  state_e state_machine_t::state() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  bool state_machine_t::enter_no_signal(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
      case state_e::starting:
      case state_e::negotiating:
      case state_e::source_change:
        state_ = state_e::no_signal;
        last_reason_ = reason;
        stable_count_ = 0;
        idr_pending_ = true;
        ++transitions_;
        return true;
      default:
        return false;
    }
  }

  bool state_machine_t::enter_negotiating(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
      case state_e::starting:
      case state_e::no_signal:
      case state_e::source_change:
        state_ = state_e::negotiating;
        last_reason_ = reason;
        stable_count_ = 0;
        ++transitions_;
        return true;
      default:
        return false;
    }
  }

  std::uint32_t state_machine_t::record_stable_timing() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == state_e::negotiating) {
      ++stable_count_;
    }
    return stable_count_;
  }

  void state_machine_t::reset_stable_timing() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == state_e::negotiating) {
      stable_count_ = 0;
    }
  }

  bool state_machine_t::timing_is_stable() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stable_count_ >= k_stable_timing_count;
  }

  bool state_machine_t::enter_streaming_direct(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != state_e::negotiating || stable_count_ < k_stable_timing_count) {
      return false;
    }
    state_ = state_e::streaming_direct;
    last_reason_ = reason;
    idr_pending_ = true;
    ++transitions_;
    return true;
  }

  bool state_machine_t::enter_streaming_rga(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != state_e::negotiating || stable_count_ < k_stable_timing_count) {
      return false;
    }
    state_ = state_e::streaming_rga;
    last_reason_ = reason;
    idr_pending_ = true;
    ++transitions_;
    return true;
  }

  bool state_machine_t::enter_source_change(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (state_) {
      case state_e::streaming_direct:
      case state_e::streaming_rga:
      case state_e::negotiating:
        state_ = state_e::source_change;
        last_reason_ = reason;
        stable_count_ = 0;
        idr_pending_ = true;
        ++transitions_;
        return true;
      default:
        return false;
    }
  }

  bool state_machine_t::enter_shutdown(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_terminal_state(state_)) {
      return false;
    }
    state_ = state_e::shutdown;
    last_reason_ = reason;
    shutdown_requested_ = true;
    ++transitions_;
    shutdown_cv_.notify_all();
    return true;
  }

  bool state_machine_t::enter_fatal(std::string_view reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_terminal_state(state_)) {
      return false;
    }
    state_ = state_e::fatal;
    last_reason_ = reason;
    ++transitions_;
    return true;
  }

  bool state_machine_t::consume_idr_request() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (idr_pending_) {
      idr_pending_ = false;
      return true;
    }
    return false;
  }

  bool state_machine_t::wait_for_interval() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutdown_requested_) {
      return false;
    }
    return !shutdown_cv_.wait_for(lock, k_placeholder_interval, [this] {
      return shutdown_requested_;
    });
  }

  std::string state_machine_t::last_reason() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_reason_;
  }

  std::uint64_t state_machine_t::transition_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return transitions_;
  }

}  // namespace platf::input_sm

#include <iostream>

namespace platf::hdmirx {

  session_negotiator_t::session_negotiator_t(edid::ioctl_backend_t &backend, input_sm::state_machine_t &sm, std::uint32_t pad):
      backend_(backend),
      sm_(sm),
      pad_(pad) {}

  void session_negotiator_t::start_negotiation(
    const resolution_t &target,
    const std::vector<hdmi_mode_t> &candidates,
    std::optional<resolution_t> current_input
  ) {
    target_ = target;
    last_input_.reset();

    // An already matching live timing is the only case where EDID negotiation
    // cannot improve the route. Any mismatch must still select and apply the
    // closest advertised mode so RGA remains a fallback rather than policy.
    if (current_input && *current_input == target) {
      sm_.enter_negotiating("current HDMI timing already matches target; skipped EDID write");
      for (std::uint32_t sample = 0; sample < input_sm::k_stable_timing_count; ++sample) {
        if (check_lock(current_input)) {
          break;
        }
      }
      return;
    }
    // The current EDID writer has exact timing fixtures for these four
    // resolutions. Do not silently turn an unsupported selected mode into a
    // 1080p EDID: select the closest mode that can actually be advertised.
    std::vector<hdmi_mode_t> writable_candidates;
    std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(writable_candidates), [](const auto &mode) {
      const auto &resolution = mode.resolution;
      return resolution == resolution_t {1280, 720} ||
             resolution == resolution_t {1920, 1080} ||
             resolution == resolution_t {2560, 1440} ||
             resolution == resolution_t {3840, 2160};
    });
    auto selected = select_hdmi_mode(writable_candidates, target);
    auto caps = edid::probe_capabilities(backend_, pad_);

    // The capability probe is intentionally read-only.  Do not write anything
    // until the guard has independently saved the complete original EDID.
    if (!selected || !caps.readable) {
      // skip negotiation
      sm_.enter_negotiating("skipped edid");
      return;
    }

    // EDID can be safely written.
    guard_ = std::make_unique<edid::edid_restore_guard_t>(backend_, pad_);
    if (!guard_->is_armed()) {
      guard_.reset();
      sm_.enter_negotiating("EDID original could not be saved; skipping write");
      return;
    }

    // Keep the receiver's audio, HDMI VSDB, and HDMI Forum VSDB capabilities.
    // Real HDMI 2.0 sources can reject a synthetic minimal EDID even when its
    // single video timing is valid, falling back to 640x480 instead of 4K60.
    const auto new_edid = edid::restrict_edid_to_resolution(guard_->saved_edid(), selected->resolution);
    if (new_edid.empty()) {
      guard_.reset();
      sm_.enter_negotiating("selected mode could not be represented while preserving the original EDID");
      return;
    }

    auto res = edid::write_edid(backend_, pad_, new_edid);
    if (!res.has_value()) {
      // A failed write can be partial.  Restore immediately while the guard
      // still owns the complete original and make the fallback observable.
      const bool restored = guard_->restore();
      guard_.reset();
      sm_.enter_negotiating(restored ? "EDID write failed; original restored" : "EDID write failed; original restore pending");
      return;
    }

    // RK3588's VIDIOC_S_EDID implementation already drives HPD low, leaves it
    // low while programming EDID, and schedules the hot-plug worker to raise
    // HPD after one second.  Issuing RK_HDMIRX_CMD_SOFT_RESET here resets the
    // same receiver registers before that pulse completes and can leave a
    // source transmitting its old timing against the new EDID.
    sm_.enter_negotiating("EDID written; driver HPD renegotiation requested");
  }

  bool session_negotiator_t::check_lock(const std::optional<resolution_t> &actual_input) {
    if (sm_.state() != input_sm::state_e::negotiating) {
      return false;
    }

    if (!actual_input) {
      last_input_.reset();
      sm_.enter_no_signal("no signal");
      return false;
    }

    if (actual_input->width == 0 || actual_input->height == 0) {
      last_input_.reset();
      sm_.enter_no_signal("invalid HDMI timings");
      return false;
    }

    if (last_input_ && *last_input_ != *actual_input) {
      sm_.reset_stable_timing();
    }
    last_input_ = actual_input;

    sm_.record_stable_timing();
    if (sm_.timing_is_stable()) {
      // Rockchip HDMI RX disables its audio domain during link setup.  Start
      // its optional audio state machine after input timing has stabilized.
      (void) backend_.set_audio_enabled(true);
      if (target_ && actual_input->width == target_->width && actual_input->height == target_->height) {
        sm_.enter_streaming_direct("timing matches");
      } else {
        sm_.enter_streaming_rga("timing mismatch");
      }
      return true;
    }
    return false;
  }

  void session_negotiator_t::end_session() {
    if (guard_) {
      (void) guard_->restore();
      guard_.reset();
    }
    sm_.enter_shutdown();
  }

}  // namespace platf::hdmirx
