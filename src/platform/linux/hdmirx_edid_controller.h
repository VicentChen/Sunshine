/**
 * @file src/platform/linux/hdmirx_edid_controller.h
 * @brief Process-level, idempotent HDMI RX EDID control plane.
 */
#pragma once

#include "src/platform/hdmirx_policy.h"
#include "src/platform/linux/edid.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace platf::hdmirx {

  /**
   * @brief Result of applying one Moonlight target to the HDMI receiver.
   */
  enum class edid_apply_status_e {
    live_match,  ///< Current live input already has the requested dimensions.
    unchanged,  ///< The exact projected EDID is already installed.
    applied,  ///< One new projected EDID was written and verified.
    read_failed,  ///< The current EDID could not be read.
    invalid_native,  ///< The saved native EDID is incomplete or invalid.
    no_mode,  ///< The native EDID contains no selectable video mode.
    projection_failed,  ///< The selected native mode could not be projected safely.
    write_failed,  ///< The single guarded write failed.
    verify_failed,  ///< Readback did not equal the requested bytes.
  };

  /**
   * @brief Immutable result returned to the caller after one control transaction.
   */
  struct edid_apply_result_t {
    edid_apply_status_e status {};  ///< Transaction outcome.
    std::optional<hdmi_mode_t> selected_mode;  ///< Closest native mode, when available.
    std::string message;  ///< Human-readable diagnostic.
  };

  /**
   * @brief Own EDID selection and writes independently from video capture.
   *
   * The object has no source-change or timing-notification API by design. One
   * target application performs zero writes when unchanged and at most one
   * normal write when the selected target changes. A second write is permitted
   * only to restore the native bytes after a failed or unverifiable write.
   */
  class edid_controller_t {
  public:
    /**
     * @brief Construct a controller with optional persistent process state.
     *
     * @param state_path Atomic state-file path, or empty for in-memory use.
     */
    explicit edid_controller_t(std::filesystem::path state_path = {});

    /**
     * @brief Apply the closest native EDID mode for a Moonlight target.
     *
     * @param backend Current receiver ioctl backend.
     * @param pad V4L2 EDID pad index.
     * @param target Moonlight requested resolution.
     * @param requested_refresh Optional Moonlight requested refresh rate.
     * @param current_input Current live HDMI dimensions, when a real frame exists.
     * @return Transaction outcome; failures never block video capture.
     */
    edid_apply_result_t apply_target(
      edid::ioctl_backend_t &backend,
      std::uint32_t pad,
      const resolution_t &target,
      std::optional<refresh_rate_t> requested_refresh = std::nullopt,
      std::optional<resolution_t> current_input = std::nullopt
    );

    /**
     * @brief Return the process-level native EDID snapshot.
     *
     * @return Copy of the native bytes, or an empty vector before initialization.
     */
    std::vector<std::uint8_t> native_edid() const;

    /**
     * @brief Clear cached process state for isolated unit tests.
     */
    void reset_for_tests() noexcept;

  private:
    /** @brief Best-effort single native-EDID restore after an abnormal write. */
    void restore_after_error(edid::ioctl_backend_t &backend, std::uint32_t pad) noexcept;
    /** @brief Enable receiver audio once without affecting the video result. */
    void enable_audio_once(edid::ioctl_backend_t &backend) noexcept;
    /** @brief Load a validated native/applied snapshot once. */
    void load_state_locked() noexcept;
    /** @brief Atomically persist validated native/applied bytes. */
    void persist_state_locked() const noexcept;

    mutable std::mutex mutex_;  ///< Serializes all EDID control transactions.
    std::vector<std::uint8_t> native_edid_;  ///< Validated receiver-native baseline.
    std::vector<std::uint8_t> last_applied_edid_;  ///< Last byte-exact Sunshine projection.
    std::filesystem::path state_path_;  ///< Optional crash-recovery state file.
    bool state_loaded_ {};  ///< Whether persistent state was inspected.
    bool audio_enable_attempted_ {};  ///< Prevents repeated audio-state ioctls.
  };

  /**
   * @brief Return the single process-level HDMI RX EDID controller.
   *
   * @return Shared controller instance.
   */
  edid_controller_t &process_edid_controller();

}  // namespace platf::hdmirx
