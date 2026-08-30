/**
 * @file tests/unit/test_xbox_remote_startup.cpp
 * @brief Offline tests for the Xbox data-channel startup coordinator.
 */

// standard includes
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// local includes
#include "src/xbox_remote/startup.h"

namespace {
  using namespace std::chrono_literals;
  using xbox_remote::startup::coordinator_t;
  using xbox_remote::startup::error_e;
  using xbox_remote::startup::sender_t;
  using xbox_remote::startup::state_e;

  struct event_t {
    std::string channel;
    std::string text;
    std::vector<std::uint8_t> binary;
  };

  class fake_sender_t final: public sender_t {
  public:
    bool send_text(std::string_view channel, std::string_view payload) override {
      events.push_back({std::string(channel), std::string(payload), {}});
      return !fail_at || events.size() != *fail_at;
    }

    bool send_binary(std::string_view channel, const std::vector<std::uint8_t> &payload) override {
      events.push_back({std::string(channel), {}, payload});
      return !fail_at || events.size() != *fail_at;
    }

    std::vector<event_t> events;
    std::optional<std::size_t> fail_at;
  };

  const auto epoch = coordinator_t::time_point_t {};
  const std::string ack = R"({"type":"HandshakeAck","version":"messageV1"})";
}  // namespace

TEST(XboxRemoteStartupTest, SendsExactOrderedStartupSequenceAfterDelay) {
  fake_sender_t sender;
  xbox_remote::startup::options_t options;
  options.handshake_id = "fixture-handshake";
  options.startup_messages.install_id = "00000000-0000-0000-0000-000000000123";
  coordinator_t coordinator(sender, options);

  ASSERT_TRUE(coordinator.start(epoch));
  ASSERT_EQ(sender.events.size(), 1);
  EXPECT_EQ(sender.events[0].channel, "message");
  EXPECT_EQ(nlohmann::json::parse(sender.events[0].text)["type"], "Handshake");

  const auto transitioned = coordinator.on_message(ack, epoch + 10ms);
  ASSERT_TRUE(transitioned);
  EXPECT_TRUE(transitioned.value);
  ASSERT_EQ(sender.events.size(), 3);
  EXPECT_EQ(sender.events[1].channel, "control");
  EXPECT_EQ(nlohmann::json::parse(sender.events[1].text)["message"], "authorizationRequest");
  EXPECT_FALSE(nlohmann::json::parse(sender.events[2].text)["wasAdded"].get<bool>());

  ASSERT_TRUE(coordinator.poll(epoch + 509ms, false));
  EXPECT_EQ(sender.events.size(), 3);
  const auto ready = coordinator.poll(epoch + 510ms, false);
  ASSERT_TRUE(ready);
  EXPECT_TRUE(ready.value);
  EXPECT_EQ(coordinator.state(), state_e::ready);
  ASSERT_EQ(sender.events.size(), 11);
  EXPECT_EQ(sender.events[3].channel, "control");
  EXPECT_TRUE(nlohmann::json::parse(sender.events[3].text)["wasAdded"].get<bool>());
  for (std::size_t index = 4; index < 10; ++index) {
    EXPECT_EQ(sender.events[index].channel, "message");
  }
  EXPECT_EQ(sender.events[10].channel, "input");
  EXPECT_EQ(sender.events[10].binary.size(), xbox_remote::protocol::client_metadata_packet_size);
}

TEST(XboxRemoteStartupTest, IgnoresDuplicateAckWithoutRepeatingAdd) {
  fake_sender_t sender;
  coordinator_t coordinator(sender);
  ASSERT_TRUE(coordinator.start(epoch));
  ASSERT_TRUE(coordinator.on_message(ack, epoch));
  ASSERT_TRUE(coordinator.poll(epoch + 500ms, false));
  const auto count = sender.events.size();

  const auto duplicate = coordinator.on_message(ack, epoch + 600ms);
  ASSERT_TRUE(duplicate);
  EXPECT_FALSE(duplicate.value);
  EXPECT_EQ(sender.events.size(), count);

  const auto unrelated = coordinator.on_message(R"({"type":"status"})", epoch + 700ms);
  ASSERT_TRUE(unrelated);
  EXPECT_FALSE(unrelated.value);
  EXPECT_EQ(sender.events.size(), count);
}

