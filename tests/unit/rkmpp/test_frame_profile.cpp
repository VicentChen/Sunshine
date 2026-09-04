/**
 * @file tests/unit/rkmpp/test_frame_profile.cpp
 * @brief Unit tests for bounded per-frame performance statistics.
 */
#include "tests/tests_common.h"

#include <src/frame_profile.h>

namespace {
  video::frame_profile_t complete_profile(std::int64_t base_us = 0) {
    using namespace std::chrono;
    const auto base = video::frame_profile_t::time_point(microseconds(base_us));
    video::frame_profile_t profile;
    profile.capture = base;
    profile.dequeued = base + microseconds(100);
    profile.capture_queue_exit = base + microseconds(150);
    profile.rga_used = true;
    profile.rga_begin = base + microseconds(150);
    profile.rga_end = base + microseconds(250);
    profile.mpp_import_begin = base + microseconds(251);
    profile.mpp_import_end = base + microseconds(255);
    profile.mpp_output_buffer_begin = base + microseconds(256);
    profile.mpp_output_buffer_end = base + microseconds(257);
    profile.mpp_output_packet_begin = base + microseconds(258);
    profile.mpp_output_packet_end = base + microseconds(259);
    profile.mpp_prep_begin = base + microseconds(260);
    profile.mpp_prep_end = base + microseconds(270);
    profile.mpp_encode_begin = base + microseconds(270);
    profile.mpp_encode_end = base + microseconds(480);
    profile.mpp_packet_get_begin = base + microseconds(480);
    profile.mpp_packet_get_end = base + microseconds(500);
    profile.mpp_output = base + microseconds(500);
    profile.packetize_begin = base + microseconds(550);
    profile.send_end = base + microseconds(650);
    return profile;
  }
}  // namespace

TEST(FrameProfileWindow, CalculatesEveryStage) {
  video::frame_profile_window_t window;
  auto profile = complete_profile();
  profile.hdmirx_width = 1920;
  profile.hdmirx_height = 1080;
  profile.moonlight_width = 1920;
  profile.moonlight_height = 1080;
  window.collect(profile);
  const auto snapshot = window.snapshot_and_reset();

  EXPECT_EQ(snapshot.captured_frames, 1);
  EXPECT_EQ(snapshot.hdmirx_width, 1920U);
  EXPECT_EQ(snapshot.hdmirx_height, 1080U);
  EXPECT_EQ(snapshot.moonlight_width, 1920U);
  EXPECT_EQ(snapshot.moonlight_height, 1080U);
  EXPECT_EQ(snapshot.placeholder_frames, 0);
  EXPECT_EQ(snapshot.rga_bypass_frames, 0);
  EXPECT_EQ(snapshot.freshness_drops, 0U);
  const auto metric = [&](video::frame_profile_metric_e value) -> const auto & {
    return snapshot.metrics[static_cast<std::size_t>(value)];
  };
  EXPECT_EQ(metric(video::frame_profile_metric_e::rx_driver_age).p50_us, 100);
  EXPECT_EQ(metric(video::frame_profile_metric_e::capture_queue).p50_us, 50);
  EXPECT_EQ(metric(video::frame_profile_metric_e::rga).p50_us, 100);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_import).p50_us, 4);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_output_buffer_acquire).p50_us, 1);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_output_packet_init).p50_us, 1);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_prep).p50_us, 10);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_encode).p50_us, 210);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_packet_get).p50_us, 20);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_total).p50_us, 240);
  EXPECT_EQ(metric(video::frame_profile_metric_e::encoded_queue).p50_us, 50);
  EXPECT_EQ(metric(video::frame_profile_metric_e::packetize_send).p50_us, 100);
  EXPECT_EQ(metric(video::frame_profile_metric_e::protocol_host).p50_us, 550);
  EXPECT_EQ(metric(video::frame_profile_metric_e::host_send).p50_us, 650);
}

