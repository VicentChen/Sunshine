/**
 * @file tests/unit/xbox/test_xbox_remote_transport.cpp
 * @brief Unit tests for the isolated Xbox WebRTC transport gate.
 */

// standard includes
#include <chrono>
#include <string>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/xbox_remote/transport.h"

using namespace std::chrono_literals;

TEST(XboxRemoteTransportTest, AcceptsOutOfOrderReadyEventsAndConsumesMedia) {
  using namespace xbox_remote::transport;
  state_tracker_t tracker;
  tracker.observe_peer(peer_state_e::connected);
  for (const auto &channel : xbox_remote::protocol::data_channels) {
    tracker.observe_channel(channel.label, true);
  }
  EXPECT_FALSE(tracker.snapshot().ready());
  tracker.observe_ice(ice_state_e::connected);
  tracker.observe_media(true, 1200);
  tracker.observe_media(false, 240);
  const auto ready = tracker.wait_ready(10ms, {});
  ASSERT_TRUE(ready);
  const auto snapshot = tracker.snapshot();
  EXPECT_TRUE(snapshot.ready());
  EXPECT_EQ(snapshot.video_packets, 1);
  EXPECT_EQ(snapshot.video_bytes, 1200);
  EXPECT_EQ(snapshot.audio_packets, 1);
  EXPECT_EQ(snapshot.audio_bytes, 240);
}

TEST(XboxRemoteTransportTest, DeduplicatesCandidatesAndFailsAfterRequiredChannelCloses) {
  using namespace xbox_remote::transport;
  state_tracker_t tracker;
  EXPECT_TRUE(tracker.accept_remote_candidate("candidate:fixture", "video"));
  EXPECT_FALSE(tracker.accept_remote_candidate("candidate:fixture", "video"));
  EXPECT_TRUE(tracker.accept_remote_candidate("candidate:fixture", "audio"));
  EXPECT_EQ(tracker.snapshot().remote_candidate_count, 2);
  tracker.observe_peer(peer_state_e::connected);
  tracker.observe_ice(ice_state_e::completed);
  for (const auto &channel : xbox_remote::protocol::data_channels) {
    tracker.observe_channel(channel.label, true);
  }
  ASSERT_TRUE(tracker.snapshot().ready());
  tracker.observe_channel("input", false);
  EXPECT_EQ(tracker.snapshot().failure_stage, "data_channel");
  const auto failed = tracker.wait_ready(10ms, {});
  EXPECT_FALSE(failed);
  EXPECT_EQ(failed.error.code, error_e::peer_failed);
}

TEST(XboxRemoteTransportTest, AcceptsChatCloseAfterItHasOpened) {
  using namespace xbox_remote::transport;
  state_tracker_t tracker;
  tracker.observe_peer(peer_state_e::connected);
  tracker.observe_ice(ice_state_e::connected);
  tracker.observe_channel("chat", true);
  tracker.observe_channel("chat", false);
  tracker.observe_channel("message", true);
  tracker.observe_channel("input", true);
  tracker.observe_channel("control", true);

  const auto snapshot = tracker.snapshot();
  EXPECT_FALSE(snapshot.all_channels_open());
  EXPECT_TRUE(snapshot.all_channels_ever_opened());
  EXPECT_TRUE(snapshot.critical_channels_open());
  EXPECT_TRUE(snapshot.failure_stage.empty());
  EXPECT_TRUE(snapshot.ready());
  EXPECT_TRUE(tracker.wait_ready(10ms, {}));
}

TEST(XboxRemoteTransportTest, ReportsIceFailureCancellationAndTimeout) {
  using namespace xbox_remote::transport;
  state_tracker_t failed_tracker;
  failed_tracker.observe_ice(ice_state_e::failed);
  EXPECT_EQ(failed_tracker.wait_ready(10ms, {}).error.code, error_e::peer_failed);

  state_tracker_t dtls_tracker;
  dtls_tracker.observe_peer(peer_state_e::failed);
  EXPECT_EQ(dtls_tracker.snapshot().failure_stage, "peer");
  EXPECT_EQ(dtls_tracker.wait_ready(10ms, {}).error.code, error_e::peer_failed);

  state_tracker_t cancelled_tracker;
  EXPECT_EQ(cancelled_tracker.wait_ready(100ms, []() {
                               return true;
                             })
              .error.code,
            error_e::cancelled);

  state_tracker_t timeout_tracker;
  EXPECT_EQ(timeout_tracker.wait_ready(1ms, {}).error.code, error_e::timeout);
}

TEST(XboxRemoteTransportTest, CreatesCompleteOfflineOfferAndDestroysPeersRepeatedly) {
  using namespace xbox_remote::transport;
  {
    peer_t peer {{false}};
    EXPECT_FALSE(peer.send_text("message", "offline"));
    EXPECT_FALSE(peer.send_binary("input", {0x00}));
    EXPECT_FALSE(peer.take_message());
    const auto offer = peer.create_offer(5s, {});
    ASSERT_TRUE(offer) << offer.error.stage << ": " << offer.error.message;
    EXPECT_FALSE(offer.value.used_host_candidate_fallback);
    const auto video = offer.value.sdp.find("m=video ");
    const auto audio = offer.value.sdp.find("m=audio ");
    const auto application = offer.value.sdp.find("m=application ");
    ASSERT_NE(video, std::string::npos);
    ASSERT_NE(audio, std::string::npos);
    ASSERT_NE(application, std::string::npos);
    EXPECT_LT(video, audio);
    EXPECT_LT(audio, application);
    peer.close();

    for (int iteration = 0; iteration < 100; ++iteration) {
      peer_t repeated {{false}};
      repeated.close();
    }
  }
  cleanup_runtime();
}
