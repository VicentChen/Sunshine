/**
 * @file tests/unit/test_xbox_remote_input_queue.cpp
 * @brief Offline tests for bounded Xbox input scheduling and packetization.
 */

// standard includes
#include <chrono>
#include <cstdint>
#include <limits>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/xbox_remote/input_queue.h"

namespace {
  using namespace std::chrono_literals;
  using xbox_remote::input::item_kind_e;
  using xbox_remote::input::outbound_queue_t;
  using xbox_remote::protocol::gamepad_button_e;
  using xbox_remote::protocol::gamepad_frame_t;

  const auto epoch = outbound_queue_t::time_point_t {};

  gamepad_frame_t with_button(gamepad_button_e button) {
    gamepad_frame_t frame;
    frame.button_mask = static_cast<std::uint16_t>(button);
    return frame;
  }
}  // namespace

TEST(XboxRemoteInputQueueTest, LatestAnalogStateWinsWithoutGrowingQueue) {
  outbound_queue_t queue;
  gamepad_frame_t frame;
  ASSERT_TRUE(queue.attach(frame, epoch));
  ASSERT_EQ(queue.take(epoch)->kind, item_kind_e::attach);
  ASSERT_EQ(queue.take(epoch)->kind, item_kind_e::state);

  for (std::int16_t value = 1; value <= 100; ++value) {
    frame.left_stick_x = value;
    queue.submit(frame, epoch + 1ms);
  }
  EXPECT_EQ(queue.edge_count(), 0);
  const auto latest = queue.take(epoch + 1ms);
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->frame.left_stick_x, 100);
  EXPECT_FALSE(queue.take(epoch + 1ms));
}

TEST(XboxRemoteInputQueueTest, PreservesPressReleaseEdgesUntilExpiry) {
  outbound_queue_t queue;
  ASSERT_TRUE(queue.attach({}, epoch));
  ASSERT_TRUE(queue.take(epoch));
  ASSERT_TRUE(queue.take(epoch));

  queue.submit(with_button(gamepad_button_e::a), epoch + 1ms);
  queue.submit({}, epoch + 2ms);
  queue.submit(with_button(gamepad_button_e::b), epoch + 3ms);
  ASSERT_EQ(queue.edge_count(), 3);
  EXPECT_EQ(queue.take(epoch + 49ms)->frame.button_mask, static_cast<std::uint16_t>(gamepad_button_e::a));
  EXPECT_EQ(queue.take(epoch + 49ms)->frame.button_mask, 0);
  EXPECT_EQ(queue.take(epoch + 49ms)->frame.button_mask, static_cast<std::uint16_t>(gamepad_button_e::b));

  queue.submit({}, epoch + 60ms);
  queue.submit(with_button(gamepad_button_e::x), epoch + 61ms);
  const auto expired_latest = queue.take(epoch + 112ms);
  ASSERT_TRUE(expired_latest);
  EXPECT_EQ(expired_latest->frame.button_mask, static_cast<std::uint16_t>(gamepad_button_e::x));
  EXPECT_FALSE(queue.take(epoch + 112ms));
}

TEST(XboxRemoteInputQueueTest, BoundsEdgeJournalAndReportsOverflow) {
  xbox_remote::input::queue_options_t options;
  options.edge_capacity = 2;
  outbound_queue_t queue(options);
  ASSERT_TRUE(queue.attach({}, epoch));
  ASSERT_TRUE(queue.take(epoch));
  ASSERT_TRUE(queue.take(epoch));

  queue.submit(with_button(gamepad_button_e::a), epoch + 1ms);
  queue.submit({}, epoch + 2ms);
  queue.submit(with_button(gamepad_button_e::b), epoch + 3ms);
  EXPECT_EQ(queue.edge_count(), 2);
  EXPECT_EQ(queue.dropped_edges(), 1);
  EXPECT_EQ(queue.take(epoch + 3ms)->frame.button_mask, 0);
  EXPECT_EQ(queue.take(epoch + 3ms)->frame.button_mask, static_cast<std::uint16_t>(gamepad_button_e::b));

  xbox_remote::input::queue_options_t no_edges_options;
  no_edges_options.edge_capacity = 0;
  outbound_queue_t no_edges(no_edges_options);
  ASSERT_TRUE(no_edges.attach({}, epoch));
  ASSERT_TRUE(no_edges.take(epoch));
  ASSERT_TRUE(no_edges.take(epoch));
  no_edges.submit(with_button(gamepad_button_e::a), epoch + 1ms);
  EXPECT_EQ(no_edges.edge_count(), 0);
  EXPECT_EQ(no_edges.dropped_edges(), 1);
}