TEST(FrameProfileWindow, SeparatesBypassPlaceholderAndInvalidSamples) {
  video::frame_profile_window_t window;
  auto bypass = complete_profile();
  bypass.rga_used = false;
  window.collect(bypass);

  video::frame_profile_t placeholder;
  placeholder.kind = video::frame_profile_kind_e::placeholder;
  window.collect(placeholder);

  auto invalid = complete_profile();
  invalid.mpp_output = *invalid.mpp_prep_begin - std::chrono::microseconds(1);
  window.collect(invalid);

  const auto snapshot = window.snapshot_and_reset();
  const auto rga = snapshot.metrics[static_cast<std::size_t>(video::frame_profile_metric_e::rga)];
  const auto encode = snapshot.metrics[static_cast<std::size_t>(video::frame_profile_metric_e::mpp_total)];
  EXPECT_EQ(snapshot.captured_frames, 2);
  EXPECT_EQ(snapshot.placeholder_frames, 1);
  EXPECT_EQ(snapshot.rga_bypass_frames, 1);
  EXPECT_EQ(rga.count, 1);
  EXPECT_EQ(encode.count, 1);
  EXPECT_EQ(encode.invalid, 1);
}

TEST(FrameProfileWindow, AggregatesFreshnessDropsOnlyForCapturedFrames) {
  video::frame_profile_window_t window;
  auto first = complete_profile();
  first.freshness_drops = 2;
  window.collect(first);

  auto second = complete_profile();
  second.freshness_drops = 3;
  window.collect(second);

  video::frame_profile_t placeholder;
  placeholder.kind = video::frame_profile_kind_e::placeholder;
  placeholder.freshness_drops = 99;
  window.collect(placeholder);

  EXPECT_EQ(window.snapshot_and_reset().freshness_drops, 5U);
}

TEST(FrameProfileWindow, CalculatesNearestRankPercentiles) {
  video::frame_profile_window_t window;
  for (std::int64_t sample = 1; sample <= 100; ++sample) {
    auto profile = complete_profile(sample * 1000);
    profile.mpp_prep_begin = video::frame_profile_t::time_point(std::chrono::microseconds(0));
    profile.mpp_output = video::frame_profile_t::time_point(std::chrono::microseconds(sample));
    window.collect(profile);
  }
  const auto snapshot = window.snapshot_and_reset();
  const auto metric = snapshot.metrics[static_cast<std::size_t>(video::frame_profile_metric_e::mpp_total)];
  EXPECT_EQ(metric.count, 100);
  EXPECT_EQ(metric.minimum_us, 1);
  EXPECT_EQ(metric.p50_us, 50);
  EXPECT_EQ(metric.p95_us, 95);
  EXPECT_EQ(metric.p99_us, 99);
  EXPECT_EQ(metric.maximum_us, 100);
}

TEST(FrameProfileProtocolLatency, RoundsAndSaturates) {
  using namespace std::chrono;
  EXPECT_EQ(video::duration_to_protocol_latency(microseconds(-1)), 0);
  EXPECT_EQ(video::duration_to_protocol_latency(microseconds(49)), 0);
  EXPECT_EQ(video::duration_to_protocol_latency(microseconds(50)), 1);
  EXPECT_EQ(video::duration_to_protocol_latency(microseconds(149)), 1);
  EXPECT_EQ(video::duration_to_protocol_latency(microseconds(150)), 2);
  EXPECT_EQ(video::duration_to_protocol_latency(seconds(60)), std::numeric_limits<std::uint16_t>::max());
}

TEST(FrameProfileSnapshotStore, PublishesOnlyNewGenerations) {
  auto &store = video::frame_profile_snapshot_store();
  video::frame_profile_snapshot_t published;
  published.captured_frames = 123;
  const auto published_generation = store.publish(published);

  std::uint64_t generation = published_generation - 1U;
  video::frame_profile_snapshot_t received;
  EXPECT_TRUE(store.read_newer(generation, received));
  EXPECT_EQ(generation, published_generation);
  EXPECT_EQ(received.captured_frames, 123U);
  EXPECT_FALSE(store.read_newer(generation, received));
}

