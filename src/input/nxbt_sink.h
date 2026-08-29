/**
 * @file src/input/nxbt_sink.h
 * @brief Gamepad output sink that maps Sunshine input to the NXBT IPC client.
 */
#pragma once

// standard includes
#include <array>
#include <memory>
#include <mutex>
#include <optional>

// local includes
#include "src/input/gamepad_router.h"
#include "src/input/nxbt_client.h"
#include "src/input/nxbt_mapping.h"

namespace input::nxbt {
  /**
   * @brief Map Sunshine gamepad snapshots and submit them to an NXBT client.
   *
   * The sink owns per-slot trigger hysteresis and sequence state. The supplied
   * client owns the IPC worker and outlives all calls made by this sink.
   */
  class sink_t final: public gamepad::sink_t {
  public:
    /**
     * @brief Create an NXBT sink.
     *
     * @param client Shared reconnecting IPC client.
     * @param face_button_policy Face-button label or position policy.
     * @param fixed_controller_slot Optional single Bridge slot used for the first allocated Sunshine gamepad.
     * @param trigger_press_threshold Value at or above which ZL/ZR become pressed.
     * @param trigger_release_threshold Value at or below which ZL/ZR become released.
     */
    sink_t(
      std::shared_ptr<client_t> client,
      face_button_policy_e face_button_policy,
      std::optional<std::uint8_t> fixed_controller_slot = std::nullopt,
      std::uint8_t trigger_press_threshold = 64,
      std::uint8_t trigger_release_threshold = 48
    );

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
     * @brief Per-controller conversion state protected from lifecycle races.
     */
    struct slot_t {
      bool allocated = false;  ///< Whether the logical slot is allocated.
      trigger_state_t triggers;  ///< Persistent ZL/ZR hysteresis state.
      std::uint32_t sequence = 0;  ///< Most recently issued state sequence.
    };

    std::shared_ptr<client_t> client_;  ///< Non-blocking IPC client.
    face_button_policy_e face_button_policy_;  ///< Configured face-button mapping.
    std::optional<std::uint8_t> fixed_controller_slot_;  ///< Optional first-version single Bridge slot.
    std::optional<int> fixed_sunshine_slot_;  ///< Sunshine slot currently owning the configured fixed Bridge slot.
    std::uint8_t trigger_press_threshold_;  ///< Configured digital-trigger press threshold.
    std::uint8_t trigger_release_threshold_;  ///< Configured digital-trigger release threshold.
    std::mutex mutex_;  ///< Protects per-slot mapping and lifecycle state.
    std::array<slot_t, platf::MAX_GAMEPADS> slots_ {};  ///< Bounded per-controller state.
  };
}  // namespace input::nxbt
