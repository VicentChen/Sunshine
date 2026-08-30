/**
 * @file src/xbox_remote/worker.h
 * @brief Cancellable background worker for Sunshine Xbox Remote Play input.
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// local includes
#include "src/input/xbox_remote_sink.h"
#include "src/xbox_remote/input_queue.h"

namespace xbox_remote::worker {
  /**
   * @brief User-visible lifecycle state of the production Remote Play worker.
   */
  enum class state_e {
    idle,  ///< No application requested a session.
    starting,  ///< A background connection is being created.
    ready,  ///< The connection can send queued controller operations.
    stopping,  ///< Cancellation and deterministic cleanup are in progress.
    failed,  ///< The last connection attempt ended with a sanitized failure.
  };

  /**
   * @brief Return a stable non-sensitive name for a worker state.
   *
   * @param state Worker state.
   * @return Fixed state name suitable for logs and status APIs.
   */
  std::string_view state_name(state_e state);

  /**
   * @brief Recovery policy attached to a sanitized connection failure.
   */
  enum class failure_kind_e {
    retryable,  ///< Retry with bounded exponential backoff.
    reauthentication_required,  ///< Stop until the operator refreshes Microsoft credentials.
    permanent,  ///< Stop because configuration or protocol input is invalid.
  };

  /**
   * @brief Return a stable name for a recovery policy.
   *
   * @param kind Recovery policy.
   * @return Fixed non-sensitive policy name.
   */
  std::string_view failure_kind_name(failure_kind_e kind);

  /**
   * @brief Result of opening or polling a Remote Play connection.
   */
  struct result_t {
    bool ok = true;  ///< Whether the operation succeeded.
    std::string stage;  ///< Fixed non-sensitive failure stage.
    failure_kind_e failure_kind = failure_kind_e::retryable;  ///< Recovery policy when @c ok is false.
  };

  /**
   * @brief One connected Remote Play transport owned only by the worker thread.
   */
  class connection_t {
  public:
    /**
     * @brief Callback receiving fixed non-sensitive connection stages.
     */
    using progress_handler_t = std::function<void(std::string_view)>;

    /**
     * @brief Destroy the connection after worker-thread cleanup.
     */
    virtual ~connection_t() = default;

    /**
     * @brief Complete authentication, discovery, WakeUp, signaling, and startup.
     *
     * @param cancelled Callback checked during every bounded wait.
     * @return Success or a fixed sanitized failure stage.
     */
    virtual result_t open(const std::function<bool()> &cancelled) = 0;

    /**
     * @brief Register a sanitized connection-stage observer.
     *
     * The default implementation permits simple fake connections to omit progress.
     *
     * @param handler Stage callback owned by the worker.
     */
    virtual void set_progress_handler(progress_handler_t handler) {
      static_cast<void>(handler);
    }

    /**
     * @brief Send one queued controller operation without waiting for network readiness.
     *
     * @param item Controller attach, state, neutralize, or detach operation.
     * @return @c true when the connected transport accepted the operation.
     */
    virtual bool send(const input::item_t &item) = 0;

    /**
     * @brief Perform one bounded keepalive/inbound-feedback iteration.
     *
     * @param cancelled Callback checked during the iteration.
     * @param vibration Receives one parsed feedback command when available.
     * @return Success or a fixed sanitized failure stage.
     */
    virtual result_t poll(
      const std::function<bool()> &cancelled,
      std::optional<protocol::vibration_t> &vibration
    ) = 0;

    /**
     * @brief Release all remote resources after the worker sends final cleanup operations.
     *
     * @return Success or a fixed sanitized cleanup failure stage.
     */
    virtual result_t close() = 0;
  };

  /**
   * @brief Factory creating a fresh connection for each application start.
   */
  using connection_factory_t = std::function<std::unique_ptr<connection_t>()>;

  /**
   * @brief Controls worker cadence and bounded queue behavior.
   */
  struct options_t {
    std::chrono::milliseconds poll_interval {16};  ///< Maximum ready-loop sleep interval.
    std::chrono::milliseconds watchdog_timeout {2000};  ///< Idle Moonlight input deadline before a neutral snapshot.
    std::chrono::milliseconds reconnect_initial_backoff {250};  ///< First retry delay.
    std::chrono::milliseconds reconnect_max_backoff {4000};  ///< Maximum retry delay.
    std::size_t maximum_reconnect_attempts = 3;  ///< Retry attempts after the initial connection.
    input::queue_options_t queue;  ///< Bounded input queue capacities and edge lifetime.
  };

  /**
   * @brief Background Xbox session implementing the Sunshine sink interface.
   */
  class session_t final: public ::input::xbox_remote::session_t {
  public:
    /**
     * @brief Construct an idle worker.
     *
     * @param factory Factory for one application-scoped connection.
     * @param options Worker cadence and queue controls.
     */
    explicit session_t(connection_factory_t factory, options_t options = {});

    /**
     * @brief Stop and join the background worker.
     */
    ~session_t() override;

    session_t(const session_t &) = delete;
    session_t &operator=(const session_t &) = delete;

    /**
     * @brief Start one application-scoped connection asynchronously.
     *
     * @return @c true when a new worker was started.
     */
    bool start();

    /**
     * @brief Cancel, clean up, and join the active connection.
     */
    void stop();

    /**
     * @brief Return the current worker state.
     *
     * @return Consistent state snapshot.
     */
    state_e state() const;

    /**
     * @brief Return the last fixed failure stage.
     *
     * @return Empty string unless the worker is failed.
     */
    std::string failure_stage() const;

    /**
     * @brief Return the current sanitized connection stage.
     *
     * @return Fixed stage name suitable for logs and status APIs.
     */
    std::string stage() const;

    /**
     * @brief Return the recovery policy for the last terminal failure.
     *
     * @return Stable recovery policy name, empty outside failed state.
     */
    std::string failure_kind() const;

    /**
     * @brief Return the monotonically increasing connection epoch.
     *
     * @return Zero before the first connection, otherwise the current epoch.
     */
    std::uint64_t epoch() const;

    /**
     * @copydoc input::xbox_remote::session_t::set_vibration_handler
     */
    void set_vibration_handler(vibration_handler_t handler) override;

    /**
     * @copydoc input::xbox_remote::session_t::attach
     */
    bool attach(const protocol::gamepad_frame_t &frame) override;

    /**
     * @copydoc input::xbox_remote::session_t::rebind
     */
    bool rebind() override;

    /**
     * @copydoc input::xbox_remote::session_t::submit
     */
    bool submit(const protocol::gamepad_frame_t &frame) override;

    /**
     * @copydoc input::xbox_remote::session_t::neutralize
     */
    bool neutralize() override;

    /**
     * @copydoc input::xbox_remote::session_t::detach
     */
    void detach() override;

  private:
    /**
     * @brief Own one connection lifecycle on the background thread.
     */
    void run();

    /**
     * @brief Check cancellation under the worker mutex.
     *
     * @return @c true after stop was requested.
     */
    bool cancelled() const;

    connection_factory_t factory_;  ///< Creates application-scoped connections.
    options_t options_;  ///< Worker cadence and queue controls.
    mutable std::mutex mutex_;  ///< Protects lifecycle, queue, and callback state.
    std::condition_variable changed_;  ///< Wakes the worker for input or cancellation.
    input::outbound_queue_t queue_;  ///< Bounded latest-state/control queue.
    vibration_handler_t vibration_handler_;  ///< Current sink feedback consumer.
    std::thread thread_;  ///< Application-scoped connection worker.
    state_e state_ = state_e::idle;  ///< Current lifecycle state.
    std::string failure_stage_;  ///< Fixed last failure stage.
    std::string stage_;  ///< Fixed current connection stage.
    std::string failure_kind_;  ///< Fixed terminal recovery policy name.
    std::chrono::steady_clock::time_point last_input_ {};  ///< Last complete Moonlight state submission.
    bool stop_requested_ = false;  ///< Cancellation flag observed by blocking stages.
    bool watchdog_armed_ = false;  ///< Whether a non-neutral state can expire.
    std::uint64_t epoch_ = 0;  ///< Monotonic connection generation.
  };
}  // namespace xbox_remote::worker
