/**
 * @file src/platform/linux/ui_controller.h
 * @brief Thread-safe controller routing policy for the RKMPP Vulkan UI.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

// local includes
#include "../common.h"

namespace platf::ui {
  /** @brief One controller navigation action emitted on a digital edge. */
  enum class navigation_e {
    none,  ///< No navigation edge was emitted.
    up,  ///< Move focus upward.
    down,  ///< Move focus downward.
    left,  ///< Move focus left.
    right,  ///< Move focus right.
    confirm,  ///< Activate the focused item.
    back  ///< Return to the previous page.
  };

  /** @brief Complete controller state needed by the UI input policy. */
  struct controller_input_t {
    std::uint32_t buttons {};  ///< Moonlight gamepad button mask.
    std::int16_t left_stick_x {};  ///< Left stick horizontal position.
    std::int16_t left_stick_y {};  ///< Left stick vertical position; positive is up.
  };

  /** @brief Input-routing result for one controller packet. */
  struct decision_t {
    bool consume {};  ///< Prevent this packet from reaching the configured gamepad output.
    bool neutralize {};  ///< Send one neutral state to the configured gamepad output.
    bool visibility_changed {};  ///< The packet opened or closed the UI.
    bool visible {};  ///< UI visibility after processing the packet.
    navigation_e navigation {navigation_e::none};  ///< Navigation edge emitted by the owner.
  };

  /** @brief Immutable renderer-facing state published by the controller policy. */
  struct snapshot_t {
    bool visible {};  ///< Whether the panel should be composed into video frames.
    std::uint8_t focus {};  ///< Focused item in the three-column status page.
    std::uint64_t revision {1};  ///< Monotonic render revision.
  };

  /** @brief Detect the UI chord, own modal input, and publish navigation state. */
  class controller_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Monotonic clock used by the hold detector.

    /**
     * @brief Process one complete Moonlight controller state.
     *
     * @param controller Process-wide Sunshine gamepad slot.
     * @param input Complete buttons and left-stick state.
     * @param now Monotonic event time.
     * @return Routing, cleanup, visibility, and navigation decisions.
     */
    decision_t update(std::uint8_t controller, const controller_input_t &input, clock_t::time_point now);

    /**
     * @brief Advance a held chord without requiring repeated input packets.
     *
     * Moonlight sends state changes rather than periodic repeats, so the video
     * path calls this once per frame while a stream is active.
     *
     * @param now Monotonic time used by the hold detector.
     * @return A visibility transition when a continuously held chord expires.
     */
    decision_t tick(clock_t::time_point now);

    /**
     * @brief Remove one controller and close the UI when it owns the panel.
     *
     * @param controller Process-wide Sunshine gamepad slot being removed.
     * @return Cleanup and visibility decision for the removed controller.
     */
    decision_t disconnect(std::uint8_t controller);

    /** @brief Clear all controller state and hide the UI. */
    void reset();

    /** @brief Return one consistent renderer-facing state snapshot. */
    snapshot_t snapshot() const;

    /** @brief Return whether the modal UI is currently visible. */
    bool visible() const;

    /** @brief Return the process-wide gamepad slot that owns the UI. */
    std::optional<std::uint8_t> owner() const;

  private:
    /** @brief Per-controller edge, axis, and chord state. */
    struct input_state_t {
      std::optional<clock_t::time_point> chord_since;  ///< Start of the uninterrupted full chord.
      std::uint32_t previous_navigation {};  ///< Previous digitalized navigation mask.
      std::int8_t axis_x {};  ///< Hysteretic horizontal axis direction.
      std::int8_t axis_y {};  ///< Hysteretic vertical axis direction.
    };

    static constexpr std::uint32_t chord_ = platf::START | platf::BACK;  ///< Three-second visibility chord.
    static constexpr std::chrono::seconds hold_time_ {3};  ///< Required uninterrupted hold duration.
    static constexpr std::int16_t axis_press_ = 16000;  ///< Analog navigation press threshold.
    static constexpr std::int16_t axis_release_ = 8000;  ///< Analog navigation release threshold.
    static constexpr std::uint8_t item_count_ = 3;  ///< Items in the initial status page.

    /** @brief Convert one analog axis into a stable negative, neutral, or positive direction. */
    static std::int8_t axis_direction(std::int16_t value, std::int8_t previous) noexcept;

    /** @brief Convert buttons and left stick into a digital navigation mask. */
    static std::uint32_t navigation_mask(const controller_input_t &input, input_state_t &state) noexcept;

    mutable std::mutex mutex_;  ///< Serializes input updates and renderer snapshots.
    std::array<input_state_t, platf::MAX_GAMEPADS> inputs_;  ///< State indexed by process-wide gamepad slot.
    std::optional<std::uint8_t> owner_;  ///< Controller that opened and may navigate the UI.
    std::optional<std::uint8_t> release_controller_;  ///< Controller held behind the full-release gate.
    bool visible_ {};  ///< Current modal visibility.
    std::uint8_t focus_ {};  ///< Focused item in the initial status page.
    std::uint64_t revision_ {1};  ///< Monotonic renderer revision.
  };

  /** @brief Return the process-wide UI controller shared by input and video. */
  controller_t &global_controller();
}  // namespace platf::ui
