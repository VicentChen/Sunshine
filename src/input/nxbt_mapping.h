/**
 * @file src/input/nxbt_mapping.h
 * @brief Pure conversion of Sunshine gamepad input to NXBT controller values.
 */
#pragma once

// local includes
#include "src/platform/common.h"

// standard includes
#include <cstdint>

namespace input::nxbt {
  /**
   * @brief Face-button mapping policy for a Nintendo Switch controller.
   */
  enum class face_button_policy_e {
    labels,  ///< Preserve button labels: Sunshine A maps to Switch A.
    positions,  ///< Preserve physical positions: Sunshine A maps to Switch B.
  };

  /**
   * @brief NXBT button flags used by the bridge's complete input snapshot.
   */
  enum nxbt_button_e : std::uint32_t {
    nxbt_dpad_up = 1U << 0U,  ///< Switch D-pad up.
    nxbt_dpad_down = 1U << 1U,  ///< Switch D-pad down.
    nxbt_dpad_left = 1U << 2U,  ///< Switch D-pad left.
    nxbt_dpad_right = 1U << 3U,  ///< Switch D-pad right.
    nxbt_plus = 1U << 4U,  ///< Switch PLUS button.
    nxbt_minus = 1U << 5U,  ///< Switch MINUS button.
    nxbt_left_stick_press = 1U << 6U,  ///< Switch left-stick press.
    nxbt_right_stick_press = 1U << 7U,  ///< Switch right-stick press.
    nxbt_left_shoulder = 1U << 8U,  ///< Switch L shoulder.
    nxbt_right_shoulder = 1U << 9U,  ///< Switch R shoulder.
    nxbt_home = 1U << 10U,  ///< Switch HOME button.
    nxbt_capture = 1U << 11U,  ///< Switch CAPTURE button.
    nxbt_y = 1U << 12U,  ///< Switch Y face button.
    nxbt_x = 1U << 13U,  ///< Switch X face button.
    nxbt_b = 1U << 14U,  ///< Switch B face button.
    nxbt_a = 1U << 15U,  ///< Switch A face button.
    nxbt_zl = 1U << 16U,  ///< Switch ZL digital trigger.
    nxbt_zr = 1U << 17U,  ///< Switch ZR digital trigger.
  };

  /**
   * @brief NXBT calibration values for one analog stick.
   */
  struct stick_calibration_t {
    std::int16_t center_x = 0;  ///< Calibrated X-axis center.
    std::int16_t center_y = 0;  ///< Calibrated Y-axis center.
    std::int16_t min_x = 0;  ///< X-axis travel from center at full negative input.
    std::int16_t max_x = 0;  ///< X-axis travel from center at full positive input.
    std::int16_t min_y = 0;  ///< Y-axis travel from center at full negative input.
    std::int16_t max_y = 0;  ///< Y-axis travel from center at full positive input.
  };

  /**
   * @brief Calibrated NXBT analog stick coordinates.
   */
  struct stick_position_t {
    std::int16_t x = 0;  ///< Calibrated X coordinate.
    std::int16_t y = 0;  ///< Calibrated Y coordinate.
  };

  /**
   * @brief Persistent digital state used for trigger hysteresis.
   */
  struct trigger_state_t {
    bool left_pressed = false;  ///< Whether ZL was pressed by the previous conversion.
    bool right_pressed = false;  ///< Whether ZR was pressed by the previous conversion.
  };

  /**
   * @brief Convert Sunshine button flags to NXBT button flags.
   *
   * @param button_flags Sunshine button mask.
   * @param policy Face-button mapping policy.
   * @return NXBT button mask excluding trigger state.
   */
  std::uint32_t map_buttons(std::uint32_t button_flags, face_button_policy_e policy);

  /**
   * @brief Apply digital-trigger hysteresis to Sunshine trigger values.
   *
   * @param left_trigger Sunshine left-trigger value.
   * @param right_trigger Sunshine right-trigger value.
   * @param state Previous and updated trigger state.
   * @param press_threshold Value at or above which a released trigger becomes pressed.
   * @param release_threshold Value at or below which a pressed trigger becomes released.
   * @return NXBT ZL/ZR button bits.
   */
  std::uint32_t map_triggers(
    std::uint8_t left_trigger,
    std::uint8_t right_trigger,
    trigger_state_t &state,
    std::uint8_t press_threshold = 64,
    std::uint8_t release_threshold = 48
  );

  /**
   * @brief Convert one Sunshine axis to one NXBT calibrated coordinate.
   *
   * @param value Sunshine axis value in [-32768, 32767].
   * @param center NXBT calibrated center.
   * @param minimum Travel from center for full negative input.
   * @param maximum Travel from center for full positive input.
   * @return Calibrated NXBT coordinate.
   */
  std::int16_t map_axis(std::int16_t value, std::int16_t center, std::int16_t minimum, std::int16_t maximum);

  /**
   * @brief Convert Sunshine stick axes with a stick-specific calibration.
   *
   * @param x Sunshine X-axis value.
   * @param y Sunshine Y-axis value.
   * @param calibration NXBT calibration for the selected stick.
   * @return Calibrated NXBT stick position.
   */
  stick_position_t map_stick(std::int16_t x, std::int16_t y, const stick_calibration_t &calibration);

  /**
   * @brief Return the fixed NXBT calibration for the Pro Controller left stick.
   *
   * @return Left-stick calibration.
   */
  const stick_calibration_t &left_stick_calibration();

  /**
   * @brief Return the fixed NXBT calibration for the Pro Controller right stick.
   *
   * @return Right-stick calibration.
   */
  const stick_calibration_t &right_stick_calibration();
}  // namespace input::nxbt
