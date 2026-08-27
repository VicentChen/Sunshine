#pragma once
/**
 * @file src/platform/linux/input_state_machine.h
 * @brief HDMI RX input state machine for green placeholder frames and signal
 *        recovery.
 *
 * When HDMI input is unstable, source-changing, or has no signal, the state
 * machine outputs deterministic pure green NV12 frames at size T to keep the
 * Moonlight session alive. The green color values are correct for the NV12
 * color space (BT.709 limited range).
 *
 * ## State model
 *
 * @verbatim
 * starting/negotiating/no_signal
 *   -> output green frames @ T
 *   -> obtain stable timing AND valid captured frame
 * streaming_direct or streaming_rga
 *   -> source-change / signal loss
 * starting/negotiating/no_signal
 * @endverbatim
 *
 * Fatal device errors, MPP unrecoverable, or shutdown end the session.
 * Resolution mismatch and EDID not applied are NOT fatal errors.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace platf::input_sm {

  /**
   * @brief NV12 luma (Y) value for BT.709 limited-range green.
   *
   * Derived from BT.709 limited-range matrix for pure green (R=0, G=255, B=0):
   *   Y = round(16 + 65.481*0 + 128.553*1 + 24.966*0) = 149
   *
   * Clamped to valid limited range [16, 235].
   */
  inline constexpr std::uint8_t k_green_nv12_y = 149;

  /**
   * @brief NV12 Cb (U) value for BT.709 limited-range green.
   *
   * Using integer BT.709 coefficients for 8-bit [0,255] input:
   *   Cb = clamp(round((-38*R - 74*G + 112*B)/256 + 128), 16, 240)
   *   For R=0, G=255, B=0: Cb = round((-74*255)/256 + 128) = round(54.28) = 54
   */
  inline constexpr std::uint8_t k_green_nv12_u = 54;

  /**
   * @brief NV12 Cr (V) value for BT.709 limited-range green.
   *
   * Using integer BT.709 coefficients for 8-bit [0,255] input:
   *   Cr = clamp(round((112*R - 94*G - 18*B)/256 + 128), 16, 240)
   *   For R=0, G=255, B=0: Cr = round((-94*255)/256 + 128) = round(34.48) = 34
   */
  inline constexpr std::uint8_t k_green_nv12_v = 34;

  /**
   * @brief Packed NV12 green color for RGA fill().
   *
   * For the Rockchip RGA2/RGA3 API, the NV12 fill color is packed as:
   *   bits [7:0]   = Y
   *   bits [15:8]  = Cb (U)
   *   bits [23:16] = Cr (V)
   *   bits [31:24] = 0 (ignored)
   *
   * Result: (V << 16) | (U << 8) | Y = 0x00223695
   */
  inline constexpr std::uint32_t k_green_nv12_packed =
      (static_cast<std::uint32_t>(k_green_nv12_v) << 16U) |
      (static_cast<std::uint32_t>(k_green_nv12_u) << 8U) |
      static_cast<std::uint32_t>(k_green_nv12_y);

  /**
   * @brief Placeholder frame rate in frames per second.
   *
   * During placeholder state, encode at a low frame rate sufficient to keep
   * the Moonlight session alive without wasting encode resources. Moonlight
   * typically requires at least one frame every 5-10 seconds. We use 2 FPS
   * to provide a comfortable margin.
   */
  inline constexpr std::uint32_t k_placeholder_fps = 2;

  /**
   * @brief Interval between placeholder frames.
   *
   * Derived from k_placeholder_fps. At 2 FPS this is 500ms.
   */
  inline constexpr auto k_placeholder_interval =
      std::chrono::milliseconds(1000 / k_placeholder_fps);

  /**
   * @brief Number of consecutive stable timing queries required before
   *        declaring signal recovery.
   *
   * A single valid timing/frame is insufficient to confirm stable input.
   * This threshold prevents premature transitions from source-change storms.
   */
  inline constexpr std::uint32_t k_stable_timing_count = 3;

  /**
   * @brief Maximum time to wait for signal recovery before declaring no-signal.
   *
   * After losing signal, the state machine waits this long for timing to
   * stabilize before falling back to placeholder output.
   */
  inline constexpr auto k_signal_recovery_timeout = std::chrono::seconds(5);

  /**
   * @brief Input capture states for the HDMI RX state machine.
   *
   * State transitions:
   * - starting -> no_signal | shutdown | fatal
   * - no_signal -> negotiating | shutdown | fatal
   * - negotiating -> streaming_direct | streaming_rga | source_change | no_signal | shutdown | fatal
   * - streaming_direct -> source_change | shutdown | fatal
   * - streaming_rga -> source_change | shutdown | fatal
   * - source_change -> no_signal | negotiating | shutdown | fatal
   * - shutdown -> (terminal)
   * - fatal -> (terminal)
   */
  enum class state_e {
    starting,         ///< Initial state before first capture attempt.
    no_signal,        ///< No HDMI signal detected; outputting green frames.
    negotiating,      ///< Signal detected but not yet stable; outputting green frames.
    streaming_direct, ///< Stable signal, I == T, direct encode path.
    streaming_rga,    ///< Stable signal, I != T, RGA conversion path.
    source_change,    ///< Source change event detected; re-entering negotiation.
    shutdown,         ///< Clean shutdown requested.
    fatal,            ///< Unrecoverable device error; session must end.
  };

  /**
   * @brief Human-readable name for a state_e value.
   *
   * @param state State to describe.
   * @return Null-terminated compile-time string view.
   */
  constexpr std::string_view state_name(state_e state) noexcept {
    switch (state) {
      case state_e::starting:         return "starting";
      case state_e::no_signal:        return "no_signal";
      case state_e::negotiating:      return "negotiating";
      case state_e::streaming_direct: return "streaming_direct";
      case state_e::streaming_rga:    return "streaming_rga";
      case state_e::source_change:    return "source_change";
      case state_e::shutdown:         return "shutdown";
      case state_e::fatal:            return "fatal";
    }
    return "unknown";
  }

  /**
   * @brief Whether a state outputs green placeholder frames.
   *
   * @param state State to test.
   * @return true when the state machine should produce green frames.
   */
  constexpr bool is_placeholder_state(state_e state) noexcept {
    switch (state) {
      case state_e::starting:
      case state_e::no_signal:
      case state_e::negotiating:
      case state_e::source_change:
        return true;
      default:
        return false;
    }
  }

  /**
   * @brief Whether a state is streaming real captured frames.
   *
   * @param state State to test.
   * @return true when the state machine should produce real frames.
   */
  constexpr bool is_streaming_state(state_e state) noexcept {
    return state == state_e::streaming_direct || state == state_e::streaming_rga;
  }

  /**
   * @brief Whether a state is terminal (no further transitions).
   *
   * @param state State to test.
   * @return true when the state machine has reached a final state.
   */
  constexpr bool is_terminal_state(state_e state) noexcept {
    return state == state_e::shutdown || state == state_e::fatal;
  }

  /**
   * @brief Signal recovery event describing a successfully recovered input.
   */
  struct recovery_event_t {
    std::uint32_t width {};   ///< Recovered input width.
    std::uint32_t height {};  ///< Recovered input height.
    bool needs_rga {};        ///< Whether RGA conversion is required.
  };

  /**
   * @brief Thread-safe HDMI RX input state machine.
   *
   * Controls transitions between placeholder (green frame) output and real
   * captured frame streaming. All public methods are thread-safe.
   *
   * The state machine tracks:
   * - Current state and last transition reason
   * - Whether an IDR frame should be requested on the next encode
   * - Stable timing count for recovery validation
   * - Shutdown signalling for interruptible waits
   */
  class state_machine_t {
  public:
    /**
     * @brief Construct a state machine in the starting state.
     *
     * The first green frame will request an IDR.
     */
    state_machine_t() = default;

    /** @brief State machines are not copyable. */
    state_machine_t(const state_machine_t &) = delete;
    /** @brief State machines are not copyable. */
    state_machine_t &operator=(const state_machine_t &) = delete;

    /**
     * @brief Get the current state.
     *
     * @return Current state_e value.
     */
    state_e state() const noexcept;

    /**
     * @brief Transition to no_signal state.
     *
     * Valid from: starting, negotiating, source_change.
     * First green frame after entering this state will request IDR.
     *
     * @param reason Human-readable reason for the transition.
     * @return true if the transition was accepted.
     */
    bool enter_no_signal(std::string_view reason = "no signal");

    /**
     * @brief Transition to negotiating state.
     *
     * Valid from: no_signal, source_change.
     * Resets stable timing counter.
     *
     * @param reason Human-readable reason for the transition.
     * @return true if the transition was accepted.
     */
    bool enter_negotiating(std::string_view reason = "timing detected");

    /**
     * @brief Record one stable timing observation during negotiation.
     *
     * When k_stable_timing_count consecutive stable observations are recorded,
     * the state machine is ready for a streaming transition.
     *
     * @return Number of consecutive stable observations so far.
     */
    std::uint32_t record_stable_timing();

    /**
     * @brief Discard partial timing observations without changing state.
     *
     * A receiver can report a valid but different timing while the upstream
     * device is switching modes. Such observations must not be combined into
     * one lock decision.
     */
    void reset_stable_timing() noexcept;

    /**
     * @brief Check whether enough stable timing observations have been recorded.
     *
     * @return true when record_stable_timing() has been called k_stable_timing_count
     *         times since entering the negotiating state.
     */
    bool timing_is_stable() const noexcept;

    /**
     * @brief Transition to streaming_direct state.
     *
     * Valid from: negotiating (when timing is stable).
     * Requests IDR on the first real frame.
     *
     * @param reason Human-readable reason for the transition.
     * @return true if the transition was accepted.
     */
    bool enter_streaming_direct(std::string_view reason = "direct path");

    /**
     * @brief Transition to streaming_rga state.
     *
     * Valid from: negotiating (when timing is stable).
     * Requests IDR on the first real frame.
     *
     * @param reason Human-readable reason for the transition.
     * @return true if the transition was accepted.
     */
    bool enter_streaming_rga(std::string_view reason = "RGA path");

    /**
     * @brief Transition to source_change state.
     *
     * Valid from: streaming_direct, streaming_rga, negotiating.
     * Resets stable timing counter.
     *
     * @param reason Human-readable reason for the transition.
     * @return true if the transition was accepted.
     */
    bool enter_source_change(std::string_view reason = "source change");

    /**
     * @brief Transition to shutdown state.
     *
     * Valid from any non-terminal state. Wakes up any thread waiting on
     * wait_for_interval().
     *
     * @param reason Human-readable reason for the shutdown.
     * @return true if the transition was accepted.
     */
    bool enter_shutdown(std::string_view reason = "shutdown");

    /**
     * @brief Transition to fatal error state.
     *
     * Valid from any non-terminal state.
     *
     * Resolution mismatch and EDID not applied MUST NOT trigger this.
     * Only device errors, MPP unrecoverable errors, etc.
     *
     * @param reason Human-readable reason for the fatal error.
     * @return true if the transition was accepted.
     */
    bool enter_fatal(std::string_view reason = "fatal error");

    /**
     * @brief Consume and clear the pending IDR request.
     *
     * Called by the encode loop before encoding a frame. Returns true
     * exactly once after each state transition that requires an IDR.
     *
     * @return true if an IDR frame should be produced.
     */
    bool consume_idr_request() noexcept;

    /**
     * @brief Wait for the placeholder frame interval or until interrupted.
     *
     * Blocks for k_placeholder_interval unless shutdown is signaled, in which
     * case it returns immediately. Safe to call from a single producer thread.
     *
     * @return true if the wait completed normally (time to send a frame).
     *         false if interrupted by shutdown.
     */
    bool wait_for_interval();

    /**
     * @brief Get the human-readable reason for the last state transition.
     *
     * @return Reason string from the last successful transition.
     */
    std::string last_reason() const;

    /**
     * @brief Get the total number of state transitions.
     *
     * @return Transition count since construction.
     */
    std::uint64_t transition_count() const noexcept;

  private:
    mutable std::mutex mutex_;                     ///< Protects all mutable state.
    std::condition_variable shutdown_cv_;           ///< Signaled on shutdown for wait interruption.
    state_e state_ {state_e::starting};            ///< Current state.
    std::string last_reason_ {"initial"};          ///< Reason for the last transition.
    std::uint32_t stable_count_ {};                ///< Consecutive stable timing observations.
    bool idr_pending_ {true};                      ///< Whether the next frame should be IDR.
    bool shutdown_requested_ {};                   ///< Whether shutdown has been requested.
    std::uint64_t transitions_ {};                 ///< Total transition count.
  };

}  // namespace platf::input_sm
#include <memory>
#include <vector>
#include "src/platform/linux/edid.h"
#include "src/platform/linux/input_state_machine.h"
#include "src/platform/hdmirx_policy.h"

namespace platf::hdmirx {

class session_negotiator_t {
public:
  session_negotiator_t(edid::ioctl_backend_t &backend, input_sm::state_machine_t &sm, std::uint32_t pad);

  // Perform the initial EDID negotiation step.
  void start_negotiation(const resolution_t &target, const std::vector<hdmi_mode_t>& candidates);

  // Check if we locked, returning true if transition to streaming is done.
  bool check_lock(const std::optional<resolution_t>& actual_input);

  void end_session();

private:
  edid::ioctl_backend_t &backend_;
  input_sm::state_machine_t &sm_;
  std::uint32_t pad_;
  std::unique_ptr<edid::edid_restore_guard_t> guard_;
  std::optional<resolution_t> target_;
  std::optional<resolution_t> last_input_;
};

} // namespace platf::hdmirx
