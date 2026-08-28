/**
 * @file tests/unit/test_frame_profile.cpp
 * @brief Unit tests for bounded per-frame performance statistics.
 */
#include "../tests_common.h"

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
    profile.mpp_submit_begin = base + microseconds(260);
    profile.mpp_submit_end = base + microseconds(280);
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
  const auto metric = [&](video::frame_profile_metric_e value) -> const auto & {
    return snapshot.metrics[static_cast<std::size_t>(value)];
  };
  EXPECT_EQ(metric(video::frame_profile_metric_e::rx_driver_age).p50_us, 100);
  EXPECT_EQ(metric(video::frame_profile_metric_e::capture_queue).p50_us, 50);
  EXPECT_EQ(metric(video::frame_profile_metric_e::rga).p50_us, 100);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_submit).p50_us, 20);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_output_wait).p50_us, 220);
  EXPECT_EQ(metric(video::frame_profile_metric_e::mpp_encode).p50_us, 240);
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
  invalid.mpp_output = *invalid.mpp_submit_begin - std::chrono::microseconds(1);
  window.collect(invalid);

  const auto snapshot = window.snapshot_and_reset();
  const auto rga = snapshot.metrics[static_cast<std::size_t>(video::frame_profile_metric_e::rga)];
  const auto encode = snapshot.metrics[static_cast<std::size_t>(video::frame_profile_metric_e::mpp_encode)];
  EXPECT_EQ(snapshot.captured_frames, 2);
  EXPECT_EQ(snapshot.placeholder_frames, 1);
  EXPECT_EQ(snapshot.rga_bypass_frames, 1);
  EXPECT_EQ(rga.count, 1);
  EXPECT_EQ(encode.count, 1);
  EXPECT_EQ(encode.invalid, 1);
}

TEST(FrameProfileWindow, CalculatesNearestRankPercentiles) {
  video::frame_profile_window_t window;
  for (std::int64_t sample = 1; sample <= 100; ++sample) {
    auto profile = complete_profile(sample * 1000);
    profile.mpp_submit_begin = video::frame_profile_t::time_point(std::chrono::microseconds(0));
    profile.mpp_output = video::frame_profile_t::time_point(std::chrono::microseconds(sample));
    window.collect(profile);
  }
  const auto snapshot = window.snapshot_and_reset();
  const auto metric = snapshot.metrics[static_cast<std::size_t>(video::frame_profile_metric_e::mpp_encode)];
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
