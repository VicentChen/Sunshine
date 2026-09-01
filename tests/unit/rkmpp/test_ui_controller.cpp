/**
 * @file tests/unit/rkmpp/test_ui_controller.cpp
 * @brief Unit tests for the modal RKMPP Vulkan UI controller policy.
 */

// standard includes
#include <chrono>
#include <cstdint>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/ui_controller.h"

namespace {
  using namespace std::chrono_literals;
  using clock_t = platf::ui::controller_t::clock_t;
  using navigation_e = platf::ui::navigation_e;
  constexpr std::uint32_t start_back = platf::START | platf::BACK;

  /** @brief Send one test controller state at an offset from the zero epoch. */
  platf::ui::decision_t update(
    platf::ui::controller_t &ui,
    std::uint8_t controller,
    std::chrono::milliseconds offset,
    std::uint32_t buttons = 0,
    std::int16_t left_x = 0,
    std::int16_t left_y = 0
  ) {
    return ui.update(controller, {buttons, left_x, left_y}, clock_t::time_point {} + offset);
  }

  /** @brief Open the UI with the full chord and clear its release gate. */
  void open(platf::ui::controller_t &ui, std::uint8_t controller = 0) {
    EXPECT_TRUE(update(ui, controller, 0ms, start_back).consume);
    const auto opened = update(ui, controller, 3000ms, start_back);
    EXPECT_TRUE(opened.visible);
    EXPECT_TRUE(opened.visibility_changed);
    EXPECT_TRUE(opened.neutralize);
    EXPECT_TRUE(update(ui, controller, 3001ms).consume);
  }

  TEST(UiController, RequiresContinuousThreeSecondChordAndFullRelease) {
    platf::ui::controller_t ui;
    const auto started = update(ui, 0, 0ms, start_back);
    EXPECT_TRUE(started.consume);
    EXPECT_TRUE(started.neutralize);
    EXPECT_FALSE(update(ui, 0, 2999ms, start_back).visible);

    EXPECT_FALSE(update(ui, 0, 3000ms, platf::START).consume);
    EXPECT_FALSE(update(ui, 0, 4000ms, start_back).visible);
    const auto opened = update(ui, 0, 7000ms, start_back);
    EXPECT_TRUE(opened.visible);
    EXPECT_EQ(ui.owner(), 0);

    EXPECT_TRUE(update(ui, 0, 7001ms, platf::START).consume);
    EXPECT_TRUE(ui.owner().has_value());
    EXPECT_TRUE(update(ui, 0, 7002ms).consume);
    EXPECT_TRUE(ui.visible());
  }

  TEST(UiController, VideoTickCompletesChordWithoutRepeatedInputPackets) {
    platf::ui::controller_t ui;
    const auto started = update(ui, 2, 0ms, start_back);
    EXPECT_TRUE(started.consume);
    EXPECT_TRUE(started.neutralize);

    EXPECT_FALSE(ui.tick(clock_t::time_point {} + 2999ms).visible);
    const auto opened = ui.tick(clock_t::time_point {} + 3000ms);
    EXPECT_TRUE(opened.visible);
    EXPECT_TRUE(opened.visibility_changed);
    EXPECT_EQ(ui.owner(), 2);

    EXPECT_TRUE(update(ui, 2, 3001ms).consume);
    EXPECT_TRUE(ui.visible());

    EXPECT_TRUE(update(ui, 2, 4000ms, start_back).consume);
    EXPECT_TRUE(ui.tick(clock_t::time_point {} + 6999ms).visible);
    const auto closed = ui.tick(clock_t::time_point {} + 7000ms);
    EXPECT_FALSE(closed.visible);
    EXPECT_TRUE(closed.visibility_changed);
    EXPECT_TRUE(update(ui, 2, 7001ms).consume);
    EXPECT_FALSE(ui.owner().has_value());
  }

  TEST(UiController, OwnerChordClosesAndKeepsReleaseGateUntilBothButtonsAreUp) {
    platf::ui::controller_t ui;
    open(ui, 3);

    EXPECT_TRUE(update(ui, 3, 4000ms, start_back).consume);
    const auto closed = update(ui, 3, 7000ms, start_back);
    EXPECT_FALSE(closed.visible);
    EXPECT_TRUE(closed.visibility_changed);
    EXPECT_TRUE(closed.neutralize);
    EXPECT_TRUE(update(ui, 3, 7001ms, platf::BACK).consume);
    EXPECT_EQ(ui.owner(), 3);
    EXPECT_TRUE(update(ui, 3, 7002ms).consume);
    EXPECT_FALSE(ui.owner().has_value());
    EXPECT_FALSE(update(ui, 3, 7003ms).consume);
  }

  TEST(UiController, VisibleUiIsModalAndOnlyOwnerCanNavigate) {
    platf::ui::controller_t ui;
    open(ui, 1);

    EXPECT_TRUE(update(ui, 2, 4000ms, platf::A).consume);
    EXPECT_EQ(update(ui, 2, 4001ms, platf::A).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4100ms, platf::DPAD_UP).navigation, navigation_e::up);
    EXPECT_EQ(ui.snapshot().focus, 2);
    EXPECT_EQ(update(ui, 1, 4101ms, platf::DPAD_UP).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4102ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4103ms, platf::A).navigation, navigation_e::confirm);
    EXPECT_EQ(update(ui, 1, 4104ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4105ms, platf::BACK).navigation, navigation_e::back);
  }

  TEST(UiController, LeftStickUsesHysteresisAndPublishesChangedFocusRevision) {
    platf::ui::controller_t ui;
    open(ui);
    const auto initial = ui.snapshot();

    EXPECT_EQ(update(ui, 0, 4000ms, 0, 17000, 0).navigation, navigation_e::right);
    const auto first = ui.snapshot();
    EXPECT_EQ(first.focus, 1);
    EXPECT_GT(first.revision, initial.revision);
    EXPECT_EQ(update(ui, 0, 4001ms, 0, 9000, 0).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4002ms, 0, 7000, 0).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4003ms, 0, 17000, 0).navigation, navigation_e::right);
    EXPECT_EQ(ui.snapshot().focus, 2);
    EXPECT_EQ(update(ui, 0, 4004ms, 0, 0, 17000).navigation, navigation_e::up);
    EXPECT_EQ(ui.snapshot().focus, 1);
  }

  TEST(UiController, OwnerDisconnectHidesUiAndRequiresCleanup) {
    platf::ui::controller_t ui;
    open(ui, 4);
    const auto other = ui.disconnect(2);
    EXPECT_TRUE(other.visible);
    EXPECT_FALSE(other.neutralize);

    const auto owner = ui.disconnect(4);
    EXPECT_FALSE(owner.visible);
    EXPECT_TRUE(owner.visibility_changed);
    EXPECT_TRUE(owner.neutralize);
    EXPECT_FALSE(ui.owner().has_value());
  }

  TEST(UiController, ResetClearsModalStateWithoutChangingFocus) {
    platf::ui::controller_t ui;
    open(ui);
    update(ui, 0, 4000ms, platf::DPAD_RIGHT);
    ASSERT_EQ(ui.snapshot().focus, 1);
    ui.reset();
    EXPECT_FALSE(ui.visible());
    EXPECT_FALSE(ui.owner().has_value());
    EXPECT_EQ(ui.snapshot().focus, 1);
    EXPECT_FALSE(update(ui, 0, 5000ms).consume);
  }
}  // namespace
