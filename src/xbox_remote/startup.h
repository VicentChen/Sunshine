/**
 * @file src/xbox_remote/startup.h
 * @brief Cancellable Xbox data-channel startup-handshake coordinator.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/xbox_remote/protocol.h"

namespace xbox_remote::startup {
  /**
   * @brief Sanitized startup-handshake error categories.
   */
  enum class error_e {
    none,  ///< The operation succeeded.
    cancelled,  ///< The caller cancelled the startup sequence.
    timeout,  ///< The handshake acknowledgement deadline expired.
    invalid_ack,  ///< The message channel returned an invalid acknowledgement.
    channel_closed,  ///< A required channel closed during startup.
    send_failed,  ///< A required channel rejected an outbound message.
    invalid_state,  ///< The caller attempted an invalid state transition.
  };

  /**
   * @brief Caller-visible failure without channel payload contents.
   */
  struct failure_t {
    error_e code = error_e::none;  ///< Machine-readable failure category.
    std::string stage;  ///< Fixed startup stage.
    std::string message;  ///< Non-sensitive diagnostic.
  };

  /**
   * @brief Result returned by startup operations.
   *
   * @tparam T Successful value type.
   */
  template<typename T>
  struct result_t {
    T value {};  ///< Successful value.
    failure_t error {};  ///< Sanitized failure.

    /**
     * @brief Check whether the operation succeeded.
     *
     * @return @c true when no error is present.
     */
    explicit operator bool() const {
      return error.code == error_e::none;
    }
  };

  /**
   * @brief Observable startup-handshake state.
   */
  enum class state_e {
    idle,  ///< No handshake has been started.
    waiting_ack,  ///< Handshake was sent and an acknowledgement is pending.
    waiting_gamepad_add,  ///< Authorization/remove completed and the add delay is pending.
    ready,  ///< All startup messages and client metadata were sent.
    failed,  ///< A terminal startup error occurred.
    cancelled,  ///< The caller cancelled startup.
  };

  /**
   * @brief Abstract non-blocking data-channel sender used by the coordinator.
   */
  class sender_t {
  public:
    virtual ~sender_t() = default;

    /**
     * @brief Send one UTF-8 message on a named channel.
     *
     * @param channel Required Xbox channel label.
     * @param payload UTF-8 protocol payload.
     * @return @c true when the transport accepted the payload.
     */
    virtual bool send_text(std::string_view channel, std::string_view payload) = 0;

    /**
     * @brief Send one binary message on a named channel.
     *
     * @param channel Required Xbox channel label.
     * @param payload Binary protocol payload.
     * @return @c true when the transport accepted the payload.
     */
    virtual bool send_binary(std::string_view channel, const std::vector<std::uint8_t> &payload) = 0;
  };

  /**
   * @brief Deterministic startup-handshake controls.
   */
  struct options_t {
    std::string handshake_id = "sunshine-xbox-remote";  ///< Non-secret message identifier.
    protocol::startup_message_parameters_t startup_messages {};  ///< Display and capability values.
    std::chrono::milliseconds ack_timeout {5000};  ///< Maximum HandshakeAck wait.
    std::chrono::milliseconds gamepad_add_delay {500};  ///< Required remove-to-add interval.
    std::uint8_t gamepad_index = 0;  ///< First-version Xbox gamepad slot.
  };

  /**
   * @brief Single-owner, non-blocking Xbox startup-handshake state machine.
   *
   * The owner feeds message-channel payloads and monotonic time through
   * @ref on_message and @ref poll. The class never sleeps and never performs
   * network I/O other than immediate calls to @ref sender_t.
   */
  class coordinator_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Monotonic scheduling clock.
    using time_point_t = clock_t::time_point;  ///< Monotonic timestamp type.

    /**
     * @brief Construct a startup coordinator.
     *
     * @param sender Non-blocking transport sender that outlives the coordinator.
     * @param options Deterministic protocol and timeout controls.
     */
    explicit coordinator_t(sender_t &sender, options_t options = {});

    /**
     * @brief Send the initial message-channel handshake.
     *
     * @param now Current monotonic time.
     * @return Success or a sanitized state/send failure.
     */
    result_t<bool> start(time_point_t now);

    /**
     * @brief Consume a message-channel payload while waiting for HandshakeAck.
     *
     * Valid duplicate acknowledgements after the first transition are ignored.
     *
     * @param payload UTF-8 message-channel payload.
     * @param now Current monotonic time.
     * @return @c true only when this payload completed the acknowledgement transition.
     */
    result_t<bool> on_message(std::string_view payload, time_point_t now);

    /**
     * @brief Advance timeout, cancellation, and delayed gamepad-add work.
     *
     * @param now Current monotonic time.
     * @param cancelled Whether the owning operation has been cancelled.
     * @return @c true when the startup sequence is ready.
     */
    result_t<bool> poll(time_point_t now, bool cancelled);

    /**
     * @brief Record closure of a required channel.
     *
     * @param channel Closed channel label.
     * @return Terminal failure for required channels, otherwise success.
     */
    result_t<bool> on_channel_closed(std::string_view channel);

    /**
     * @brief Return the current state.
     *
     * @return Current startup state.
     */
    state_e state() const;

    /**
     * @brief Return the terminal failure, if any.
     *
     * @return Sanitized failure or an empty failure while healthy.
     */
    const failure_t &failure() const;

  private:
    result_t<bool> fail(error_e code, std::string stage, std::string message);
    result_t<bool> send_text(std::string_view channel, std::string payload, std::string_view stage);
    result_t<bool> send_binary(std::string_view channel, std::vector<std::uint8_t> payload, std::string_view stage);
    result_t<bool> finish_startup(time_point_t now);

    sender_t &sender_;  ///< Non-owning transport sender.
    options_t options_;  ///< Immutable startup controls.
    state_e state_ = state_e::idle;  ///< Current state.
    failure_t failure_ {};  ///< Terminal sanitized failure.
    time_point_t ack_deadline_ {};  ///< Handshake acknowledgement deadline.
    time_point_t gamepad_add_at_ {};  ///< Earliest permitted add time.
  };
}  // namespace xbox_remote::startup
