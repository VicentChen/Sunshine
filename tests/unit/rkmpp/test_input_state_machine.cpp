/**
 * @file tests/unit/rkmpp/test_input_state_machine.cpp
 * @brief Unit tests for the HDMI RX input state machine (Stage 5).
 *
 * Tests cover:
 * - Green NV12 color values and packed representation
 * - State transitions (valid and invalid)
 * - IDR request lifecycle (first green frame, recovery)
 * - Placeholder timing constants
 * - Stable timing counter and threshold
 * - Shutdown interruption of placeholder wait
 * - Source-change storms
 * - State classification helpers
 */
#ifdef SUNSHINE_BUILD_RKMPP

  #include <chrono>
  #include <thread>

  #include <gtest/gtest.h>

  #include <src/platform/linux/input_state_machine.h>

namespace {

  using namespace platf::input_sm;

  // ====================================================================
  // Green NV12 color values
  // ====================================================================

  TEST(GreenNV12Values, YUVValuesAreInLimitedRange) {
    EXPECT_GE(k_green_nv12_y, 16);
    EXPECT_LE(k_green_nv12_y, 235);
    EXPECT_GE(k_green_nv12_u, 16);
    EXPECT_LE(k_green_nv12_u, 240);
    EXPECT_GE(k_green_nv12_v, 16);
    EXPECT_LE(k_green_nv12_v, 240);
  }

  TEST(GreenNV12Values, MatchesBT709LimitedRangeGreen) {
    EXPECT_EQ(k_green_nv12_y, 149);
    EXPECT_EQ(k_green_nv12_u, 54);
    EXPECT_EQ(k_green_nv12_v, 34);
  }

  TEST(GreenNV12Values, PackedColorEncodesCorrectly) {
    const std::uint32_t expected =
        (static_cast<std::uint32_t>(k_green_nv12_v) << 16U) |
        (static_cast<std::uint32_t>(k_green_nv12_u) << 8U) |
        static_cast<std::uint32_t>(k_green_nv12_y);
    EXPECT_EQ(k_green_nv12_packed, expected);
    EXPECT_EQ(k_green_nv12_packed & 0xFFU, k_green_nv12_y);
    EXPECT_EQ((k_green_nv12_packed >> 8U) & 0xFFU, k_green_nv12_u);
    EXPECT_EQ((k_green_nv12_packed >> 16U) & 0xFFU, k_green_nv12_v);
    EXPECT_EQ((k_green_nv12_packed >> 24U) & 0xFFU, 0U);
  }

  TEST(GreenNV12Values, IsNotBlack) {
    constexpr std::uint32_t nv12_black = 0x00808000;
    EXPECT_NE(k_green_nv12_packed, nv12_black);
  }

  TEST(GreenNV12Values, IsNotRGBGreenBytes) {
    EXPECT_NE(k_green_nv12_y, 0);
    EXPECT_NE(k_green_nv12_y, 255);
    EXPECT_NE(k_green_nv12_u, 128);
    EXPECT_NE(k_green_nv12_v, 128);
  }

  // ====================================================================
  // Placeholder timing constants
  // ====================================================================

  TEST(PlaceholderConstants, FPSIsReasonablyLow) {
    EXPECT_GE(k_placeholder_fps, 1U);
    EXPECT_LE(k_placeholder_fps, 10U);
  }

  TEST(PlaceholderConstants, IntervalMatchesFPS) {
    const auto expected_ms = std::chrono::milliseconds(1000 / k_placeholder_fps);
    EXPECT_EQ(k_placeholder_interval, expected_ms);
  }

  TEST(PlaceholderConstants, StableTimingCountIsPositive) {
    EXPECT_GE(k_stable_timing_count, 1U);
  }

  TEST(PlaceholderConstants, SignalRecoveryTimeoutIsReasonable) {
    EXPECT_GE(k_signal_recovery_timeout, std::chrono::seconds(1));
    EXPECT_LE(k_signal_recovery_timeout, std::chrono::seconds(30));
  }

  // ====================================================================
  // State classification helpers
  // ====================================================================

  TEST(StateClassification, PlaceholderStatesAreCorrect) {
    EXPECT_TRUE(is_placeholder_state(state_e::starting));
    EXPECT_TRUE(is_placeholder_state(state_e::no_signal));
    EXPECT_TRUE(is_placeholder_state(state_e::negotiating));
    EXPECT_TRUE(is_placeholder_state(state_e::source_change));
    EXPECT_FALSE(is_placeholder_state(state_e::streaming_direct));
    EXPECT_FALSE(is_placeholder_state(state_e::streaming_rga));
    EXPECT_FALSE(is_placeholder_state(state_e::shutdown));
    EXPECT_FALSE(is_placeholder_state(state_e::fatal));
  }

