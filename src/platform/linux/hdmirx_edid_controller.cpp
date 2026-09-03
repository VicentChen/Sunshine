/**
 * @file src/platform/linux/hdmirx_edid_controller.cpp
 * @brief Process-level, idempotent HDMI RX EDID control implementation.
 */

#include "src/platform/linux/hdmirx_edid_controller.h"

#include <array>
#include <cstdlib>
#include <fstream>
#include <utility>

namespace platf::hdmirx {
  namespace {
    constexpr std::array<char, 8> k_state_magic {'S', 'S', 'E', 'D', 'I', 'D', '0', '1'};

    /** @brief Resolve a Linux per-user state path without platform-link dependencies. */
    std::filesystem::path default_state_path() {
      if (const auto *xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config && *xdg_config) {
        return std::filesystem::path {xdg_config} / "sunshine" / "hdmirx-edid-state.bin";
      }
      if (const auto *user_home = std::getenv("HOME"); user_home && *user_home) {
        return std::filesystem::path {user_home} / ".config" / "sunshine" / "hdmirx-edid-state.bin";
      }
      return {};
    }
  }  // namespace

  edid_controller_t::edid_controller_t(std::filesystem::path state_path):
      state_path_(std::move(state_path)) {}

  void input_mode_verifier_t::begin(
    const resolution_t &expected,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration timeout
  ) noexcept {
    expected_ = expected;
    last_actual_.reset();
    deadline_ = now + timeout;
  }

  std::optional<input_mode_result_t> input_mode_verifier_t::observe(
    std::optional<resolution_t> actual,
    std::chrono::steady_clock::time_point now
  ) noexcept {
    if (!expected_) {
      return std::nullopt;
    }
    if (actual) {
      last_actual_ = actual;
      if (*actual == *expected_) {
        const input_mode_result_t result {input_mode_status_e::matched, *expected_, actual};
        expected_.reset();
        return result;
      }
    }
    if (now < deadline_) {
      return std::nullopt;
    }
    const input_mode_result_t result {input_mode_status_e::timed_out, *expected_, last_actual_};
    expected_.reset();
    return result;
  }

  edid_apply_result_t edid_controller_t::apply_target(
    edid::ioctl_backend_t &backend,
    std::uint32_t pad,
    const resolution_t &target,
    std::optional<refresh_rate_t> requested_refresh,
    std::optional<resolution_t> current_input
  ) {
    std::lock_guard lock(mutex_);
    load_state_locked();

    auto current_result = edid::read_edid(backend, pad);
    if (!current_result) {
      if (current_input && *current_input == target) {
        enable_audio_once(backend);
        return {edid_apply_status_e::live_match, std::nullopt, "live HDMI input already matches Moonlight; EDID read was unnecessary"};
      }
      return {edid_apply_status_e::read_failed, std::nullopt, current_result.error().message};
    }
    auto current = std::move(*current_result);
    if (!edid::validate_edid_checksums(current)) {
      return {edid_apply_status_e::invalid_native, std::nullopt, "receiver EDID failed complete validation"};
    }

    if (native_edid_.empty() || (current != native_edid_ && current != last_applied_edid_)) {
      native_edid_ = current;
      last_applied_edid_.clear();
      persist_state_locked();
    }
    if (!edid::validate_edid_checksums(native_edid_)) {
      return {edid_apply_status_e::invalid_native, std::nullopt, "cached native EDID failed validation"};
    }

    const auto selected = select_hdmi_mode(edid::parse_edid_modes(native_edid_), target, requested_refresh);
    if (!selected) {
      return {edid_apply_status_e::no_mode, std::nullopt, "native EDID contains no selectable mode"};
    }
    if (current_input && *current_input == selected->resolution) {
      enable_audio_once(backend);
      return {edid_apply_status_e::live_match, selected, "live HDMI input matches the selected native mode"};
    }

    auto projected = edid::project_edid_for_mode(native_edid_, *selected);
    if (projected.empty()) {
      return {edid_apply_status_e::projection_failed, selected, "native EDID projection was not safe"};
    }

    const bool reasserted = current == projected;
    const auto write_result = edid::write_edid(backend, pad, projected);
    if (!write_result) {
      restore_after_error(backend, pad);
      return {edid_apply_status_e::write_failed, selected, write_result.error().message};
    }
    const auto readback = edid::read_edid(backend, pad);
    if (!readback || *readback != projected) {
      restore_after_error(backend, pad);
      return {edid_apply_status_e::verify_failed, selected, "EDID readback did not match the written projection"};
    }

    last_applied_edid_ = std::move(projected);
    persist_state_locked();
    enable_audio_once(backend);
    const auto message = reasserted ?
                           "target EDID reasserted and byte-verified; awaiting live HDMI timing" :
                           "target EDID written and byte-verified; awaiting live HDMI timing";
    return {edid_apply_status_e::advertised, selected, message};
  }

