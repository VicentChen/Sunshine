/**
 * @file src/xbox_remote/session.h
 * @brief Xbox Home console discovery and REST session lifecycle.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/xbox_remote/auth.h"
#include "src/xbox_remote/protocol.h"

namespace xbox_remote::session {
  /**
   * @brief Sanitized Home REST failure categories.
   */
  enum class error_e {
    none,  ///< Operation succeeded.
    cancelled,  ///< Caller cancelled the operation.
    timeout,  ///< Provisioning or exchange polling timed out.
    network_error,  ///< HTTPS transport failed.
    unauthorized,  ///< Authentication was rejected after at most one refresh.
    not_found,  ///< Requested console or service resource does not exist.
    duplicate_console,  ///< Stable console selection matched more than once.
    ambiguous_console,  ///< Automatic selection found more than one Home console.
    rate_limited,  ///< Xbox service returned HTTP 429.
    server_error,  ///< Xbox service returned HTTP 5xx.
    http_error,  ///< Xbox service returned another unexpected status.
    invalid_response,  ///< Response JSON or a caller identifier was invalid.
    failed_state,  ///< Home provisioning entered a terminal failure state.
  };

  /**
   * @brief Caller-visible failure without response bodies or credentials.
   */
  struct failure_t {
    error_e code = error_e::none;  ///< Machine-readable category.
    std::string stage;  ///< Fixed REST stage name.
    std::uint32_t http_status = 0;  ///< HTTP status without response contents.
    std::string message;  ///< Fixed diagnostic safe for logs and UI.
  };

  /**
   * @brief Result returned by Home REST operations.
   *
   * @tparam T Successful value type.
   */
  template<typename T>
  struct result_t {
    T value {};  ///< Successful value.
    failure_t error {};  ///< Sanitized failure.
    std::uint32_t http_status = 0;  ///< Final successful HTTP status, or zero when no request was needed.

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
   * @brief Home session provisioning controls.
   */
  struct options_t {
    std::uint32_t width = 1280;  ///< Client display width in pixels.
    std::uint32_t height = 720;  ///< Client display height in pixels.
    std::string locale = "en-US";  ///< Xbox service locale.
    std::string os_name = "android";  ///< Xbox compatibility OS marker.
    std::chrono::seconds poll_interval {1};  ///< State polling cadence.
    std::chrono::seconds provision_timeout {30};  ///< Maximum Home provisioning time.
  };

  /**
   * @brief Xbox remote-management wake controls.
   */
  struct wake_options_t {
    std::string command_session_id;  ///< Caller-generated UUID for the XCCS command envelope.
    std::chrono::seconds poll_interval {1};  ///< Console power-state polling cadence.
    std::chrono::seconds wake_timeout {45};  ///< Maximum wait for the console to report On.
  };

  /**
   * @brief Provisioned Home session and keepalive schedule.
   */
  struct provisioned_t {
    std::string session_id;  ///< Server-assigned opaque session identifier.
    protocol::session_configuration_t configuration;  ///< Parsed transport configuration.
    std::chrono::steady_clock::time_point next_keepalive {};  ///< Next monotonic keepalive deadline.
    std::uint32_t create_http_status = 0;  ///< Session creation HTTP status.
    std::uint32_t state_http_status = 0;  ///< Final Provisioned state HTTP status.
    std::uint32_t configuration_http_status = 0;  ///< Configuration HTTP status.
  };

  /**
   * @brief Callback that refreshes the in-memory Xbox session context.
   */
  using refresh_callback_t = std::function<bool(auth::session_context_t &)>;

  /**
   * @brief Xbox Home discovery, provisioning, signaling, and cleanup client.
   */
  class client_t {
  public:
    /**
     * @brief Construct a Home REST client.
     *
     * @param http Deadline-aware HTTP transport.
     * @param runtime Injectable monotonic clock and waits.
     * @param context In-memory Xbox authorization context.
     * @param refresh Optional one-shot refresh callback used after HTTP 401.
     */
    client_t(
      auth::http_client_t &http,
      auth::runtime_t &runtime,
      auth::session_context_t &context,
      refresh_callback_t refresh = {}
    );

    /**
     * @brief Discover Home consoles associated with the account.
     *
     * @param cancelled Cancellation callback.
     * @return Parsed console list.
     */
    result_t<std::vector<protocol::console_t>> discover(const std::function<bool()> &cancelled);

    /**
     * @brief Select one console by stable identifier or unique discovery.
     *
     * @param consoles Discovered consoles.
     * @param server_id Exact stable identifier, or empty to select the only discovered console.
     * @return Selected console, not-found, duplicate, or ambiguous failure.
     */
    result_t<protocol::console_t> select_console(
      const std::vector<protocol::console_t> &consoles,
      std::string_view server_id
    ) const;

    /**
     * @brief Wake a standby console through XCCS and wait for the On state.
     *
     * A console already reporting On succeeds without sending a command. A
     * network failure after the non-repeatable WakeUp POST is treated as
     * ambiguous, so the method polls power state without replaying the command.
     *
     * @param console Selected Home console and its current power state.
     * @param options Wake command identifier, cadence, and deadline.
     * @param cancelled Cancellation callback.
     * @return Updated console once On, or a sanitized failure.
     */
    result_t<protocol::console_t> wake_and_wait(
      const protocol::console_t &console,
      const wake_options_t &options,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Create a Home session, wait for Provisioned, and fetch configuration.
     *
     * Any failure after creation attempts an idempotent DELETE without honoring
     * the original cancellation flag.
     *
     * @param server_id Selected console identifier.
     * @param options Provisioning parameters.
     * @param cancelled Cancellation callback.
     * @return Provisioned session or sanitized failure.
     */
    result_t<provisioned_t> create_and_wait(
      std::string_view server_id,
      const options_t &options,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Submit a local SDP offer.
     *
     * @param session_id Active session identifier.
     * @param offer SDP offer.
     * @param cancelled Cancellation callback.
     * @return Success flag or sanitized failure.
     */
    result_t<bool> send_sdp(
      std::string_view session_id,
      const protocol::sdp_offer_t &offer,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Poll the remote SDP exchange response.
     *
     * @param session_id Active session identifier.
     * @param cancelled Cancellation callback.
     * @return Empty optional for HTTP 204, otherwise the encoded exchange response.
     */
    result_t<std::optional<std::string>> poll_sdp(
      std::string_view session_id,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Submit local ICE candidates.
     *
     * @param session_id Active session identifier.
     * @param candidates Local candidates.
     * @param cancelled Cancellation callback.
     * @return Success flag or sanitized failure.
     */
    result_t<bool> send_ice(
      std::string_view session_id,
      const std::vector<protocol::ice_candidate_t> &candidates,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Poll the remote ICE exchange response.
     *
     * @param session_id Active session identifier.
     * @param cancelled Cancellation callback.
     * @return Empty optional for HTTP 204, otherwise the encoded exchange response.
     */
    result_t<std::optional<std::string>> poll_ice(
      std::string_view session_id,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Send a keepalive only after its monotonic deadline.
     *
     * @param session Provisioned session whose deadline is updated on success.
     * @param cancelled Cancellation callback.
     * @return @c true when sent, @c false when not yet due, or a failure.
     */
    result_t<bool> keepalive_if_due(
      provisioned_t &session,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Idempotently delete a Home session.
     *
     * Empty identifiers and HTTP 404/410 are treated as already deleted.
     *
     * @param session_id Session identifier.
     * @param cancelled Cancellation callback.
     * @return Success flag or sanitized failure.
     */
    result_t<bool> delete_session(
      std::string_view session_id,
      const std::function<bool()> &cancelled = {}
    );

  private:
    /**
     * @brief Internal HTTP method selector.
     */
    enum class method_e {
      get,  ///< HTTP GET.
      post,  ///< HTTP POST.
      remove,  ///< HTTP DELETE.
    };

    /**
     * @brief Send an authorized request with one optional 401 refresh.
     *
     * @param method HTTP method.
     * @param path Absolute path relative to the current GSSV base URI.
     * @param body Optional request body.
     * @param headers Additional headers.
     * @param stage Fixed stage name.
     * @param cancelled Cancellation callback.
     * @return Raw HTTP response or transport/authentication failure.
     */
    result_t<auth::http_response_t> request(
      method_e method,
      std::string url,
      std::string body,
      std::map<std::string, std::string> headers,
      std::string stage,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Poll a signaling exchange endpoint.
     *
     * @param session_id Active session identifier.
     * @param endpoint Endpoint suffix.
     * @param stage Fixed stage name.
     * @param cancelled Cancellation callback.
     * @return Empty optional for 204, otherwise encoded exchange response.
     */
    result_t<std::optional<std::string>> poll_exchange(
      std::string_view session_id,
      std::string_view endpoint,
      std::string stage,
      const std::function<bool()> &cancelled
    );

    auth::http_client_t &http_;  ///< Injected HTTP transport.
    auth::runtime_t &runtime_;  ///< Injected clocks and waits.
    auth::session_context_t &context_;  ///< Mutable in-memory authorization context.
    refresh_callback_t refresh_;  ///< Optional refresh callback.
  };
}  // namespace xbox_remote::session