  TEST(StateClassification, StreamingStatesAreCorrect) {
    EXPECT_FALSE(is_streaming_state(state_e::starting));
    EXPECT_FALSE(is_streaming_state(state_e::no_signal));
    EXPECT_FALSE(is_streaming_state(state_e::negotiating));
    EXPECT_FALSE(is_streaming_state(state_e::source_change));
    EXPECT_TRUE(is_streaming_state(state_e::streaming_direct));
    EXPECT_TRUE(is_streaming_state(state_e::streaming_rga));
    EXPECT_FALSE(is_streaming_state(state_e::shutdown));
    EXPECT_FALSE(is_streaming_state(state_e::fatal));
  }

  TEST(StateClassification, TerminalStatesAreCorrect) {
    EXPECT_FALSE(is_terminal_state(state_e::starting));
    EXPECT_FALSE(is_terminal_state(state_e::no_signal));
    EXPECT_FALSE(is_terminal_state(state_e::negotiating));
    EXPECT_FALSE(is_terminal_state(state_e::source_change));
    EXPECT_FALSE(is_terminal_state(state_e::streaming_direct));
    EXPECT_FALSE(is_terminal_state(state_e::streaming_rga));
    EXPECT_TRUE(is_terminal_state(state_e::shutdown));
    EXPECT_TRUE(is_terminal_state(state_e::fatal));
  }

  TEST(StateClassification, StateNamesAreNonEmpty) {
    EXPECT_FALSE(state_name(state_e::starting).empty());
    EXPECT_FALSE(state_name(state_e::no_signal).empty());
    EXPECT_FALSE(state_name(state_e::negotiating).empty());
    EXPECT_FALSE(state_name(state_e::streaming_direct).empty());
    EXPECT_FALSE(state_name(state_e::streaming_rga).empty());
    EXPECT_FALSE(state_name(state_e::source_change).empty());
    EXPECT_FALSE(state_name(state_e::shutdown).empty());
    EXPECT_FALSE(state_name(state_e::fatal).empty());
  }

  TEST(StateClassification, ClassificationsAreDisjoint) {
    for (auto s : {state_e::starting, state_e::no_signal, state_e::negotiating,
                   state_e::streaming_direct, state_e::streaming_rga,
                   state_e::source_change, state_e::shutdown, state_e::fatal}) {
      int count = 0;
      if (is_placeholder_state(s)) ++count;
      if (is_streaming_state(s)) ++count;
      if (is_terminal_state(s)) ++count;
      EXPECT_LE(count, 1) << "State " << state_name(s) << " is in multiple categories";
    }
  }

  // ====================================================================
  // State machine: initial state
  // ====================================================================

  TEST(StateMachine, StartsInStartingState) {
    state_machine_t sm;
    EXPECT_EQ(sm.state(), state_e::starting);
  }

  TEST(StateMachine, InitialTransitionCountIsZero) {
    state_machine_t sm;
    EXPECT_EQ(sm.transition_count(), 0U);
  }

  // ====================================================================
  // State machine: starting -> no_signal
  // ====================================================================

  TEST(StateMachine, StartingToNoSignal) {
    state_machine_t sm;
    EXPECT_TRUE(sm.enter_no_signal("test no signal"));
    EXPECT_EQ(sm.state(), state_e::no_signal);
    EXPECT_EQ(sm.last_reason(), "test no signal");
    EXPECT_EQ(sm.transition_count(), 1U);
  }

  // ====================================================================
  // State machine: no_signal -> negotiating -> streaming
  // ====================================================================

  TEST(StateMachine, StartingCanEnterNegotiating) {
    state_machine_t sm;
    EXPECT_TRUE(sm.enter_negotiating("initial timing"));
    EXPECT_EQ(sm.state(), state_e::negotiating);
  }

  TEST(StateMachine, ResetStableTimingRequiresFreshObservations) {
    state_machine_t sm;
    ASSERT_TRUE(sm.enter_negotiating());
    sm.record_stable_timing();
    sm.record_stable_timing();
    sm.reset_stable_timing();
    EXPECT_FALSE(sm.timing_is_stable());
    EXPECT_FALSE(sm.enter_streaming_direct());
  }

