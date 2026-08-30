/**
 * @file src/xbox_remote/http_runtime.h
 * @brief Production HTTPS and clock adapters for Xbox Remote Play authentication.
 */
#pragma once

// standard includes
#include <chrono>
#include <functional>

// local includes
#include "src/xbox_remote/auth.h"

namespace xbox_remote::auth {
  /**
   * @brief libcurl-backed HTTPS client with bounded response storage.
   */
  class curl_http_client_t: public http_client_t {
  public:
    /**
     * @brief Set the callback used to abort an in-flight libcurl transfer.
     *
     * The callback is configured and consumed by the single Remote Play worker
     * thread; callers must not mutate it concurrently with a request.
     *
     * @param cancelled Callback returning @c true when the transfer must stop.
     */
    void set_cancellation_callback(std::function<bool()> cancelled);

    /**
     * @brief Send a deadline-bounded HTTPS POST request.
     *
     * @param url HTTPS endpoint.
     * @param body Request body.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @return HTTP status/body, or a sanitized transport failure.
     */
    http_response_t post(
      std::string_view url,
      std::string_view body,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override;

    /**
     * @brief Send a deadline-bounded HTTPS GET request.
     *
     * @param url HTTPS endpoint.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @return HTTP status/body, or a sanitized transport failure.
     */
    http_response_t get(
      std::string_view url,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override;

    /**
     * @brief Send a deadline-bounded HTTPS DELETE request.
     *
     * @param url HTTPS endpoint.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @return HTTP status/body, or a sanitized transport failure.
     */
    http_response_t remove(
      std::string_view url,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override;

  private:
    std::function<bool()> cancelled_;  ///< Worker cancellation sampled by libcurl progress callbacks.
  };

  /**
   * @brief Production wall clock, monotonic clock, and bounded cancellation wait.
   */
  class system_runtime_t: public runtime_t {
  public:
    /**
     * @brief Return the current system wall clock.
     *
     * @return Current wall-clock time.
     */
    std::chrono::system_clock::time_point system_now() const override;

    /**
     * @brief Return the current monotonic clock.
     *
     * @return Current monotonic time.
     */
    std::chrono::steady_clock::time_point steady_now() const override;

    /**
     * @brief Wait while checking cancellation at short bounded intervals.
     *
     * @param duration Requested wait duration.
     * @param cancelled Callback returning @c true when work must stop.
     * @return @c true after the duration, or @c false when cancelled.
     */
    bool wait_for(std::chrono::seconds duration, const std::function<bool()> &cancelled) override;
  };
}  // namespace xbox_remote::auth
