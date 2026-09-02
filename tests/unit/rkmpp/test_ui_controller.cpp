/**
 * @file tests/unit/rkmpp/test_ui_controller.cpp
 * @brief Unit tests for the modal RKMPP Vulkan UI controller policy.
 */

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/ui_controller.h"

namespace {
  using namespace std::chrono_literals;
  using clock_t = platf::ui::controller_t::clock_t;
  using navigation_e = platf::ui::navigation_e;
  using action_e = platf::ui::action_e;
  using page_e = platf::ui::page_e;
  using ui_size_e = platf::ui::ui_size_e;
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

  /** @brief Open the UI with ordered Start then Back and clear its release gate. */
  void open(platf::ui::controller_t &ui, std::uint8_t controller = 0) {
    const auto started = update(ui, controller, 0ms, platf::START);
    EXPECT_TRUE(started.consume);
    EXPECT_TRUE(started.neutralize);
    const auto opened = update(ui, controller, 1ms, start_back);
    EXPECT_TRUE(opened.visible);
    EXPECT_TRUE(opened.visibility_changed);
    EXPECT_TRUE(opened.neutralize);
    EXPECT_TRUE(update(ui, controller, 2ms).consume);
  }

  TEST(UiController, DefersStartAndOpensImmediatelyWhenBackJoins) {
    platf::ui::controller_t ui;
    const auto started = update(ui, 0, 0ms, platf::START);
    EXPECT_TRUE(started.consume);
    EXPECT_TRUE(started.neutralize);
    EXPECT_FALSE(started.visible);

    const auto opened = update(ui, 0, 1ms, start_back);
    EXPECT_TRUE(opened.consume);
    EXPECT_TRUE(opened.neutralize);
    EXPECT_TRUE(opened.visibility_changed);
    EXPECT_TRUE(opened.visible);
    EXPECT_EQ(ui.owner(), 0);

    EXPECT_TRUE(update(ui, 0, 2ms, platf::START).consume);
    EXPECT_TRUE(ui.owner().has_value());
    EXPECT_TRUE(update(ui, 0, 3ms).consume);
    EXPECT_TRUE(ui.visible());
  }

  TEST(UiController, DeferredStandaloneStartRequestsReplayOnRelease) {
    platf::ui::controller_t ui;
    const auto started = update(ui, 2, 0ms, platf::START);
    EXPECT_TRUE(started.consume);
    EXPECT_TRUE(started.neutralize);
    EXPECT_FALSE(started.replay_start_tap);
    EXPECT_FALSE(ui.tick(clock_t::time_point {} + 10s).visible);

    const auto released = update(ui, 2, 100ms);
    EXPECT_TRUE(released.consume);
    EXPECT_TRUE(released.replay_start_tap);
    EXPECT_FALSE(released.visible);
    EXPECT_FALSE(ui.owner().has_value());
  }

  TEST(UiController, OtherButtonCancelsStartDeferralAndPassesThroughUntilRelease) {
    platf::ui::controller_t ui;
    EXPECT_TRUE(update(ui, 0, 0ms, platf::START).consume);
    EXPECT_FALSE(update(ui, 0, 1ms, platf::START | platf::A).consume);
    EXPECT_FALSE(update(ui, 0, 2ms, platf::START).consume);
    EXPECT_FALSE(update(ui, 0, 3ms).consume);
    EXPECT_TRUE(update(ui, 0, 4ms, platf::START).consume);
  }

  TEST(UiController, OwnerChordClosesAndKeepsReleaseGateUntilBothButtonsAreUp) {
    platf::ui::controller_t ui;
    open(ui, 3);

    const auto closed = update(ui, 3, 10ms, start_back);
    EXPECT_FALSE(closed.visible);
    EXPECT_TRUE(closed.visibility_changed);
    EXPECT_TRUE(closed.neutralize);
    EXPECT_TRUE(update(ui, 3, 11ms, platf::BACK).consume);
    EXPECT_EQ(ui.owner(), 3);
    EXPECT_TRUE(update(ui, 3, 12ms).consume);
    EXPECT_FALSE(ui.owner().has_value());
    EXPECT_FALSE(update(ui, 3, 13ms).consume);
  }

