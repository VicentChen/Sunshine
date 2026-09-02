/**
 * @file tests/unit/rkmpp/test_input_state_machine.cpp
 * @brief Tests the HDMI RX live-frame and recovery state model.
 */

#include "src/platform/linux/input_state_machine.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace {
  using namespace platf::input_sm;

  TEST(HdmirxInputState, GreenPlaceholderUsesBt709LimitedRange) {
    EXPECT_EQ(k_green_nv12_y, 149U);
    EXPECT_EQ(k_green_nv12_u, 54U);
    EXPECT_EQ(k_green_nv12_v, 34U);
    EXPECT_EQ(k_green_nv12_packed, 0x00223695U);
  }

  TEST(HdmirxInputState, ClassifiesOnlyUnavailableStatesAsPlaceholder) {
    EXPECT_TRUE(is_placeholder_state(state_e::starting));
    EXPECT_TRUE(is_placeholder_state(state_e::no_signal));
    EXPECT_TRUE(is_placeholder_state(state_e::reconfiguring));
    EXPECT_FALSE(is_placeholder_state(state_e::streaming_direct));
    EXPECT_FALSE(is_placeholder_state(state_e::streaming_rga));
    EXPECT_FALSE(is_placeholder_state(state_e::shutdown));
    EXPECT_FALSE(is_placeholder_state(state_e::fatal));
  }

  TEST(HdmirxInputState, FirstDequeuedMatchingFrameImmediatelyStreamsDirect) {
    state_machine_t state;
    EXPECT_TRUE(state.enter_streaming(false));
    EXPECT_EQ(state.state(), state_e::streaming_direct);
    EXPECT_EQ(state.transition_count(), 1U);
    EXPECT_TRUE(state.consume_idr_request());
    EXPECT_FALSE(state.consume_idr_request());
  }

  TEST(HdmirxInputState, FirstDequeuedMismatchedFrameImmediatelyStreamsThroughRga) {
    state_machine_t state;
    EXPECT_TRUE(state.enter_streaming(true));
    EXPECT_EQ(state.state(), state_e::streaming_rga);
    EXPECT_TRUE(is_streaming_state(state.state()));
  }

  TEST(HdmirxInputState, FrameRouteCanChangeWithoutAStabilityGate) {
    state_machine_t state;
    ASSERT_TRUE(state.enter_streaming(false));
    EXPECT_TRUE(state.enter_streaming(true, "new real frame dimensions"));
    EXPECT_EQ(state.state(), state_e::streaming_rga);
    EXPECT_EQ(state.last_reason(), "new real frame dimensions");
    EXPECT_TRUE(state.consume_idr_request());
  }

  TEST(HdmirxInputState, QueueRecoveryReturnsToLiveOnTheFirstFrame) {
    state_machine_t state;
    ASSERT_TRUE(state.enter_streaming(false));
    ASSERT_TRUE(state.enter_reconfiguring("source-change"));
    EXPECT_EQ(state.state(), state_e::reconfiguring);
    EXPECT_TRUE(state.enter_streaming(true, "first frame from new queue"));
    EXPECT_EQ(state.state(), state_e::streaming_rga);
  }

  TEST(HdmirxInputState, NoSignalIsTheOnlySteadyPlaceholderCondition) {
    state_machine_t state;
    ASSERT_TRUE(state.enter_no_signal("no frame"));
    EXPECT_EQ(state.state(), state_e::no_signal);
    EXPECT_FALSE(state.enter_no_signal("still no frame"));
    EXPECT_EQ(state.transition_count(), 1U);
  }

  TEST(HdmirxInputState, StateNamesAreStable) {
    EXPECT_EQ(state_name(state_e::starting), "starting");
    EXPECT_EQ(state_name(state_e::no_signal), "no_signal");
    EXPECT_EQ(state_name(state_e::streaming_direct), "streaming_direct");
    EXPECT_EQ(state_name(state_e::streaming_rga), "streaming_rga");
    EXPECT_EQ(state_name(state_e::reconfiguring), "reconfiguring");
    EXPECT_EQ(state_name(state_e::shutdown), "shutdown");
    EXPECT_EQ(state_name(state_e::fatal), "fatal");
  }

  TEST(HdmirxInputState, TerminalStatesRejectFurtherChanges) {
    state_machine_t shutdown_state;
    ASSERT_TRUE(shutdown_state.enter_shutdown());
    EXPECT_FALSE(shutdown_state.enter_streaming(false));
    EXPECT_FALSE(shutdown_state.enter_no_signal());
    EXPECT_TRUE(is_terminal_state(shutdown_state.state()));

    state_machine_t fatal_state;
    ASSERT_TRUE(fatal_state.enter_fatal());
    EXPECT_FALSE(fatal_state.enter_reconfiguring());
    EXPECT_TRUE(is_terminal_state(fatal_state.state()));
  }

  TEST(HdmirxInputState, ShutdownInterruptsPlaceholderWait) {
    state_machine_t state;
    bool wait_result = true;
    std::thread waiter([&] {
      wait_result = state.wait_for_interval();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(state.enter_shutdown());
    waiter.join();
    EXPECT_FALSE(wait_result);
  }
}  // namespace