TEST(XboxRemoteStartupTest, CoversAckTimeoutInvalidVersionAndCancellation) {
  fake_sender_t timeout_sender;
  coordinator_t timeout_coordinator(timeout_sender);
  ASSERT_TRUE(timeout_coordinator.start(epoch));
  EXPECT_EQ(timeout_coordinator.poll(epoch + 4999ms, false).error.code, error_e::none);
  EXPECT_EQ(timeout_coordinator.poll(epoch + 5000ms, false).error.code, error_e::timeout);

  fake_sender_t invalid_sender;
  coordinator_t invalid_coordinator(invalid_sender);
  ASSERT_TRUE(invalid_coordinator.start(epoch));
  EXPECT_EQ(
    invalid_coordinator.on_message(R"({"type":"HandshakeAck","version":"messageV2"})", epoch).error.code,
    error_e::invalid_ack
  );

  fake_sender_t cancelled_sender;
  coordinator_t cancelled_coordinator(cancelled_sender);
  ASSERT_TRUE(cancelled_coordinator.start(epoch));
  EXPECT_EQ(cancelled_coordinator.poll(epoch, true).error.code, error_e::cancelled);
  EXPECT_EQ(cancelled_coordinator.state(), state_e::cancelled);
  EXPECT_EQ(cancelled_coordinator.poll(epoch, false).error.code, error_e::cancelled);
  EXPECT_EQ(cancelled_coordinator.on_channel_closed("message").error.code, error_e::cancelled);
}

TEST(XboxRemoteStartupTest, CoversSendFailuresAndChannelClosure) {
  for (std::size_t fail_at = 1; fail_at <= 11; ++fail_at) {
    fake_sender_t sender;
    sender.fail_at = fail_at;
    coordinator_t coordinator(sender);
    auto result = coordinator.start(epoch);
    if (fail_at == 1) {
      EXPECT_EQ(result.error.code, error_e::send_failed);
      continue;
    }
    ASSERT_TRUE(result);
    result = coordinator.on_message(ack, epoch);
    if (fail_at <= 3) {
      EXPECT_EQ(result.error.code, error_e::send_failed);
      continue;
    }
    ASSERT_TRUE(result);
    EXPECT_EQ(coordinator.poll(epoch + 500ms, false).error.code, error_e::send_failed);
  }

  fake_sender_t sender;
  coordinator_t coordinator(sender);
  ASSERT_TRUE(coordinator.start(epoch));
  EXPECT_EQ(coordinator.on_channel_closed("message").error.code, error_e::channel_closed);

  fake_sender_t unknown_sender;
  coordinator_t unknown_coordinator(unknown_sender);
  ASSERT_TRUE(unknown_coordinator.start(epoch));
  EXPECT_EQ(unknown_coordinator.on_channel_closed("optional").error.code, error_e::none);

  fake_sender_t chat_sender;
  coordinator_t chat_coordinator(chat_sender);
  ASSERT_TRUE(chat_coordinator.start(epoch));
  EXPECT_EQ(chat_coordinator.on_channel_closed("chat").error.code, error_e::none);
  EXPECT_EQ(chat_coordinator.state(), state_e::waiting_ack);
}

TEST(XboxRemoteStartupTest, RejectsInvalidLifecycleCallsWithoutLeakingPayloads) {
  fake_sender_t sender;
  coordinator_t coordinator(sender);
  EXPECT_EQ(coordinator.on_message(ack, epoch).error.code, error_e::invalid_state);

  fake_sender_t started_sender;
  coordinator_t started(started_sender);
  ASSERT_TRUE(started.start(epoch));
  EXPECT_EQ(started.start(epoch).error.code, error_e::invalid_state);
  EXPECT_EQ(started.failure().stage, "handshake");
  EXPECT_EQ(started.failure().message.find("HandshakeAck"), std::string::npos);
}