  std::vector<std::uint8_t> edid_controller_t::native_edid() const {
    std::lock_guard lock(mutex_);
    return native_edid_;
  }

  void edid_controller_t::reset_for_tests() noexcept {
    std::lock_guard lock(mutex_);
    native_edid_.clear();
    last_applied_edid_.clear();
    state_loaded_ = true;
    audio_enable_attempted_ = false;
  }

  void edid_controller_t::restore_after_error(edid::ioctl_backend_t &backend, std::uint32_t pad) noexcept {
    if (native_edid_.empty()) {
      return;
    }
    try {
      (void) edid::write_edid(backend, pad, native_edid_);
    } catch (...) {
    }
    last_applied_edid_.clear();
    persist_state_locked();
  }

  void edid_controller_t::enable_audio_once(edid::ioctl_backend_t &backend) noexcept {
    if (audio_enable_attempted_) {
      return;
    }
    audio_enable_attempted_ = true;
    try {
      (void) backend.set_audio_enabled(true);
    } catch (...) {
    }
  }

  void edid_controller_t::load_state_locked() noexcept {
    if (state_loaded_) {
      return;
    }
    state_loaded_ = true;
    if (state_path_.empty()) {
      return;
    }
    try {
      std::ifstream input(state_path_, std::ios::binary);
      std::array<char, k_state_magic.size()> magic {};
      std::uint32_t native_size {};
      std::uint32_t applied_size {};
      if (!input.read(magic.data(), magic.size()) || magic != k_state_magic || !input.read(reinterpret_cast<char *>(&native_size), sizeof(native_size)) || !input.read(reinterpret_cast<char *>(&applied_size), sizeof(applied_size)) || native_size > edid::k_max_edid_size || applied_size > edid::k_max_edid_size) {
        return;
      }
      std::vector<std::uint8_t> native(native_size);
      std::vector<std::uint8_t> applied(applied_size);
      if (!input.read(reinterpret_cast<char *>(native.data()), native.size()) || !input.read(reinterpret_cast<char *>(applied.data()), applied.size()) || !edid::validate_edid_checksums(native) || (!applied.empty() && !edid::validate_edid_checksums(applied))) {
        return;
      }
      native_edid_ = std::move(native);
      last_applied_edid_ = std::move(applied);
    } catch (...) {
    }
  }

  void edid_controller_t::persist_state_locked() const noexcept {
    if (state_path_.empty() || native_edid_.empty()) {
      return;
    }
    try {
      std::filesystem::create_directories(state_path_.parent_path());
      auto temporary = state_path_;
      temporary += ".tmp";
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      const auto native_size = static_cast<std::uint32_t>(native_edid_.size());
      const auto applied_size = static_cast<std::uint32_t>(last_applied_edid_.size());
      output.write(k_state_magic.data(), k_state_magic.size());
      output.write(reinterpret_cast<const char *>(&native_size), sizeof(native_size));
      output.write(reinterpret_cast<const char *>(&applied_size), sizeof(applied_size));
      output.write(reinterpret_cast<const char *>(native_edid_.data()), native_edid_.size());
      output.write(reinterpret_cast<const char *>(last_applied_edid_.data()), last_applied_edid_.size());
      output.flush();
      if (!output) {
        return;
      }
      output.close();
      std::filesystem::rename(temporary, state_path_);
    } catch (...) {
    }
  }

  edid_controller_t &process_edid_controller() {
    static edid_controller_t controller {default_state_path()};
    return controller;
  }

}  // namespace platf::hdmirx
