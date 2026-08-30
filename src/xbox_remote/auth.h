/**
 * @file src/xbox_remote/auth.h
 * @brief Injectable Microsoft and Xbox Home Remote Play authentication chain.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

// local includes
#include "src/xbox_remote/token_store.h"

namespace xbox_remote::auth {
  /**
   * @brief HTTP response returned to the authentication state machine.
   */
  struct http_response_t {
    std::uint32_t status_code = 0;  ///< HTTP status, or zero before a response exists.
    std::string body;  ///< Response body consumed only by strict parsers.
    bool network_error = false;  ///< Whether transport failed before a valid response.
  };

  /**
   * @brief Injectable deadline-aware HTTP client.
   */
  class http_client_t {
  public:
    /**
     * @brief Destroy the HTTP client.
     */
    virtual ~http_client_t() = default;

    /**
     * @brief Send one HTTP POST request.
     *
     * @param url HTTPS endpoint.
     * @param body Request body.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @return Status, body, and sanitized transport outcome.
     */
    virtual http_response_t post(
      std::string_view url,
      std::string_view body,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) = 0;

    /**
     * @brief Send one HTTP GET request.
     *
     * @param url HTTPS endpoint.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @return Status, body, and sanitized transport outcome.
     */
    virtual http_response_t get(
      std::string_view url,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) {
      static_cast<void>(url);
      static_cast<void>(headers);
      static_cast<void>(timeout);
      return {0, {}, true};
    }

    /**
     * @brief Send one HTTP DELETE request.
     *
     * @param url HTTPS endpoint.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @return Status, body, and sanitized transport outcome.
     */
    virtual http_response_t remove(
      std::string_view url,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) {
      static_cast<void>(url);
      static_cast<void>(headers);
      static_cast<void>(timeout);
      return {0, {}, true};
    }
  };

  /**
   * @brief Injectable wall/monotonic clock and cancellable wait provider.
   */
  class runtime_t {
  public:
    /**
     * @brief Destroy the runtime provider.
     */
    virtual ~runtime_t() = default;

    /**
     * @brief Return the current wall-clock time for token expiry.
     *
     * @return Current wall-clock time.
     */
    virtual std::chrono::system_clock::time_point system_now() const = 0;

    /**
     * @brief Return the current monotonic time for polling deadlines.
     *
     * @return Current monotonic time.
     */
    virtual std::chrono::steady_clock::time_point steady_now() const = 0;

    /**
     * @brief Wait for a polling interval while observing cancellation.
     *
     * @param duration Requested wait.
     * @param cancelled Callback returning @c true when work must stop.
     * @return @c true after the duration, or @c false when cancelled.
     */
    virtual bool wait_for(std::chrono::seconds duration, const std::function<bool()> &cancelled) = 0;
  };

  /**
   * @brief Authentication error categories safe to surface to a caller.
   */
  enum class auth_error_e {
    none,  ///< Authentication succeeded.
    cancelled,  ///< Caller cancelled the operation.
    timeout,  ///< Device Code or request budget expired.
    network_error,  ///< HTTP transport failed.
    http_error,  ///< Service returned an unexpected non-success status.
    invalid_response,  ///< A success response was malformed or incomplete.
    authorization_declined,  ///< User declined the Microsoft sign-in request.
    code_expired,  ///< Microsoft reported an expired Device Code.
  };

  /**
   * @brief Sanitized authentication failure.
   */
  struct auth_failure_t {
    auth_error_e code = auth_error_e::none;  ///< Machine-readable category.
    std::string stage;  ///< Fixed authentication stage name.
    std::uint32_t http_status = 0;  ///< HTTP status without response contents.
    std::string message;  ///< Fixed diagnostic that never embeds credentials.
  };

  /**
   * @brief Result returned by authentication operations.
   *
   * @tparam T Successful value type.
   */
  template<typename T>
  struct auth_result_t {
    T value {};  ///< Successful value.
    auth_failure_t error {};  ///< Sanitized failure.

    /**
     * @brief Check whether the operation succeeded.
     *
     * @return @c true when @c error.code is @c auth_error_e::none.
     */
    explicit operator bool() const {
      return error.code == auth_error_e::none;
    }
  };

  /**
   * @brief Microsoft Device Code prompt and private polling state.
   */
  struct device_code_t {
    std::string device_code;  ///< Private code submitted only to Microsoft token polling.
    std::string user_code;  ///< Short code shown to the user.
    std::string verification_uri;  ///< Microsoft URL shown to the user.
    std::chrono::seconds polling_interval {5};  ///< Initial service-requested poll interval.
    std::chrono::seconds expires_in {900};  ///< Monotonic validity duration.
    std::chrono::steady_clock::time_point issued_at {};  ///< Injectable monotonic issue time.
  };

  /**
   * @brief In-memory Xbox Home streaming authorization context.
   */
  struct session_context_t {
    std::string user_hash;  ///< Xbox user hash used in XBL3.0 authorization.
    std::string web_token;  ///< XSTS token used for Home console discovery.
    std::string gs_token;  ///< xHome GSSV bearer token.
    std::string base_uri;  ///< Default xHome GSSV region URI.
    std::chrono::system_clock::time_point expires_at {};  ///< GSSV expiry derived at receipt.
  };

  /**
   * @brief Complete successful authentication result.
   */
  struct authenticated_t {
    oauth_credentials_t oauth;  ///< OAuth credentials eligible for secure persistence.
    session_context_t session;  ///< Derived Xbox tokens retained only in memory.
  };

  /**
   * @brief Microsoft Device Code, refresh, Xbox User Token, XSTS, and GSSV client.
   */
  class client_t {
  public:
    /**
     * @brief Construct an authentication client with injected dependencies.
     *
     * @param http Deadline-aware HTTP implementation.
     * @param runtime Injectable clocks and cancellable waits.
     */
    client_t(http_client_t &http, runtime_t &runtime);

    /**
     * @brief Request a new Microsoft Device Code.
     *
     * @return User prompt and private polling state, or a sanitized failure.
     */
    auth_result_t<device_code_t> begin_device_code();

    /**
     * @brief Poll a Device Code and derive the complete Xbox Home token chain.
     *
     * @param device_code State returned by @c begin_device_code().
     * @param cancelled Callback returning @c true when work must stop.
     * @return OAuth credentials and in-memory Xbox session context.
     */
    auth_result_t<authenticated_t> complete_device_code(
      const device_code_t &device_code,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Resume from persisted OAuth credentials, refreshing when required.
     *
     * @param credentials Persisted OAuth credentials.
     * @param cancelled Callback returning @c true when work must stop.
     * @return Refreshed OAuth credentials and in-memory Xbox session context.
     */
    auth_result_t<authenticated_t> resume(
      oauth_credentials_t credentials,
      const std::function<bool()> &cancelled
    );

  private:
    /**
     * @brief Send one authentication request with bounded transport retries.
     *
     * @param url HTTPS endpoint.
     * @param body Request body.
     * @param headers Request headers.
     * @param stage Fixed authentication stage name.
     * @param cancelled Cancellation callback.
     * @return HTTP response, or a sanitized transport/cancellation failure.
     */
    auth_result_t<http_response_t> request_with_retry(
      std::string_view url,
      std::string_view body,
      const std::map<std::string, std::string> &headers,
      std::string stage,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Exchange one OAuth response for the Xbox token chain.
     *
     * @param credentials Current OAuth credentials.
     * @param cancelled Cancellation callback.
     * @return Complete authentication result.
     */
    auth_result_t<authenticated_t> derive_xbox_tokens(
      oauth_credentials_t credentials,
      const std::function<bool()> &cancelled
    );

    http_client_t &http_;  ///< Injected HTTP implementation.
    runtime_t &runtime_;  ///< Injected clocks and wait provider.
  };
}  // namespace xbox_remote::auth