  TEST(UiController, VisibleUiIsModalAndOnlyOwnerCanNavigate) {
    platf::ui::controller_t ui;
    open(ui, 1);

    EXPECT_TRUE(update(ui, 2, 4000ms, platf::A).consume);
    EXPECT_EQ(update(ui, 2, 4001ms, platf::A).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4100ms, platf::DPAD_UP).navigation, navigation_e::up);
    EXPECT_EQ(ui.snapshot().focus, 3);
    EXPECT_EQ(update(ui, 1, 4101ms, platf::DPAD_UP).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4102ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4103ms, platf::DPAD_RIGHT).navigation, navigation_e::right);
    EXPECT_EQ(ui.snapshot().focus, 3);
    EXPECT_EQ(update(ui, 1, 4104ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4105ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(ui.snapshot().focus, 0);
    EXPECT_EQ(update(ui, 1, 4106ms).navigation, navigation_e::none);
    const auto confirm = update(ui, 1, 4107ms, platf::A);
    EXPECT_EQ(confirm.navigation, navigation_e::confirm);
    EXPECT_EQ(confirm.action, action_e::none);
    EXPECT_EQ(ui.snapshot().page, page_e::connection_status);
    EXPECT_EQ(update(ui, 1, 4108ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 1, 4109ms, platf::BACK).navigation, navigation_e::back);
    EXPECT_EQ(ui.snapshot().page, page_e::main_menu);
  }

  TEST(UiController, MainMenuOpensPagesSizeSettingsAndExitActionClosesModal) {
    platf::ui::controller_t ui;
    open(ui);

    EXPECT_EQ(ui.snapshot().page, page_e::main_menu);
    EXPECT_EQ(update(ui, 0, 4000ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(ui.snapshot().focus, 1);
    EXPECT_EQ(update(ui, 0, 4001ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4002ms, platf::A).navigation, navigation_e::confirm);
    EXPECT_EQ(ui.snapshot().page, page_e::profile);
    EXPECT_TRUE(ui.visible());

    EXPECT_EQ(update(ui, 0, 4003ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4004ms, platf::BACK).navigation, navigation_e::back);
    EXPECT_EQ(ui.snapshot().page, page_e::main_menu);
    EXPECT_EQ(update(ui, 0, 4005ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4006ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(ui.snapshot().focus, 2);
    EXPECT_EQ(update(ui, 0, 4007ms).navigation, navigation_e::none);

    EXPECT_EQ(update(ui, 0, 4008ms, platf::A).navigation, navigation_e::confirm);
    ASSERT_EQ(ui.snapshot().page, page_e::ui_size);
    EXPECT_EQ(ui.snapshot().ui_size, ui_size_e::standard);
    EXPECT_EQ(ui.snapshot().ui_size_focus, 1);
    EXPECT_EQ(update(ui, 0, 4009ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(ui.snapshot().ui_size_focus, 2);
    EXPECT_EQ(update(ui, 0, 4010ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4011ms, platf::A).navigation, navigation_e::confirm);
    EXPECT_EQ(ui.snapshot().ui_size, ui_size_e::large);
    EXPECT_EQ(update(ui, 0, 4012ms, platf::BACK).navigation, navigation_e::back);
    EXPECT_EQ(ui.snapshot().page, page_e::main_menu);
    EXPECT_EQ(update(ui, 0, 4013ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4014ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(ui.snapshot().focus, 3);
    EXPECT_EQ(update(ui, 0, 4015ms).navigation, navigation_e::none);

    const auto closed = update(ui, 0, 4016ms, platf::A);
    EXPECT_EQ(closed.navigation, navigation_e::confirm);
    EXPECT_EQ(closed.action, action_e::close_modal);
    EXPECT_TRUE(closed.visibility_changed);
    EXPECT_TRUE(closed.neutralize);
    EXPECT_FALSE(closed.visible);
    EXPECT_FALSE(ui.visible());
    EXPECT_FALSE(ui.owner().has_value());
  }

  TEST(UiController, BackOnMainMenuClosesThroughTheSameAction) {
    platf::ui::controller_t ui;
    open(ui);

    const auto closed = update(ui, 0, 4000ms, platf::BACK);
    EXPECT_EQ(closed.navigation, navigation_e::back);
    EXPECT_EQ(closed.action, action_e::close_modal);
    EXPECT_TRUE(closed.visibility_changed);
    EXPECT_TRUE(closed.neutralize);
    EXPECT_FALSE(closed.visible);
    EXPECT_FALSE(ui.owner().has_value());
  }

  TEST(UiController, AutomaticConnectionStatusTracksCombinedReadinessWithoutTakingInput) {
    platf::ui::controller_t ui;
    platf::ui::connection_status_t status {
      .video_state = "no_signal",
      .gamepad_state = "ready",
      .moonlight_width = 1920,
      .moonlight_height = 1080,
      .gamepad_ready = true
    };
    ui.update_connection(status, clock_t::time_point {0ms});
    const auto video_wait = ui.snapshot();
    EXPECT_TRUE(video_wait.visible);
    EXPECT_FALSE(video_wait.modal);
    EXPECT_EQ(video_wait.page, page_e::connection_status);
    EXPECT_FALSE(update(ui, 0, 0ms, platf::A).consume);

    status.video_state = "streaming_direct";
    status.video_ready = true;
    ui.update_connection(status, clock_t::time_point {1ms});
    const auto ready_but_holding = ui.snapshot();
    EXPECT_TRUE(ready_but_holding.visible);
    EXPECT_GT(ready_but_holding.revision, video_wait.revision);

    ui.update_connection(status, clock_t::time_point {3000ms});
    EXPECT_TRUE(ui.snapshot().visible);
    ui.update_connection(status, clock_t::time_point {3001ms});
    const auto ready = ui.snapshot();
    EXPECT_FALSE(ready.visible);
    EXPECT_GT(ready.revision, ready_but_holding.revision);

    status.gamepad_state = "starting";
    status.gamepad_ready = false;
    ui.update_connection(status, clock_t::time_point {3002ms});
    const auto gamepad_wait = ui.snapshot();
    EXPECT_TRUE(gamepad_wait.visible);
    EXPECT_FALSE(gamepad_wait.modal);
    EXPECT_GT(gamepad_wait.revision, ready.revision);

    const auto unchanged_revision = gamepad_wait.revision;
    ui.update_connection(status, clock_t::time_point {6002ms});
    EXPECT_EQ(ui.snapshot().revision, unchanged_revision);
  }

  TEST(UiController, ReadinessLossRestartsTheFullStableHold) {
    platf::ui::controller_t ui;
    platf::ui::connection_status_t ready {
      .video_state = "streaming_direct",
      .gamepad_state = "ready",
      .video_ready = true,
      .gamepad_ready = true
    };

    ui.update_connection(ready, clock_t::time_point {0ms});
    ui.update_connection(ready, clock_t::time_point {3000ms});
    EXPECT_FALSE(ui.snapshot().visible);

    auto reconnecting = ready;
    reconnecting.gamepad_state = "starting";
    reconnecting.gamepad_ready = false;
    ui.update_connection(reconnecting, clock_t::time_point {3001ms});
    EXPECT_TRUE(ui.snapshot().visible);

    ui.update_connection(ready, clock_t::time_point {4000ms});
    ui.update_connection(ready, clock_t::time_point {6999ms});
    EXPECT_TRUE(ui.snapshot().visible);
    ui.update_connection(ready, clock_t::time_point {7000ms});
    EXPECT_FALSE(ui.snapshot().visible);
  }

  TEST(UiController, ModalConnectionPageRemainsVisibleAfterAutomaticCompletion) {
    platf::ui::controller_t ui;
    ui.update_connection({.video_state = "no_signal", .gamepad_state = "starting"});
    open(ui);
    EXPECT_TRUE(ui.snapshot().modal);
    EXPECT_EQ(update(ui, 0, 4000ms, platf::A).navigation, navigation_e::confirm);
    ASSERT_EQ(ui.snapshot().page, page_e::connection_status);

    ui.update_connection({.video_state = "streaming_direct", .gamepad_state = "ready", .video_ready = true, .gamepad_ready = true});
    EXPECT_TRUE(ui.snapshot().visible);
    EXPECT_TRUE(ui.snapshot().modal);
    EXPECT_EQ(ui.snapshot().page, page_e::connection_status);
  }

  TEST(UiController, ProfileUpdatesInvalidateOnlyTheVisibleProfilePage) {
    platf::ui::controller_t ui;
    platf::ui::profile_status_t profile {.captured_frames = 150, .available = true};
    profile.metrics[static_cast<std::size_t>(platf::ui::profile_metric_e::host_send)] = {
      .count = 150,
      .p50_us = 17000,
      .p95_us = 24000,
      .p99_us = 31000
    };

    const auto initial = ui.snapshot().revision;
    ui.update_profile(profile);
    EXPECT_EQ(ui.snapshot().revision, initial);
    EXPECT_EQ(ui.snapshot().profile.captured_frames, 150U);

    open(ui);
    EXPECT_EQ(update(ui, 0, 4000ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(update(ui, 0, 4001ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4002ms, platf::A).navigation, navigation_e::confirm);
    ASSERT_EQ(ui.snapshot().page, page_e::profile);
    const auto visible_revision = ui.snapshot().revision;

    profile.captured_frames = 151;
    ui.update_profile(profile);
    EXPECT_GT(ui.snapshot().revision, visible_revision);
    EXPECT_EQ(ui.snapshot().profile.captured_frames, 151U);
    const auto unchanged_revision = ui.snapshot().revision;
    ui.update_profile(profile);
    EXPECT_EQ(ui.snapshot().revision, unchanged_revision);

    profile.timeline.frame_count = 1;
    profile.timeline.stream_generation = 2;
    profile.timeline.frames[0] = {.frame_index = 17, .end_us = 12000};
    ui.update_profile(profile);
    EXPECT_GT(ui.snapshot().revision, unchanged_revision);
    EXPECT_EQ(ui.snapshot().profile.timeline.frames[0].frame_index, 17);
  }

  TEST(UiController, ProfileUsesBoundedVerticalControllerScrolling) {
    platf::ui::controller_t ui;
    open(ui);
    EXPECT_EQ(update(ui, 0, 4000ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(update(ui, 0, 4001ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4002ms, platf::A).navigation, navigation_e::confirm);
    ASSERT_EQ(ui.snapshot().page, page_e::profile);
    EXPECT_EQ(ui.snapshot().profile_scroll_steps, 0U);

    const auto initial_revision = ui.snapshot().revision;
    EXPECT_EQ(update(ui, 0, 4003ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4004ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    EXPECT_EQ(ui.snapshot().profile_scroll_steps, 1U);
    EXPECT_GT(ui.snapshot().revision, initial_revision);
    EXPECT_EQ(update(ui, 0, 4005ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4006ms, platf::DPAD_UP).navigation, navigation_e::up);
    EXPECT_EQ(ui.snapshot().profile_scroll_steps, 0U);

    EXPECT_EQ(update(ui, 0, 4007ms).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4008ms, platf::DPAD_UP).navigation, navigation_e::up);
    EXPECT_EQ(ui.snapshot().profile_scroll_steps, 0U);

    for (std::uint32_t index = 0; index < platf::ui::profile_scroll_step_limit + 2U; ++index) {
      const auto offset = std::chrono::milliseconds {5000 + static_cast<int>(index) * 2};
      EXPECT_EQ(update(ui, 0, offset).navigation, navigation_e::none);
      EXPECT_EQ(update(ui, 0, offset + 1ms, platf::DPAD_DOWN).navigation, navigation_e::down);
    }
    EXPECT_EQ(ui.snapshot().profile_scroll_steps, platf::ui::profile_scroll_step_limit);
  }

  TEST(UiController, VerticalStickUsesHysteresisWhileHorizontalNavigationKeepsFocus) {
    platf::ui::controller_t ui;
    open(ui);
    const auto initial = ui.snapshot();

    EXPECT_EQ(update(ui, 0, 4000ms, 0, 17000, 0).navigation, navigation_e::right);
    const auto first = ui.snapshot();
    EXPECT_EQ(first.focus, 0);
    EXPECT_EQ(first.revision, initial.revision);
    EXPECT_EQ(update(ui, 0, 4001ms, 0, 9000, 0).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4002ms, 0, 7000, 0).navigation, navigation_e::none);
    EXPECT_EQ(update(ui, 0, 4003ms, 0, 17000, 0).navigation, navigation_e::right);
    EXPECT_EQ(ui.snapshot().focus, 0);
    EXPECT_EQ(update(ui, 0, 4004ms, 0, 0, 17000).navigation, navigation_e::up);
    EXPECT_EQ(ui.snapshot().focus, 3);
    EXPECT_GT(ui.snapshot().revision, initial.revision);
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
    update(ui, 0, 4000ms, platf::DPAD_DOWN);
    ASSERT_EQ(ui.snapshot().focus, 1);
    ui.reset();
    EXPECT_FALSE(ui.visible());
    EXPECT_FALSE(ui.owner().has_value());
    EXPECT_EQ(ui.snapshot().focus, 1);
    EXPECT_EQ(ui.snapshot().page, page_e::main_menu);
    EXPECT_FALSE(update(ui, 0, 5000ms).consume);
  }
}  // namespace
