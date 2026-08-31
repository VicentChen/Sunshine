/**
 * @file tests/unit/ns/test_nxbt_mapping.cpp
 * @brief Unit tests for Sunshine-to-NXBT input conversion.
 */

// standard includes
#include <array>
#include <limits>

// local includes
#include "tests/tests_common.h"
#include "src/input/nxbt_mapping.h"

TEST(NxbtMappingTest, MapsEverySupportedButtonAndIgnoresUnsupportedButtons) {
  using namespace input::nxbt;
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 16> mappings {
    std::pair {platf::DPAD_UP, static_cast<std::uint32_t>(nxbt_dpad_up)},
    {platf::DPAD_DOWN, static_cast<std::uint32_t>(nxbt_dpad_down)},
    {platf::DPAD_LEFT, static_cast<std::uint32_t>(nxbt_dpad_left)},
    {platf::DPAD_RIGHT, static_cast<std::uint32_t>(nxbt_dpad_right)},
    {platf::START, static_cast<std::uint32_t>(nxbt_plus)},
    {platf::BACK, static_cast<std::uint32_t>(nxbt_minus)},
    {platf::LEFT_STICK, static_cast<std::uint32_t>(nxbt_left_stick_press)},
    {platf::RIGHT_STICK, static_cast<std::uint32_t>(nxbt_right_stick_press)},
    {platf::LEFT_BUTTON, static_cast<std::uint32_t>(nxbt_left_shoulder)},
    {platf::RIGHT_BUTTON, static_cast<std::uint32_t>(nxbt_right_shoulder)},
    {platf::HOME, static_cast<std::uint32_t>(nxbt_home)},
    {platf::MISC_BUTTON, static_cast<std::uint32_t>(nxbt_capture)},
    {platf::A, static_cast<std::uint32_t>(nxbt_a)},
    {platf::B, static_cast<std::uint32_t>(nxbt_b)},
    {platf::X, static_cast<std::uint32_t>(nxbt_x)},
    {platf::Y, static_cast<std::uint32_t>(nxbt_y)},
  };
  for (const auto &[source, expected] : mappings) {
    SCOPED_TRACE(source);
    EXPECT_EQ(map_buttons(source, face_button_policy_e::labels), expected);
  }
  EXPECT_EQ(map_buttons(platf::PADDLE1 | platf::PADDLE2 | platf::PADDLE3 | platf::PADDLE4 | platf::TOUCHPAD_BUTTON, face_button_policy_e::labels), 0U);

  std::uint32_t all_buttons = 0;
  std::uint32_t all_expected = 0;
  for (const auto &[source, expected] : mappings) {
    all_buttons |= source;
    all_expected |= expected;
  }
  EXPECT_EQ(map_buttons(all_buttons, face_button_policy_e::labels), all_expected);
}

TEST(NxbtMappingTest, SwapsOnlyFaceButtonsForPositionPolicy) {
  using namespace input::nxbt;
  EXPECT_EQ(map_buttons(platf::A | platf::B | platf::X | platf::Y, face_button_policy_e::positions), nxbt_a | nxbt_b | nxbt_x | nxbt_y);
  EXPECT_EQ(map_buttons(platf::A, face_button_policy_e::positions), nxbt_b);
  EXPECT_EQ(map_buttons(platf::B, face_button_policy_e::positions), nxbt_a);
  EXPECT_EQ(map_buttons(platf::X, face_button_policy_e::positions), nxbt_y);
  EXPECT_EQ(map_buttons(platf::Y, face_button_policy_e::positions), nxbt_x);
}

TEST(NxbtMappingTest, AppliesTriggerHysteresisAtEveryBoundary) {
  using namespace input::nxbt;
  trigger_state_t state;
  for (const auto value : {0, 47, 48, 49, 63}) {
    EXPECT_EQ(map_triggers(value, 0, state), 0U);
  }
  EXPECT_EQ(map_triggers(64, 0, state), nxbt_zl);
  EXPECT_EQ(map_triggers(65, 255, state), nxbt_zl | nxbt_zr);
  EXPECT_EQ(map_triggers(49, 63, state), nxbt_zl | nxbt_zr);
  EXPECT_EQ(map_triggers(48, 47, state), 0U);
}

TEST(NxbtMappingTest, TriggerHysteresisUsesConfiguredThresholds) {
  using namespace input::nxbt;
  trigger_state_t state;
  EXPECT_EQ(map_triggers(89, 0, state, 90, 70), 0U);
  EXPECT_EQ(map_triggers(90, 0, state, 90, 70), nxbt_zl);
  EXPECT_EQ(map_triggers(71, 0, state, 90, 70), nxbt_zl);
  EXPECT_EQ(map_triggers(70, 0, state, 90, 70), 0U);
}

TEST(NxbtMappingTest, MapsAxisExtremesAndUsesSeparateStickCalibrations) {
  using namespace input::nxbt;
  const auto &left = left_stick_calibration();
  const auto &right = right_stick_calibration();
  const std::array values {
    std::numeric_limits<std::int16_t>::min(),
    static_cast<std::int16_t>(-1),
    static_cast<std::int16_t>(0),
    static_cast<std::int16_t>(1),
    std::numeric_limits<std::int16_t>::max(),
  };
  for (const auto value : values) {
    const auto position = map_stick(value, value, left);
    if (value == std::numeric_limits<std::int16_t>::min()) {
      EXPECT_EQ(position.x, left.center_x + left.min_x);
      EXPECT_EQ(position.y, left.center_y + left.min_y);
    } else if (value == 0) {
      EXPECT_EQ(position.x, left.center_x);
      EXPECT_EQ(position.y, left.center_y);
    } else if (value == std::numeric_limits<std::int16_t>::max()) {
      EXPECT_EQ(position.x, left.center_x + left.max_x);
      EXPECT_EQ(position.y, left.center_y + left.max_y);
    }
  }
  EXPECT_NE(map_stick(0, 0, left).x, map_stick(0, 0, right).x);
  EXPECT_NE(map_stick(0, 0, left).y, map_stick(0, 0, right).y);
}
