/**
 * @file src/input/nxbt_mapping.cpp
 * @brief Sunshine-to-NXBT controller mapping implementation.
 */

#include "src/input/nxbt_mapping.h"

// standard includes
#include <limits>

namespace input::nxbt {
  namespace {
    /**
     * @brief Test whether a Sunshine button bit is set.
     *
     * @param button_flags Sunshine button mask.
     * @param button Sunshine button bit.
     * @return @c true when @p button is pressed.
     */
    bool is_pressed(std::uint32_t button_flags, std::uint32_t button) {
      return (button_flags & button) != 0;
    }

    /**
     * @brief Update one digital trigger using the agreed hysteresis thresholds.
     *
     * @param value Sunshine trigger value.
     * @param pressed Previous and updated pressed state.
     * @param press_threshold Value that transitions to pressed.
     * @param release_threshold Value that transitions to released.
     */
    void update_trigger(std::uint8_t value, bool &pressed, std::uint8_t press_threshold, std::uint8_t release_threshold) {
      if (!pressed && value >= press_threshold) {
        pressed = true;
      } else if (pressed && value <= release_threshold) {
        pressed = false;
      }
    }
  }  // namespace

  std::uint32_t map_buttons(std::uint32_t button_flags, face_button_policy_e policy) {
    std::uint32_t mapped = 0;
    const auto map_if_pressed = [&mapped, button_flags](std::uint32_t source, nxbt_button_e destination) {
      if (is_pressed(button_flags, source)) {
        mapped |= destination;
      }
    };

    map_if_pressed(platf::DPAD_UP, nxbt_dpad_up);
    map_if_pressed(platf::DPAD_DOWN, nxbt_dpad_down);
    map_if_pressed(platf::DPAD_LEFT, nxbt_dpad_left);
    map_if_pressed(platf::DPAD_RIGHT, nxbt_dpad_right);
    map_if_pressed(platf::START, nxbt_plus);
    map_if_pressed(platf::BACK, nxbt_minus);
    map_if_pressed(platf::LEFT_STICK, nxbt_left_stick_press);
    map_if_pressed(platf::RIGHT_STICK, nxbt_right_stick_press);
    map_if_pressed(platf::LEFT_BUTTON, nxbt_left_shoulder);
    map_if_pressed(platf::RIGHT_BUTTON, nxbt_right_shoulder);
    map_if_pressed(platf::HOME, nxbt_home);
    map_if_pressed(platf::MISC_BUTTON, nxbt_capture);
    if (policy == face_button_policy_e::labels) {
      map_if_pressed(platf::A, nxbt_a);
      map_if_pressed(platf::B, nxbt_b);
      map_if_pressed(platf::X, nxbt_x);
      map_if_pressed(platf::Y, nxbt_y);
    } else {
      map_if_pressed(platf::A, nxbt_b);
      map_if_pressed(platf::B, nxbt_a);
      map_if_pressed(platf::X, nxbt_y);
      map_if_pressed(platf::Y, nxbt_x);
    }
    return mapped;
  }

  std::uint32_t map_triggers(std::uint8_t left_trigger, std::uint8_t right_trigger, trigger_state_t &state, std::uint8_t press_threshold, std::uint8_t release_threshold) {
    update_trigger(left_trigger, state.left_pressed, press_threshold, release_threshold);
    update_trigger(right_trigger, state.right_pressed, press_threshold, release_threshold);
    return (state.left_pressed ? nxbt_zl : 0U) | (state.right_pressed ? nxbt_zr : 0U);
  }

  std::int16_t map_axis(std::int16_t value, std::int16_t center, std::int16_t minimum, std::int16_t maximum) {
    const auto range = value < 0 ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    const auto extent = value < 0 ? minimum : maximum;
    const auto divisor = value < 0 ? 32768LL : 32767LL;
    const auto scaled = range * extent;
    const auto rounded = scaled >= 0 ? (scaled + (divisor / 2)) / divisor : (scaled - (divisor / 2)) / divisor;
    const auto mapped = static_cast<std::int64_t>(center) + rounded;
    return static_cast<std::int16_t>(mapped);
  }

  stick_position_t map_stick(std::int16_t x, std::int16_t y, const stick_calibration_t &calibration) {
    return {
      map_axis(x, calibration.center_x, calibration.min_x, calibration.max_x),
      map_axis(y, calibration.center_y, calibration.min_y, calibration.max_y),
    };
  }

  const stick_calibration_t &left_stick_calibration() {
    static constexpr stick_calibration_t calibration {
      2159,
      1916,
      -1466,
      1517,
      -1583,
      1465,
    };
    return calibration;
  }

  const stick_calibration_t &right_stick_calibration() {
    static constexpr stick_calibration_t calibration {
      2070,
      2013,
      -1522,
      1414,
      -1531,
      1510,
    };
    return calibration;
  }
}  // namespace input::nxbt
