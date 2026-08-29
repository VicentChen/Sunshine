/**
 * @file src/input/gamepad_router.h
 * @brief Routable gamepad output sinks independent of the input protocol parser.
 */
#pragma once

// standard includes
#include <chrono>
#include <memory>
#include <optional>

// local includes
#include "src/platform/common.h"

namespace input::gamepad {
  /**
   * @brief Selects the gamepad output targets for one logical controller.
   */
  enum class output_mode_e {
    disabled,  ///< Accept controller state without forwarding it to an output backend.
    virtual_output,  ///< Send input only to the host virtual-HID backend.
    nxbt,  ///< Send input only to the NXBT Bridge backend.
    both,  ///< Send input to virtual-HID first and NXBT Bridge second.
  };

  /**
   * @brief Rate-limit repeated gamepad output failure logs using monotonic time.
   */
  class failure_log_limiter_t {
  public:
    /**
     * @brief Create a limiter with the minimum interval between permitted logs.
     *
     * @param interval Minimum monotonic interval between log entries.
     */
    explicit failure_log_limiter_t(std::chrono::steady_clock::duration interval);

    /**
     * @brief Decide whether a failure should be logged at the supplied time.
     *
     * The first failure is always permitted. A permitted call advances the
     * limiter timestamp, while a suppressed call leaves it unchanged.
     *
     * @param now Current monotonic time.
     * @return @c true when the caller should emit a log entry.
     */
    bool should_log(std::chrono::steady_clock::time_point now);

  private:
    std::chrono::steady_clock::duration interval_;  ///< Minimum interval between permitted logs.
    std::optional<std::chrono::steady_clock::time_point> last_log_;  ///< Most recent permitted log time.
  };

  /**
   * @brief Output implementation for a logical Sunshine gamepad.
   */
  class sink_t {
  public:
    /**
     * @brief Destroy the gamepad output sink.
     */
    virtual ~sink_t() = default;

    /**
     * @brief Allocate output resources for one global gamepad slot.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @param arrival Client-reported controller metadata.
     * @param feedback_queue Feedback queue returned to the Moonlight client.
     * @return @c true when the sink allocated the controller.
     */
    virtual bool alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue) = 0;

    /**
     * @brief Rebind retained controller output to a resumed Moonlight session.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @param feedback_queue Feedback queue returned to the resumed client.
     * @return @c true when the sink rebound the controller.
     */
    virtual bool rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue) = 0;

    /**
     * @brief Submit one complete controller snapshot without blocking the input parser.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @param state Complete Moonlight controller state.
     * @return @c true when the sink accepted the snapshot.
     */
    virtual bool update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state) = 0;

    /**
     * @brief Release all pressed inputs while retaining the output allocation.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     */
    virtual void neutralize(const platf::gamepad_id_t &id) = 0;

    /**
     * @brief Release all output resources for one logical controller.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     */
    virtual void free(const platf::gamepad_id_t &id) = 0;
  };

  /**
   * @brief Routes one logical controller to virtual-HID, NXBT, or both sinks.
   */
  class router_t {
  public:
    /**
     * @brief Create a router with optional virtual-HID and NXBT output sinks.
     *
     * @param mode Selected output mode.
     * @param virtual_sink Host virtual-HID output sink.
     * @param nxbt_sink NXBT Bridge output sink.
     */
    router_t(output_mode_e mode, std::shared_ptr<sink_t> virtual_sink, std::shared_ptr<sink_t> nxbt_sink);

    /**
     * @brief Return the output mode selected for this router.
     *
     * @return Immutable output mode.
     */
    output_mode_e mode() const;

    /**
     * @brief Allocate all selected outputs atomically.
     *
     * If a later selected sink fails allocation, every previously allocated
     * sink is freed in reverse order before this function returns failure.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @param arrival Client-reported controller metadata.
     * @param feedback_queue Feedback queue returned to the Moonlight client.
     * @return @c true when every selected sink allocated the controller.
     */
    bool alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue);

    /**
     * @brief Rebind every selected sink to a resumed Moonlight session.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @param feedback_queue Feedback queue returned to the resumed client.
     * @return @c true when every selected sink rebound successfully.
     */
    bool rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue);

    /**
     * @brief Deliver a snapshot to every selected sink.
     *
     * A temporary failure from one sink does not prevent another selected sink
     * from receiving the state.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @param state Complete Moonlight controller state.
     * @return @c true when every selected sink accepted the state.
     */
    bool update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state);

    /**
     * @brief Neutralize every selected sink in deterministic output order.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     */
    void neutralize(const platf::gamepad_id_t &id);

    /**
     * @brief Free every selected sink in reverse output order.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     */
    void free(const platf::gamepad_id_t &id);

  private:
    /**
     * @brief Check whether an identifier references a valid global gamepad slot.
     *
     * @param id Sunshine gamepad identifier.
     * @return @c true when the global slot is within Sunshine's supported range.
     */
    static bool valid(const platf::gamepad_id_t &id);

    output_mode_e mode_;  ///< Selected output mode.
    std::shared_ptr<sink_t> virtual_sink_;  ///< Optional host virtual-HID sink.
    std::shared_ptr<sink_t> nxbt_sink_;  ///< Optional NXBT Bridge sink.
  };
}  // namespace input::gamepad