TEST(FrameProfileTimeline, PreservesRelativeStageBoundsAndOverlap) {
  using namespace std::chrono;
  auto profile = complete_profile(1000000);
  const auto origin = *profile.capture;
  profile.frame_index = 42;
  profile.capture_sequence = 7;
  profile.ui_render_begin = origin + microseconds(155);
  profile.ui_render_end = origin + microseconds(225);
  profile.ui_compose_begin = origin + microseconds(225);
  profile.ui_compose_end = origin + microseconds(245);

  const auto frame = video::make_frame_profile_timeline_frame(profile, origin - milliseconds(10));
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->frame_index, 42);
  EXPECT_EQ(frame->capture_sequence, 7U);
  EXPECT_EQ(frame->origin_offset_us, 10000);
  EXPECT_EQ(frame->end_us, 650);
  EXPECT_EQ(frame->missing_stage_mask, 0U);
  EXPECT_EQ(frame->invalid_stage_mask, 0U);
  EXPECT_EQ(frame->span_count, video::frame_profile_timeline_frame_t::max_spans);

  const auto find = [&](video::frame_profile_timeline_stage_e stage) -> const video::frame_profile_timeline_span_t * {
    for (std::size_t index = 0; index < frame->span_count; ++index) {
      if (frame->spans[index].stage == stage) {
        return &frame->spans[index];
      }
    }
    return nullptr;
  };
  const auto *rga = find(video::frame_profile_timeline_stage_e::rga);
  const auto *ui_render = find(video::frame_profile_timeline_stage_e::ui_render);
  const auto *mpp_prep = find(video::frame_profile_timeline_stage_e::mpp_prep);
  const auto *mpp_encode = find(video::frame_profile_timeline_stage_e::mpp_encode);
  const auto *mpp_packet_get = find(video::frame_profile_timeline_stage_e::mpp_packet_get);
  ASSERT_NE(rga, nullptr);
  ASSERT_NE(ui_render, nullptr);
  ASSERT_NE(mpp_prep, nullptr);
  ASSERT_NE(mpp_encode, nullptr);
  ASSERT_NE(mpp_packet_get, nullptr);
  EXPECT_EQ(rga->start_us, 150);
  EXPECT_EQ(rga->end_us, 250);
  EXPECT_EQ(ui_render->start_us, 155);
  EXPECT_EQ(ui_render->end_us, 225);
  EXPECT_EQ(rga->lane, video::frame_profile_timeline_lane_e::rga);
  EXPECT_EQ(ui_render->lane, video::frame_profile_timeline_lane_e::ui);
  EXPECT_EQ(mpp_prep->end_us, mpp_encode->start_us);
  EXPECT_EQ(mpp_encode->end_us, mpp_packet_get->start_us);
  EXPECT_EQ(mpp_packet_get->end_us - mpp_prep->start_us, 240);
}

TEST(FrameProfileTimeline, MarksMissingAndInvalidStagesWithoutCreatingZeroBars) {
  auto profile = complete_profile();
  profile.rga_used = false;
  profile.mpp_import_end.reset();
  profile.packetize_begin = *profile.send_end + std::chrono::microseconds(1);

  const auto frame = video::make_frame_profile_timeline_frame(profile, *profile.capture);
  ASSERT_TRUE(frame);
  const auto import_bit = 1U << static_cast<std::uint8_t>(video::frame_profile_timeline_stage_e::mpp_import);
  const auto encoded_queue_bit = 1U << static_cast<std::uint8_t>(video::frame_profile_timeline_stage_e::encoded_queue);
  const auto packet_send_bit = 1U << static_cast<std::uint8_t>(video::frame_profile_timeline_stage_e::packetize_send);
  EXPECT_TRUE(frame->rga_bypass);
  EXPECT_NE(frame->missing_stage_mask & import_bit, 0U);
  EXPECT_NE(frame->invalid_stage_mask & encoded_queue_bit, 0U);
  EXPECT_NE(frame->invalid_stage_mask & packet_send_bit, 0U);
}

TEST(FrameProfileTimelineStore, KeepsNewestFramesInOldestToNewestOrder) {
  auto &store = video::frame_profile_timeline_store();
  store.reset();
  for (std::size_t index = 0; index < video::frame_profile_timeline_snapshot_t::frame_capacity + 3U; ++index) {
    auto profile = complete_profile(static_cast<std::int64_t>(index * 1000U));
    profile.frame_index = static_cast<std::int64_t>(index);
    ASSERT_TRUE(store.publish(profile));
  }

  std::uint64_t generation {};
  video::frame_profile_timeline_snapshot_t snapshot;
  ASSERT_TRUE(store.read_newer(generation, snapshot));
  ASSERT_EQ(snapshot.frame_count, video::frame_profile_timeline_snapshot_t::frame_capacity);
  EXPECT_EQ(snapshot.frames.front().frame_index, 3);
  EXPECT_EQ(snapshot.frames[snapshot.frame_count - 1U].frame_index, static_cast<std::int64_t>(video::frame_profile_timeline_snapshot_t::frame_capacity + 2U));
  EXPECT_FALSE(store.read_newer(generation, snapshot));
}