TEST(XboxRemoteInputQueueTest, NeutralizeDetachAndReconnectDiscardOldTransitions) {
  outbound_queue_t queue;
  ASSERT_TRUE(queue.attach({}, epoch));
  ASSERT_TRUE(queue.take(epoch));
  ASSERT_TRUE(queue.take(epoch));
  queue.submit(with_button(gamepad_button_e::a), epoch + 1ms);
  ASSERT_TRUE(queue.neutralize(epoch + 2ms));
  const auto neutral = queue.take(epoch + 2ms);
  ASSERT_TRUE(neutral);
  EXPECT_EQ(neutral->kind, item_kind_e::neutralize);
  EXPECT_EQ(neutral->frame.button_mask, 0);
  EXPECT_FALSE(queue.take(epoch + 2ms));

  queue.submit(with_button(gamepad_button_e::b), epoch + 3ms);
  queue.submit({}, epoch + 4ms);
  queue.on_reconnect();
  const auto current = queue.take(epoch + 4ms);
  ASSERT_TRUE(current);
  EXPECT_EQ(current->kind, item_kind_e::state);
  EXPECT_EQ(current->frame.button_mask, 0);
  EXPECT_FALSE(queue.take(epoch + 4ms));

  ASSERT_TRUE(queue.detach(epoch + 5ms));
  EXPECT_EQ(queue.take(epoch + 5ms)->kind, item_kind_e::detach);
  EXPECT_FALSE(queue.take(epoch + 5ms));
}

TEST(XboxRemoteInputQueueTest, RejectsControlOverflowWithoutBlocking) {
  xbox_remote::input::queue_options_t options;
  options.control_capacity = 0;
  outbound_queue_t queue(options);
  EXPECT_FALSE(queue.attach({}, epoch));
  EXPECT_FALSE(queue.neutralize(epoch));
  EXPECT_FALSE(queue.detach(epoch));
  EXPECT_FALSE(queue.take(epoch));
}

TEST(XboxRemoteInputQueueTest, AssignsSequenceAtPacketizationAndWraps) {
  xbox_remote::input::packetizer_t packetizer(std::numeric_limits<std::uint32_t>::max());
  const auto first = xbox_remote::protocol::decode_gamepad_packet(packetizer.encode({}, 12.5));
  ASSERT_TRUE(first);
  EXPECT_EQ(first.value.first.sequence, std::numeric_limits<std::uint32_t>::max());
  EXPECT_DOUBLE_EQ(first.value.first.timestamp_ms, 12.5);
  const auto second = xbox_remote::protocol::decode_gamepad_packet(packetizer.encode({}, 13.5));
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value.first.sequence, 0);
  EXPECT_EQ(packetizer.next_sequence(), 1);
  packetizer.reset(7);
  EXPECT_EQ(packetizer.next_sequence(), 7);
}

TEST(XboxRemoteInputQueueTest, MapsTriggersAndAllPhysicalActivity) {
  EXPECT_EQ(xbox_remote::input::expand_trigger(0), 0);
  EXPECT_EQ(xbox_remote::input::expand_trigger(1), 257);
  EXPECT_EQ(xbox_remote::input::expand_trigger(254), 65278);
  EXPECT_EQ(xbox_remote::input::expand_trigger(255), 65535);

  gamepad_frame_t frame;
  frame.button_mask = std::numeric_limits<std::uint16_t>::max();
  frame.left_trigger = 1;
  frame.right_trigger = 1;
  frame.left_stick_x = 1;
  frame.left_stick_y = -1;
  frame.right_stick_x = 1;
  frame.right_stick_y = -1;
  EXPECT_EQ(xbox_remote::input::activity_mask(frame), 0x003ff7ffU);
  EXPECT_EQ(xbox_remote::input::activity_mask({}), 0U);
}
