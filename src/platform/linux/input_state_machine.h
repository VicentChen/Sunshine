#pragma once
/**
 * @file src/platform/linux/input_state_machine.h
 * @brief HDMI RX live-frame and capture-recovery state.
 *
 * The state machine describes the video data plane only. EDID programming is
 * deliberately absent: a successfully dequeued frame always transitions
 * directly to a live state and can never be withheld by link negotiation.
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace platf::input_sm {

  /// @brief NV12 luma value for BT.709 limited-range green.
  inline constexpr std::uint8_t k_green_nv12_y = 149;
  /// @brief NV12 Cb value for BT.709 limited-range green.
  inline constexpr std::uint8_t k_green_nv12_u = 54;
  /// @brief NV12 Cr value for BT.709 limited-range green.
  inline constexpr std::uint8_t k_green_nv12_v = 34;
  /// @brief Packed RGA NV12 fill value for BT.709 limited-range green.
  inline constexpr std::uint32_t k_green_nv12_packed =
    (static_cast<std::uint32_t>(k_green_nv12_v) << 16U) |
    (static_cast<std::uint32_t>(k_green_nv12_u) << 8U) |
    static_cast<std::uint32_t>(k_green_nv12_y);

  /// @brief Placeholder frame rate while the receiver has no usable frame.
  inline constexpr std::uint32_t k_placeholder_fps = 2;
  /// @brief Interval between no-signal placeholder frames.
  inline constexpr auto k_placeholder_interval = std::chrono::milliseconds(1000 / k_placeholder_fps);
  /// @brief Maximum capture wait before the receiver is treated as unavailable.
  inline constexpr auto k_signal_recovery_timeout = std::chrono::seconds(5);

  /**
   * @brief HDMI RX data-plane states.
   */
  enum class state_e {
    starting,  ///< Capture has not produced a frame yet.
    no_signal,  ///< No usable HDMI frame is available.
    streaming_direct,  ///< A real frame matches the Moonlight dimensions.
    streaming_rga,  ///< A real frame requires RGA conversion.
    reconfiguring,  ///< The V4L2 queue is being rebuilt after a source event.
    shutdown,  ///< Clean shutdown was requested.
    fatal,  ///< An unrecoverable data-plane error occurred.
  };

  /**
   * @brief Return a stable diagnostic name for a data-plane state.
   *
   * @param state State to name.
   * @return Compile-time state name.
   */
  constexpr std::string_view state_name(state_e state) noexcept {
    switch (state) {
      case state_e::starting:
        return "starting";
      case state_e::no_signal:
        return "no_signal";
      case state_e::streaming_direct:
        return "streaming_direct";
      case state_e::streaming_rga:
        return "streaming_rga";
      case state_e::reconfiguring:
        return "reconfiguring";
      case state_e::shutdown:
        return "shutdown";
      case state_e::fatal:
        return "fatal";
    }
    return "unknown";
  }

  /**
   * @brief Check whether no real frame is currently available.
   *
   * @param state State to classify.
   * @return true for starting, no-signal, and queue-reconfiguration states.
   */
  constexpr bool is_placeholder_state(state_e state) noexcept {
    return state == state_e::starting || state == state_e::no_signal || state == state_e::reconfiguring;
  }

  /**
   * @brief Check whether real captured frames are flowing.
   *
   * @param state State to classify.
   * @return true for direct and RGA live states.
   */
  constexpr bool is_streaming_state(state_e state) noexcept {
    return state == state_e::streaming_direct || state == state_e::streaming_rga;
  }

  /**
   * @brief Check whether no further transitions are accepted.
   *
   * @param state State to classify.
   * @return true for shutdown and fatal states.
   */
  constexpr bool is_terminal_state(state_e state) noexcept {
    return state == state_e::shutdown || state == state_e::fatal;
  }

  /**
   * @brief Thread-safe state owned exclusively by the HDMI RX data plane.
   *
   * A frame observation immediately selects direct or RGA streaming. There is
   * no stability counter and no EDID-dependent intermediate state.
   */
  class state_machine_t {
  public:
    /// @brief Construct the state machine in the starting state.
    state_machine_t() = default;
    /// @brief State machines are not copyable.
    state_machine_t(const state_machine_t &) = delete;
    /// @brief State machines are not copyable.
    state_machine_t &operator=(const state_machine_t &) = delete;

    /**
     * @brief Return the current data-plane state.
     *
     * @return Current state.
     */
    state_e state() const noexcept;

    /**
     * @brief Record that no usable HDMI frame is available.
     *
     * @param reason Diagnostic reason.
     * @return true when the state changed.
     */
    bool enter_no_signal(std::string_view reason = "no signal");

    /**
     * @brief Record one successfully dequeued real frame.
     *
     * This transition is accepted from every non-terminal state and takes
     * effect immediately. It is the sole authority for selecting a live state.
     *
     * @param needs_rga Whether the frame requires conversion to the Moonlight size.
     * @param reason Diagnostic reason.
     * @return true when the state changed.
     */
    bool enter_streaming(bool needs_rga, std::string_view reason = "real HDMI frame dequeued");

    /**
     * @brief Record that the V4L2 queue is being rebuilt.
     *
     * @param reason Diagnostic reason.
     * @return true when the state changed.
     */
    bool enter_reconfiguring(std::string_view reason = "source change");

    /**
     * @brief Enter the terminal shutdown state and interrupt placeholder waits.
     *
     * @param reason Diagnostic reason.
     * @return true when shutdown was accepted.
     */
    bool enter_shutdown(std::string_view reason = "shutdown");

    /**
     * @brief Enter the terminal fatal state.
     *
     * @param reason Diagnostic reason.
     * @return true when the fatal transition was accepted.
     */
    bool enter_fatal(std::string_view reason = "fatal error");

    /**
     * @brief Consume the pending request for an IDR on the next encoded frame.
     *
     * @return true exactly once after a route or availability transition.
     */
    bool consume_idr_request() noexcept;

    /**
     * @brief Wait for the placeholder interval or shutdown.
     *
     * @return true after the interval, false when interrupted by shutdown.
     */
    bool wait_for_interval();

    /**
     * @brief Return the latest transition reason.
     *
     * @return Copy of the diagnostic reason.
     */
    std::string last_reason() const;

    /**
     * @brief Return the number of accepted state changes.
     *
     * @return Transition count.
     */
    std::uint64_t transition_count() const noexcept;

  private:
    /**
     * @brief Change state while mutex_ is held.
     *
     * @param next Destination state.
     * @param reason Diagnostic reason.
     * @param request_idr Whether the transition needs an IDR.
     * @return true when the state changed.
     */
    bool transition_locked(state_e next, std::string_view reason, bool request_idr);

    mutable std::mutex mutex_;  ///< Protects mutable state.
    std::condition_variable shutdown_cv_;  ///< Interrupts placeholder waits.
    state_e state_ {state_e::starting};  ///< Current data-plane state.
    std::string last_reason_ {"initial"};  ///< Latest accepted transition reason.
    bool idr_pending_ {true};  ///< Whether the next encoded frame needs an IDR.
    bool shutdown_requested_ {};  ///< Whether waiters must stop.
    std::uint64_t transitions_ {};  ///< Accepted state-change count.
  };

}  // namespace platf::input_sm