  TEST(StateMachine, NoSignalToNegotiating) {
    state_machine_t sm;
    sm.enter_no_signal();
    EXPECT_TRUE(sm.enter_negotiating("timing detected"));
    EXPECT_EQ(sm.state(), state_e::negotiating);
  }

  TEST(StateMachine, NegotiatingRequiresStableTimingForStreaming) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    EXPECT_FALSE(sm.timing_is_stable());
    EXPECT_FALSE(sm.enter_streaming_direct());
    EXPECT_FALSE(sm.enter_streaming_rga());
    EXPECT_EQ(sm.state(), state_e::negotiating);
  }

  TEST(StateMachine, NegotiatingToStreamingDirectAfterStableTiming) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) {
      sm.record_stable_timing();
    }
    EXPECT_TRUE(sm.timing_is_stable());
    EXPECT_TRUE(sm.enter_streaming_direct("direct ok"));
    EXPECT_EQ(sm.state(), state_e::streaming_direct);
  }

  TEST(StateMachine, NegotiatingToStreamingRGAAfterStableTiming) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) {
      sm.record_stable_timing();
    }
    EXPECT_TRUE(sm.enter_streaming_rga("rga ok"));
    EXPECT_EQ(sm.state(), state_e::streaming_rga);
  }

  // ====================================================================
  // State machine: streaming -> source_change -> no_signal cycle
  // ====================================================================

  TEST(StateMachine, StreamingDirectToSourceChange) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    EXPECT_TRUE(sm.enter_source_change("hdmi source change"));
    EXPECT_EQ(sm.state(), state_e::source_change);
  }

  TEST(StateMachine, StreamingRGAToSourceChange) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_rga();
    EXPECT_TRUE(sm.enter_source_change("hdmi source change"));
    EXPECT_EQ(sm.state(), state_e::source_change);
  }

  TEST(StateMachine, SourceChangeToNoSignal) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    sm.enter_source_change();
    EXPECT_TRUE(sm.enter_no_signal("cable unplugged"));
    EXPECT_EQ(sm.state(), state_e::no_signal);
  }

  TEST(StateMachine, SourceChangeToNegotiating) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_rga();
    sm.enter_source_change();
    EXPECT_TRUE(sm.enter_negotiating("new timing detected"));
    EXPECT_EQ(sm.state(), state_e::negotiating);
  }

  TEST(StateMachine, SourceChangeResetsStableCount) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    sm.enter_source_change();
    sm.enter_negotiating();
    EXPECT_FALSE(sm.timing_is_stable());
  }

  // ====================================================================
  // State machine: full recovery cycle
  // ====================================================================

  TEST(StateMachine, FullRecoveryCycle) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    sm.enter_source_change();
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_rga();
    EXPECT_EQ(sm.state(), state_e::streaming_rga);
    EXPECT_GE(sm.transition_count(), 7U);
  }

  // ====================================================================
  // State machine: invalid transitions
  // ====================================================================

  TEST(StateMachine, StartingAllowsInitialNegotiationButRejectsInvalidTransitions) {
    state_machine_t sm;

    // A detected initial timing may begin recovery directly from starting.
    EXPECT_TRUE(sm.enter_negotiating("initial timing detected"));
    EXPECT_EQ(sm.state(), state_e::negotiating);
    EXPECT_EQ(sm.last_reason(), "initial timing detected");
    EXPECT_EQ(sm.transition_count(), 1U);

    // Verify the remaining starting-only boundary on independent machines:
    // direct/RGA streaming requires stable negotiated timings, while a source
    // change cannot exist before capture has begun. Rejected transitions must
    // leave both state and transition accounting unchanged.
    state_machine_t direct_sm;
    EXPECT_FALSE(direct_sm.enter_streaming_direct());
    EXPECT_EQ(direct_sm.state(), state_e::starting);
    EXPECT_EQ(direct_sm.transition_count(), 0U);

    state_machine_t rga_sm;
    EXPECT_FALSE(rga_sm.enter_streaming_rga());
    EXPECT_EQ(rga_sm.state(), state_e::starting);
    EXPECT_EQ(rga_sm.transition_count(), 0U);

    state_machine_t source_change_sm;
    EXPECT_FALSE(source_change_sm.enter_source_change());
    EXPECT_EQ(source_change_sm.state(), state_e::starting);
    EXPECT_EQ(source_change_sm.transition_count(), 0U);
  }

  TEST(StateMachine, RejectsInvalidTransitionFromNoSignal) {
    state_machine_t sm;
    sm.enter_no_signal();
    EXPECT_FALSE(sm.enter_streaming_direct());
    EXPECT_FALSE(sm.enter_streaming_rga());
    EXPECT_FALSE(sm.enter_source_change());
    EXPECT_EQ(sm.state(), state_e::no_signal);
  }

  TEST(StateMachine, RejectsInvalidTransitionFromStreamingDirect) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    EXPECT_FALSE(sm.enter_no_signal());
    EXPECT_FALSE(sm.enter_negotiating());
    EXPECT_FALSE(sm.enter_streaming_rga());
    EXPECT_EQ(sm.state(), state_e::streaming_direct);
  }

  // ====================================================================
  // State machine: shutdown
  // ====================================================================

  TEST(StateMachine, ShutdownFromAnyNonTerminal) {
    for (auto setup_state : {state_e::starting, state_e::no_signal,
                             state_e::negotiating, state_e::streaming_direct,
                             state_e::streaming_rga, state_e::source_change}) {
      state_machine_t sm;
      switch (setup_state) {
        case state_e::starting:
          break;
        case state_e::no_signal:
          sm.enter_no_signal();
          break;
        case state_e::negotiating:
          sm.enter_no_signal();
          sm.enter_negotiating();
          break;
        case state_e::streaming_direct:
          sm.enter_no_signal();
          sm.enter_negotiating();
          for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
          sm.enter_streaming_direct();
          break;
        case state_e::streaming_rga:
          sm.enter_no_signal();
          sm.enter_negotiating();
          for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
          sm.enter_streaming_rga();
          break;
        case state_e::source_change:
          sm.enter_no_signal();
          sm.enter_negotiating();
          for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
          sm.enter_streaming_direct();
          sm.enter_source_change();
          break;
        default:
          break;
      }
      ASSERT_EQ(sm.state(), setup_state);
      EXPECT_TRUE(sm.enter_shutdown("clean shutdown"));
      EXPECT_EQ(sm.state(), state_e::shutdown);
    }
  }

  TEST(StateMachine, ShutdownFromTerminalIsFalse) {
    state_machine_t sm;
    sm.enter_shutdown();
    EXPECT_FALSE(sm.enter_shutdown("double shutdown"));
    EXPECT_EQ(sm.state(), state_e::shutdown);
  }

  TEST(StateMachine, FatalFromTerminalIsFalse) {
    state_machine_t sm;
    sm.enter_fatal("device error");
    EXPECT_FALSE(sm.enter_fatal("second fatal"));
    EXPECT_FALSE(sm.enter_shutdown());
    EXPECT_EQ(sm.state(), state_e::fatal);
  }

  // ====================================================================
  // State machine: fatal
  // ====================================================================

  TEST(StateMachine, FatalFromAnyNonTerminal) {
    state_machine_t sm;
    EXPECT_TRUE(sm.enter_fatal("device error"));
    EXPECT_EQ(sm.state(), state_e::fatal);
  }

  TEST(StateMachine, NoTransitionsAfterFatal) {
    state_machine_t sm;
    sm.enter_fatal();
    EXPECT_FALSE(sm.enter_no_signal());
    EXPECT_FALSE(sm.enter_negotiating());
    EXPECT_FALSE(sm.enter_streaming_direct());
    EXPECT_FALSE(sm.enter_streaming_rga());
    EXPECT_FALSE(sm.enter_source_change());
    EXPECT_FALSE(sm.enter_shutdown());
    EXPECT_EQ(sm.state(), state_e::fatal);
  }

  // ====================================================================
  // State machine: IDR requests
  // ====================================================================

  TEST(StateMachine, FirstFrameRequestsIDR) {
    state_machine_t sm;
    EXPECT_TRUE(sm.consume_idr_request());
    EXPECT_FALSE(sm.consume_idr_request());
  }

  TEST(StateMachine, IDROnNoSignalEntry) {
    state_machine_t sm;
    sm.consume_idr_request();
    sm.enter_no_signal();
    EXPECT_TRUE(sm.consume_idr_request());
    EXPECT_FALSE(sm.consume_idr_request());
  }

  TEST(StateMachine, IDROnStreamingRecovery) {
    state_machine_t sm;
    sm.consume_idr_request();
    sm.enter_no_signal();
    sm.consume_idr_request();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    EXPECT_TRUE(sm.consume_idr_request());
    EXPECT_FALSE(sm.consume_idr_request());
  }

  TEST(StateMachine, IDROnSourceChangeAndRecovery) {
    state_machine_t sm;
    sm.consume_idr_request();
    sm.enter_no_signal();
    sm.consume_idr_request();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_direct();
    sm.consume_idr_request();
    sm.enter_source_change();
    EXPECT_TRUE(sm.consume_idr_request());
    sm.enter_no_signal();
    sm.consume_idr_request();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) sm.record_stable_timing();
    sm.enter_streaming_rga();
    EXPECT_TRUE(sm.consume_idr_request());
  }

  // ====================================================================
  // State machine: shutdown interrupts wait
  // ====================================================================

  TEST(StateMachine, ShutdownInterruptsWait) {
    state_machine_t sm;
    auto start = std::chrono::steady_clock::now();
    std::thread shutdown_thread([&sm] {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      sm.enter_shutdown("test shutdown");
    });
    bool result = sm.wait_for_interval();
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_FALSE(result);
    EXPECT_LT(elapsed, k_placeholder_interval);
    shutdown_thread.join();
  }

  TEST(StateMachine, WaitReturnsImmediatelyAfterShutdown) {
    state_machine_t sm;
    sm.enter_shutdown();
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(sm.wait_for_interval());
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, std::chrono::milliseconds(10));
  }

  TEST(StateMachine, WaitCompletesNormallyWithoutShutdown) {
    state_machine_t sm;
    auto start = std::chrono::steady_clock::now();
    bool result = sm.wait_for_interval();
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(result);
    EXPECT_GE(elapsed, k_placeholder_interval - std::chrono::milliseconds(50));
  }

  // ====================================================================
  // State machine: source change storms
  // ====================================================================

  TEST(StateMachine, SourceChangeStormResetsStableCount) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 0; i < k_stable_timing_count - 1; ++i) {
      sm.record_stable_timing();
    }
    sm.enter_source_change("storm 1");
    sm.enter_negotiating("re-negotiate");
    EXPECT_FALSE(sm.timing_is_stable());
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) {
      sm.record_stable_timing();
    }
    EXPECT_TRUE(sm.timing_is_stable());
    EXPECT_TRUE(sm.enter_streaming_direct());
  }

  TEST(StateMachine, NegotiatingSourceChangeStorm) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (int i = 0; i < 10; ++i) {
      sm.enter_source_change("storm");
      sm.enter_negotiating("re-negotiate");
      EXPECT_FALSE(sm.timing_is_stable());
    }
    for (std::uint32_t i = 0; i < k_stable_timing_count; ++i) {
      sm.record_stable_timing();
    }
    EXPECT_TRUE(sm.enter_streaming_rga("settled"));
    EXPECT_EQ(sm.state(), state_e::streaming_rga);
  }

  // ====================================================================
  // State machine: stable timing counter
  // ====================================================================

  TEST(StateMachine, StableTimingCounterIncrementsCorrectly) {
    state_machine_t sm;
    sm.enter_no_signal();
    sm.enter_negotiating();
    for (std::uint32_t i = 1; i <= k_stable_timing_count; ++i) {
      auto count = sm.record_stable_timing();
      EXPECT_EQ(count, i);
    }
    EXPECT_TRUE(sm.timing_is_stable());
  }

  TEST(StateMachine, StableTimingIgnoredOutsideNegotiating) {
    state_machine_t sm;
    auto count = sm.record_stable_timing();
    EXPECT_EQ(count, 0U);
  }

  // ====================================================================
  // State machine: transition count
  // ====================================================================

  TEST(StateMachine, TransitionCountIncrements) {
    state_machine_t sm;
    EXPECT_EQ(sm.transition_count(), 0U);
    sm.enter_no_signal();
    EXPECT_EQ(sm.transition_count(), 1U);
    sm.enter_negotiating();
    sm.enter_streaming_direct();  // invalid without stable timing -> no change
    EXPECT_EQ(sm.transition_count(), 2U);



  }

  // ====================================================================
  // Thread safety: concurrent transitions
  // ====================================================================

  TEST(StateMachine, ConcurrentShutdownIsIdempotent) {
    state_machine_t sm;
    sm.enter_no_signal();
    std::atomic<int> success_count {0};
    auto try_shutdown = [&] {
      if (sm.enter_shutdown("race")) {
        success_count.fetch_add(1);
      }
    };
    std::thread t1(try_shutdown);
    std::thread t2(try_shutdown);
    t1.join();
    t2.join();
    EXPECT_EQ(success_count.load(), 1);
    EXPECT_EQ(sm.state(), state_e::shutdown);
  }

}  // namespace

#endif
