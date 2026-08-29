/**
 * @file src/input/nxbt_client.h
 * @brief Non-blocking Sunshine client for the local NXBT Bridge protocol.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// local includes
#include "src/input/nxbt_protocol.h"

namespace input::nxbt {
  /**
   * @brief Result of one bounded transport operation.
   */
  enum class transport_result_e {
    success,  ///< The operation completed successfully.
    timeout,  ///< The operation did not complete before its deadline.
    disconnected,  ///< The peer disconnected or the transport failed.
  };

  /**
   * @brief Result of receiving one packet from a Bridge transport.
   */
  struct receive_result_t {
    transport_result_e result = transport_result_e::timeout;  ///< Receive outcome.
    std::vector<std::uint8_t> packet;  ///< Complete packet when the operation succeeds.
    std::error_code error;  ///< System error when the transport fails.
  };

  /**
   * @brief Bounded packet transport used by the NXBT client worker.
   *
   * Implementations must return from every method no later than the supplied
   * timeout. The client never invokes a transport from Sunshine's input path.
   */
  class transport_t {
  public:
    /**
     * @brief Destroy the transport after the worker has stopped using it.
     */
    virtual ~transport_t() = default;

    /**
     * @brief Connect to a local Bridge endpoint.
     *
     * @param endpoint Unix-domain-socket path or injected transport endpoint.
     * @param timeout Maximum time allowed for the connection attempt.
     * @return Connection outcome and any system error.
     */
    virtual std::pair<transport_result_e, std::error_code> connect(const std::string &endpoint, std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Send exactly one packet.
     *
     * @param packet Complete protocol packet.
     * @param timeout Maximum time allowed for the send.
     * @return Send outcome and any system error.
     */
    virtual std::pair<transport_result_e, std::error_code> send(const std::vector<std::uint8_t> &packet, std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Receive at most one complete packet.
     *
     * @param timeout Maximum time to wait for a packet.
     * @return Receive outcome, packet, and any system error.
     */
    virtual receive_result_t receive(std::chrono::milliseconds timeout) = 0;

    /**
     * @brief Close the current connection and release its descriptor.
     */
    virtual void close() = 0;
  };

  /**
   * @brief Factory for a fresh transport on each reconnection attempt.
   */
  using transport_factory_t = std::function<std::unique_ptr<transport_t>()>;

  /**
   * @brief Observable NXBT client worker event.
   */
  enum class client_event_e {
    connected,  ///< Protocol negotiation completed.
    connect_failed,  ///< The Bridge endpoint could not be opened.
    handshake_failed,  ///< The Bridge rejected or did not finish negotiation.
    disconnected,  ///< An established Bridge connection ended.
    malformed_reply,  ///< The Bridge returned an invalid protocol packet.
    bridge_error,  ///< The Bridge returned an explicit error packet.
    heartbeat_timeout,  ///< A ping was not acknowledged before its deadline.
    controller_status,  ///< The Bridge reported a controller status change.
  };

  /**
   * @brief Details delivered with an NXBT client event.
   */
  struct client_event_t {
    client_event_e type = client_event_e::connect_failed;  ///< Event category.
    std::error_code system_error;  ///< Transport error, when applicable.
    protocol_error_e protocol_error = protocol_error_e::none;  ///< Peer or decoder error.
    std::uint8_t controller_id = 0;  ///< Related controller slot, when applicable.
    controller_status_e controller_status = controller_status_e::unavailable;  ///< Reported controller state.
  };

  /**
   * @brief Callback invoked on the client worker thread for observable events.
   */
  using client_event_callback_t = std::function<void(const client_event_t &)>;

  /**
   * @brief Thread-safe diagnostic snapshot for one NXBT client.
   */
  struct client_diagnostics_t {
    std::string endpoint;  ///< Configured local Bridge socket path.
    std::uint16_t negotiated_protocol_version = 0;  ///< Active protocol version, or zero before negotiation.
    bool socket_connected = false;  ///< Whether a negotiated Bridge connection is active.
    bool heartbeat_healthy = false;  ///< Whether the active connection has avoided a heartbeat timeout.
    bool has_last_error = false;  ///< Whether an error event has been observed.
    client_event_e last_error = client_event_e::connect_failed;  ///< Most recent error category.
    std::error_code last_system_error;  ///< Most recent transport error.
    protocol_error_e last_protocol_error = protocol_error_e::none;  ///< Most recent protocol error.
    std::vector<controller_status_e> controller_statuses;  ///< Latest status reported for each bounded controller slot.
  };

  /**
   * @brief Return a stable log label for an NXBT client event.
   *
   * @param event Event category.
   * @return Lowercase event label.
   */
  std::string_view client_event_name(client_event_e event);

  /**
   * @brief Return a stable log label for an NXBT controller status.
   *
   * @param status Controller state.
   * @return Lowercase status label.
   */
  std::string_view controller_status_name(controller_status_e status);

  /**
   * @brief Timing and endpoint settings for the NXBT client worker.
   */
  struct client_options_t {
    std::string endpoint = "/run/nxbt-bridge/control.sock";  ///< Local Bridge socket path.
    std::chrono::milliseconds connect_timeout {50};  ///< Bound for one connect attempt.
    std::chrono::milliseconds io_timeout {10};  ///< Bound for one packet send or receive.
    std::chrono::milliseconds handshake_timeout {250};  ///< Bound for hello acknowledgement.
    std::chrono::milliseconds reconnect_delay {100};  ///< Delay between failed connections.
    std::chrono::milliseconds heartbeat_interval {100};  ///< Interval between health checks.
    std::chrono::milliseconds heartbeat_timeout {300};  ///< Maximum wait for a matching pong.
    std::chrono::milliseconds error_log_interval {5000};  ///< Minimum interval for repeated error events.
  };

  /**
   * @brief Create the production Unix-domain `SOCK_SEQPACKET` transport factory.
   *
   * @return Factory suitable for constructing an NXBT client.
   */
  transport_factory_t make_unix_transport_factory();

  /**
   * @brief Own a reconnecting NXBT IPC worker with bounded latest-state storage.
   */
  class client_t {
  public:
    /**
     * @brief Start a client worker.
     *
     * @param options Endpoint and worker timing configuration.
     * @param transport_factory Factory used for every connection attempt.
     * @param event_callback Optional worker-event observer.
     */
    client_t(client_options_t options, transport_factory_t transport_factory, client_event_callback_t event_callback = {});

    /**
     * @brief Stop the worker after a bounded best-effort neutralize and detach.
     */
    ~client_t();

    client_t(const client_t &) = delete;
    client_t &operator=(const client_t &) = delete;
    client_t(client_t &&) = delete;
    client_t &operator=(client_t &&) = delete;

    /**
     * @brief Attach one logical Sunshine controller without blocking on IPC.
     *
     * @param controller_id Bridge controller slot.
     * @param client_relative_id Moonlight client-relative controller id.
     * @return @c true when the logical attachment was accepted.
     */
    bool attach(std::uint8_t controller_id, std::uint8_t client_relative_id);

    /**
     * @brief Rebind an attached slot to a resumed Moonlight controller id.
     *
     * @param controller_id Bridge controller slot.
     * @param client_relative_id New client-relative controller id.
     * @return @c true when the logical rebind was accepted.
     */
    bool rebind(std::uint8_t controller_id, std::uint8_t client_relative_id);

    /**
     * @brief Replace the pending latest state for one attached slot.
     *
     * @param state Complete mapped controller state.
     * @return @c true when the bounded latest-state slot accepted the update.
     */
    bool update(const controller_state_t &state);

    /**
     * @brief Queue neutral input for one attached slot and discard stale state.
     *
     * @param controller_id Bridge controller slot.
     */
    void neutralize(std::uint8_t controller_id);

    /**
     * @brief Queue neutralize and detach for one logical slot.
     *
     * @param controller_id Bridge controller slot.
     */
    void detach(std::uint8_t controller_id);

    /**
     * @brief Count occupied pending latest-state slots.
     *
     * This test and diagnostics accessor demonstrates that state bursts cannot
     * create an unbounded queue.
     *
     * @return Number of controller slots holding a pending state.
     */
    std::size_t pending_state_count() const;

    /**
     * @brief Copy the current connection, protocol, error, and controller diagnostics.
     *
     * @return Self-contained diagnostic snapshot safe to inspect from any thread.
     */
    client_diagnostics_t diagnostics() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Worker, transport, and synchronized desired state.
  };
}  // namespace input::nxbt
