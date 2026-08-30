/**
 * @file src/input/xbox_remote_sink.h
 * @brief Sunshine gamepad sink for a non-blocking Xbox Remote Play session.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

// local includes
#include "src/input/gamepad_router.h"
#include "src/xbox_remote/protocol.h"

namespace input::xbox_remote {
  /**
   * @brief Non-blocking input-facing surface of an Xbox Remote Play session.
   *
   * Implementations own all network, disk, timer, and WebRTC work on background
   * threads. Every method called by @ref sink_t must return after bounded in-memory
   * work and must not wait for the session to become ready.
   */
  class session_t {
  public:
    /**
     * @brief Callback receiving already validated Xbox vibration commands.
     */
    using vibration_handler_t = std::function<void(const ::xbox_remote::protocol::vibration_t &)>;

    /**
     * @brief Destroy the session input surface.
     */
    virtual ~session_t() = default;

    /**
     * @brief Replace the vibration callback used by the session worker.
     *
     * Passing an empty callback prevents future callback acquisition. A callback
     * already copied by the background worker may finish after this method
     * returns, so consumers must retain callback-safe state independently.
     *
     * @param handler New vibration callback, or an empty callback to unregister.
     */
    virtual void set_vibration_handler(vibration_handler_t handler) = 0;

    /**
     * @brief Register the first-version Xbox gamepad and its initial state.
     *
     * @param frame Initial complete Xbox state for logical gamepad zero.
     * @return @c true when the background session accepted the registration.
     */
    virtual bool attach(const ::xbox_remote::protocol::gamepad_frame_t &frame) = 0;

    /**
     * @brief Notify the session that the Moonlight feedback consumer resumed.
     *
     * @return @c true when the session accepted the rebind notification.
     */
    virtual bool rebind() = 0;

    /**
     * @brief Submit one complete Xbox gamepad state to the bounded session queue.
     *
     * @param frame Complete absolute gamepad state.
     * @return @c true when the state was accepted, including while reconnecting.
     */
    virtual bool submit(const ::xbox_remote::protocol::gamepad_frame_t &frame) = 0;

    /**
     * @brief Queue a high-priority neutral state without detaching the gamepad.
     *
     * @return @c true when the neutral operation was accepted.
     */
    virtual bool neutralize() = 0;

    /**
     * @brief Queue logical gamepad removal during sink release.
     */
    virtual void detach() = 0;
  };

  /**
   * @brief Convert a complete Sunshine state to the Xbox first-gamepad wire model.
   *
   * @param state Complete Moonlight controller state parsed by Sunshine.
   * @return Complete Xbox gamepad-zero frame with explicit activity masks.
   */
  ::xbox_remote::protocol::gamepad_frame_t map_state(const platf::gamepad_state_t &state);

  /**
   * @brief Map Sunshine gamepad lifecycle and feedback to an Xbox session.
   *
   * The first version permits one allocated Sunshine controller at a time and
   * maps it to Xbox gamepad index zero. The supplied session owns the bounded
   * queue and all asynchronous transport work.
   */
  class sink_t final: public gamepad::sink_t {
  public:
    /**
     * @brief Bind the sink to a shared non-blocking Xbox session.
     *
     * @param session Session input surface that outlives individual controllers.
     */
    explicit sink_t(std::shared_ptr<session_t> session);

    /**
     * @brief Unregister vibration delivery and destroy the sink.
     */
    ~sink_t() override;

    sink_t(const sink_t &) = delete;
    sink_t &operator=(const sink_t &) = delete;

    /**
     * @copydoc gamepad::sink_t::alloc
     */
    bool alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue) override;

    /**
     * @copydoc gamepad::sink_t::rebind
     */
    bool rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue) override;

    /**
     * @copydoc gamepad::sink_t::update
     */
    bool update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state) override;

    /**
     * @copydoc gamepad::sink_t::neutralize
     */
    void neutralize(const platf::gamepad_id_t &id) override;

    /**
     * @copydoc gamepad::sink_t::free
     */
    void free(const platf::gamepad_id_t &id) override;

  private:
    /**
     * @brief Callback-safe Moonlight feedback binding.
     */
    struct feedback_state_t {
      std::mutex mutex;  ///< Protects the active client binding.
      std::optional<int> global_index;  ///< Sunshine slot currently mapped to Xbox gamepad zero.
      std::uint8_t client_relative_index = 0;  ///< Moonlight controller identifier for feedback.
      platf::feedback_queue_t queue;  ///< Current Moonlight feedback destination.
    };

    /**
     * @brief Deliver parsed Xbox vibration to a live Moonlight feedback binding.
     *
     * @param weak_state Weak callback-safe feedback state.
     * @param vibration Parsed Xbox four-motor vibration command.
     */
    static void deliver_vibration(
      const std::weak_ptr<feedback_state_t> &weak_state,
      const ::xbox_remote::protocol::vibration_t &vibration
    );

    /**
     * @brief Validate that an identifier owns the allocated Xbox slot.
     *
     * @param id Sunshine gamepad identifier.
     * @return @c true when the identifier owns Xbox gamepad zero.
     */
    bool owns_slot(const platf::gamepad_id_t &id) const;

    std::shared_ptr<session_t> session_;  ///< Non-blocking background session surface.
    std::shared_ptr<feedback_state_t> feedback_;  ///< Callback-safe feedback binding.
  };
}  // namespace input::xbox_remote
