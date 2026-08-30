/**
 * @file src/xbox_remote/transport.cpp
 * @brief libdatachannel transport for the Xbox Remote Play compatibility probe.
 */

#include "src/xbox_remote/transport.h"

// standard includes
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

// library includes
#include <rtc/rtc.hpp>

namespace xbox_remote::transport {
  namespace {
    /**
     * @brief Return the protocol-array index for a required channel.
     *
     * @param label Channel label.
     * @return Index or no value for an unknown label.
     */
    std::optional<std::size_t> channel_index(std::string_view label) {
      for (std::size_t index = 0; index < protocol::data_channels.size(); ++index) {
        if (protocol::data_channels[index].label == label) {
          return index;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Check the generated offer without returning its sensitive contents.
     *
     * @param sdp Generated offer.
     * @return Empty string on success, otherwise a fixed failure reason.
     */
    std::string validate_offer(std::string_view sdp) {
      const auto video = sdp.find("m=video ");
      const auto audio = sdp.find("m=audio ");
      const auto application = sdp.find("m=application ");
      if (video == std::string_view::npos || audio == std::string_view::npos || application == std::string_view::npos || !(video < audio && audio < application)) {
        return "offer does not contain ordered video, audio, and application sections";
      }
      const auto video_end = audio;
      const auto audio_end = application;
      if (sdp.substr(video, video_end - video).find("a=recvonly") == std::string_view::npos) {
        return "video section is not recvonly";
      }
      if (sdp.substr(audio, audio_end - audio).find("a=recvonly") == std::string_view::npos) {
        return "audio section is not recvonly";
      }
      if (sdp.substr(video, video_end - video).find("H264/90000") == std::string_view::npos) {
        return "video section does not offer H264";
      }
      if (sdp.substr(audio, audio_end - audio).find("opus/48000") == std::string_view::npos) {
        return "audio section does not offer Opus";
      }
      return {};
    }

    peer_state_e translate(rtc::PeerConnection::State state) {
      switch (state) {
        case rtc::PeerConnection::State::New:
          return peer_state_e::created;
        case rtc::PeerConnection::State::Connecting:
          return peer_state_e::connecting;
        case rtc::PeerConnection::State::Connected:
          return peer_state_e::connected;
        case rtc::PeerConnection::State::Disconnected:
          return peer_state_e::disconnected;
        case rtc::PeerConnection::State::Failed:
          return peer_state_e::failed;
        case rtc::PeerConnection::State::Closed:
          return peer_state_e::closed;
      }
      return peer_state_e::failed;
    }

    ice_state_e translate(rtc::PeerConnection::IceState state) {
      switch (state) {
        case rtc::PeerConnection::IceState::New:
          return ice_state_e::created;
        case rtc::PeerConnection::IceState::Checking:
          return ice_state_e::checking;
        case rtc::PeerConnection::IceState::Connected:
          return ice_state_e::connected;
        case rtc::PeerConnection::IceState::Completed:
          return ice_state_e::completed;
        case rtc::PeerConnection::IceState::Disconnected:
          return ice_state_e::disconnected;
        case rtc::PeerConnection::IceState::Failed:
          return ice_state_e::failed;
        case rtc::PeerConnection::IceState::Closed:
          return ice_state_e::closed;
      }
      return ice_state_e::failed;
    }

    std::uint16_t mline_index(std::string_view mid) {
      if (mid == "audio") {
        return 1;
      }
      if (mid == "data" || mid == "application") {
        return 2;
      }
      return 0;
    }
  }  // namespace

  struct state_tracker_t::impl_t {
    mutable std::mutex mutex;  ///< Protects the snapshot and candidate set.
    mutable std::condition_variable changed;  ///< Wakes readiness waiters.
    snapshot_t state;  ///< Mutable tracked state.
    std::set<std::pair<std::string, std::string>> candidates;  ///< Candidate/mid deduplication keys.
  };

  bool snapshot_t::all_channels_open() const {
    return std::ranges::all_of(channels_open, [](bool value) {
      return value;
    });
  }

  bool snapshot_t::all_channels_ever_opened() const {
    return std::ranges::all_of(channels_ever_opened, [](bool value) {
      return value;
    });
  }

  bool snapshot_t::critical_channels_open() const {
    return channels_open[0] && channels_open[1] && channels_open[2];
  }

  bool snapshot_t::ready() const {
    return peer == peer_state_e::connected && (ice == ice_state_e::connected || ice == ice_state_e::completed) && all_channels_ever_opened() && critical_channels_open() && failure_stage.empty();
  }

  state_tracker_t::state_tracker_t():
      impl_(std::make_shared<impl_t>()) {
  }

  void state_tracker_t::observe_peer(peer_state_e state) const {
    {
      std::lock_guard lock {impl_->mutex};
      impl_->state.peer = state;
      if (state == peer_state_e::failed && impl_->state.failure_stage.empty()) {
        impl_->state.failure_stage = "peer";
      }
    }
    impl_->changed.notify_all();
  }

  void state_tracker_t::observe_ice(ice_state_e state) const {
    {
      std::lock_guard lock {impl_->mutex};
      impl_->state.ice = state;
      if (state == ice_state_e::failed && impl_->state.failure_stage.empty()) {
        impl_->state.failure_stage = "ice";
      }
    }
    impl_->changed.notify_all();
  }

  void state_tracker_t::observe_channel(std::string_view label, bool open) const {
    const auto index = channel_index(label);
    if (!index) {
      return;
    }
    {
      std::lock_guard lock {impl_->mutex};
      impl_->state.channels_open[*index] = open;
      if (open) {
        impl_->state.channels_ever_opened[*index] = true;
      }
      // XStreaming and its nano backend treat chat as passthrough. A console
      // may close it after successful negotiation when microphone chat is not
      // used; control, input, and message remain mandatory for this probe.
      if (!open && label != "chat" && impl_->state.peer == peer_state_e::connected && impl_->state.failure_stage.empty()) {
        impl_->state.failure_stage = "data_channel";
      }
    }
    impl_->changed.notify_all();
  }

  void state_tracker_t::observe_media(bool video, std::size_t bytes) const {
    std::lock_guard lock {impl_->mutex};
    if (video) {
      ++impl_->state.video_packets;
      impl_->state.video_bytes += bytes;
    } else {
      ++impl_->state.audio_packets;
      impl_->state.audio_bytes += bytes;
    }
  }

  bool state_tracker_t::accept_remote_candidate(std::string_view candidate, std::string_view mid) const {
    std::lock_guard lock {impl_->mutex};
    const auto [_, inserted] = impl_->candidates.emplace(std::string {candidate}, std::string {mid});
    impl_->state.remote_candidate_count = impl_->candidates.size();
    return inserted;
  }

  void state_tracker_t::fail(std::string stage) const {
    {
      std::lock_guard lock {impl_->mutex};
      if (impl_->state.failure_stage.empty()) {
        impl_->state.failure_stage = std::move(stage);
      }
    }
    impl_->changed.notify_all();
  }

  snapshot_t state_tracker_t::snapshot() const {
    std::lock_guard lock {impl_->mutex};
    return impl_->state;
  }

  result_t<bool> state_tracker_t::wait_ready(
    std::chrono::milliseconds timeout,
    const std::function<bool()> &cancelled
  ) const {
    result_t<bool> result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock {impl_->mutex};
    while (!impl_->state.ready() && impl_->state.failure_stage.empty()) {
      if (cancelled && cancelled()) {
        result.error = {error_e::cancelled, "wait_ready", "WebRTC readiness wait was cancelled"};
        return result;
      }
      if (impl_->changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds {100})) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline) {
        result.error = {error_e::timeout, "wait_ready", "WebRTC readiness deadline expired"};
        return result;
      }
    }
    if (!impl_->state.failure_stage.empty()) {
      result.error = {error_e::peer_failed, impl_->state.failure_stage, "WebRTC transport entered a terminal failure"};
      return result;
    }
    result.value = true;
    return result;
  }

  struct peer_t::impl_t {
    struct offer_state_t {
      std::mutex mutex;  ///< Protects gathering state and local candidates.
      std::condition_variable changed;  ///< Wakes offer waiters.
      bool gathering_complete = false;  ///< Whether ICE gathering completed.
      std::vector<protocol::ice_candidate_t> candidates;  ///< Gathered candidates.
    };

    struct incoming_state_t {
      std::mutex mutex;  ///< Protects the bounded in-memory receive queue.
      std::deque<channel_message_t> messages;  ///< Oldest-first channel messages.
      bool closed = false;  ///< Prevents callbacks from queuing during teardown.
    };

    state_tracker_t tracker;  ///< Callback-safe transport state.
    std::shared_ptr<offer_state_t> offer_state = std::make_shared<offer_state_t>();  ///< Offer callback state.
    std::shared_ptr<incoming_state_t> incoming_state = std::make_shared<incoming_state_t>();  ///< Callback-safe receive queue.
    std::shared_ptr<rtc::PeerConnection> peer;  ///< libdatachannel peer.
    std::vector<std::shared_ptr<rtc::Track>> tracks;  ///< No-op media receivers.
    std::vector<std::shared_ptr<rtc::DataChannel>> channels;  ///< Required data channels.
    bool use_public_stun = true;  ///< Whether this peer waits for the fixed public STUN servers.
    bool closed = false;  ///< Idempotent close guard.

    explicit impl_t(options_t options):
        use_public_stun(options.use_public_stun) {
      rtc::Configuration configuration;
      configuration.disableAutoNegotiation = true;
      configuration.forceMediaTransport = true;
      if (options.use_public_stun) {
        configuration.iceServers.emplace_back("stun:worldaz.relay.teams.microsoft.com:3478");
        configuration.iceServers.emplace_back("stun:stun.l.google.com:19302");
        configuration.iceServers.emplace_back("stun:stun1.l.google.com:19302");
      }
      peer = std::make_shared<rtc::PeerConnection>(std::move(configuration));
      const auto tracker_copy = tracker;
      peer->onStateChange([tracker_copy](rtc::PeerConnection::State state) {
        tracker_copy.observe_peer(translate(state));
      });
      peer->onIceStateChange([tracker_copy](rtc::PeerConnection::IceState state) {
        tracker_copy.observe_ice(translate(state));
      });
      const auto offer_state_copy = offer_state;
      peer->onLocalCandidate([offer_state_copy](rtc::Candidate candidate) {
        protocol::ice_candidate_t converted;
        converted.candidate = candidate.candidate();
        converted.sdp_mid = candidate.mid().empty() ? "video" : candidate.mid();
        converted.sdp_mline_index = mline_index(converted.sdp_mid);
        std::lock_guard lock {offer_state_copy->mutex};
        offer_state_copy->candidates.push_back(std::move(converted));
      });
      peer->onGatheringStateChange([offer_state_copy](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
          {
            std::lock_guard lock {offer_state_copy->mutex};
            offer_state_copy->gathering_complete = true;
          }
          offer_state_copy->changed.notify_all();
        }
      });

      rtc::Description::Video video {"video", rtc::Description::Direction::RecvOnly};
      video.addH264Codec(102, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f");
      video.addH264Codec(123, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
      video.addH264Codec(127, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=64001f");
      for (const int payload : video.payloadTypes()) {
        auto *mapping = video.rtpMap(payload);
        mapping->addFeedback("nack");
        mapping->addFeedback("nack pli");
        mapping->addFeedback("goog-remb");
      }
      video.addAttribute("rtcp-rsize");
      auto video_track = peer->addTrack(video);
      video_track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
      video_track->onMessage([tracker_copy](rtc::binary packet) {
        tracker_copy.observe_media(true, packet.size());
      },
                             nullptr);
      tracks.push_back(std::move(video_track));

      rtc::Description::Audio audio {"audio", rtc::Description::Direction::RecvOnly};
      audio.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
      auto audio_track = peer->addTrack(audio);
      audio_track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
      audio_track->onMessage([tracker_copy](rtc::binary packet) {
        tracker_copy.observe_media(false, packet.size());
      },
                             nullptr);
      tracks.push_back(std::move(audio_track));

      for (const auto &required : protocol::data_channels) {
        rtc::DataChannelInit init;
        init.negotiated = false;
        init.id = required.sid;
        init.protocol = std::string {required.protocol};
        auto channel = peer->createDataChannel(std::string {required.label}, std::move(init));
        const auto label = std::string {required.label};
        const auto expected_id = required.sid;
        const auto incoming_state_copy = incoming_state;
        channel->onOpen([tracker_copy, channel, label, expected_id]() {
          if (!channel->id() || *channel->id() != expected_id) {
            tracker_copy.fail("data_channel_sid");
            return;
          }
          tracker_copy.observe_channel(label, true);
        });
        channel->onClosed([tracker_copy, label]() {
          tracker_copy.observe_channel(label, false);
        });
        channel->onError([tracker_copy](std::string) {
          tracker_copy.fail("data_channel");
        });
        channel->onMessage(
          [tracker_copy, incoming_state_copy, label](rtc::binary data) {
            channel_message_t message;
            message.channel = label;
            message.binary = true;
            message.payload.reserve(data.size());
            for (const auto value : data) {
              message.payload.push_back(std::to_integer<std::uint8_t>(value));
            }
            std::lock_guard lock {incoming_state_copy->mutex};
            if (incoming_state_copy->closed) {
              return;
            }
            constexpr std::size_t maximum_queued_messages = 64;
            if (incoming_state_copy->messages.size() >= maximum_queued_messages) {
              tracker_copy.fail("data_channel_receive");
              return;
            }
            incoming_state_copy->messages.push_back(std::move(message));
          },
          [tracker_copy, incoming_state_copy, label](std::string data) {
            channel_message_t message;
            message.channel = label;
            message.payload.assign(data.begin(), data.end());
            std::lock_guard lock {incoming_state_copy->mutex};
            if (incoming_state_copy->closed) {
              return;
            }
            constexpr std::size_t maximum_queued_messages = 64;
            if (incoming_state_copy->messages.size() >= maximum_queued_messages) {
              tracker_copy.fail("data_channel_receive");
              return;
            }
            incoming_state_copy->messages.push_back(std::move(message));
          }
        );
        channels.push_back(std::move(channel));
      }
    }

    void close() {
      if (closed) {
        return;
      }
      closed = true;
      {
        std::lock_guard lock {incoming_state->mutex};
        incoming_state->closed = true;
        incoming_state->messages.clear();
      }
      for (auto &channel : channels) {
        channel->resetCallbacks();
        channel->close();
      }
      for (auto &track : tracks) {
        track->resetCallbacks();
        track->close();
      }
      if (peer) {
        peer->resetCallbacks();
        peer->close();
      }
      channels.clear();
      tracks.clear();
      peer.reset();
      tracker.observe_peer(peer_state_e::closed);
      tracker.observe_ice(ice_state_e::closed);
    }
  };

  peer_t::peer_t(options_t options):
      impl_(std::make_unique<impl_t>(options)) {
  }

  peer_t::~peer_t() {
    close();
  }

  result_t<offer_t> peer_t::create_offer(
    std::chrono::milliseconds timeout,
    const std::function<bool()> &cancelled
  ) {
    result_t<offer_t> result;
    try {
      impl_->peer->setLocalDescription(rtc::Description::Type::Offer);
    } catch (const std::exception &) {
      result.error = {error_e::invalid_sdp, "create_offer", "libdatachannel could not create the local offer"};
      return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock {impl_->offer_state->mutex};
    while (!impl_->offer_state->gathering_complete) {
      if (cancelled && cancelled()) {
        result.error = {error_e::cancelled, "create_offer", "SDP and ICE gathering was cancelled"};
        return result;
      }
      if (impl_->offer_state->changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds {100})) == std::cv_status::timeout && std::chrono::steady_clock::now() >= deadline) {
        if (impl_->use_public_stun) {
          lock.unlock();
          impl_->close();
          impl_ = std::make_unique<impl_t>(options_t {.use_public_stun = false});
          auto fallback = create_offer(timeout, cancelled);
          if (fallback) {
            fallback.value.used_host_candidate_fallback = true;
          }
          return fallback;
        }
        result.error = {error_e::timeout, "create_offer", "SDP and ICE gathering deadline expired"};
        return result;
      }
    }
    result.value.candidates = impl_->offer_state->candidates;
    lock.unlock();
    const auto description = impl_->peer->localDescription();
    if (!description) {
      result.error = {error_e::invalid_sdp, "create_offer", "local SDP was unavailable after gathering"};
      return result;
    }
    result.value.sdp = description->generateSdp();
    if (const auto validation = validate_offer(result.value.sdp); !validation.empty()) {
      result.error = {error_e::invalid_sdp, "create_offer", validation};
    }
    return result;
  }

  result_t<bool> peer_t::set_remote_answer(std::string_view sdp) {
    result_t<bool> result;
    if (sdp.empty()) {
      result.error = {error_e::invalid_sdp, "remote_answer", "remote SDP answer is empty"};
      return result;
    }
    try {
      impl_->peer->setRemoteDescription(rtc::Description {std::string {sdp}, rtc::Description::Type::Answer});
    } catch (const std::exception &) {
      result.error = {error_e::invalid_sdp, "remote_answer", "libdatachannel rejected the remote SDP answer"};
      return result;
    }
    result.value = true;
    return result;
  }

  result_t<bool> peer_t::add_remote_candidates(const std::vector<protocol::ice_candidate_t> &candidates) {
    result_t<bool> result;
    try {
      for (const auto &candidate : candidates) {
        if (candidate.candidate == "a=end-of-candidates" || candidate.candidate == "end-of-candidates") {
          continue;
        }
        std::string normalized = candidate.candidate;
        if (normalized.starts_with("a=")) {
          normalized.erase(0, 2);
        }
        const auto mid = candidate.sdp_mid.empty() ? "video" : candidate.sdp_mid;
        if (!impl_->tracker.accept_remote_candidate(normalized, mid)) {
          continue;
        }
        impl_->peer->addRemoteCandidate(rtc::Candidate {std::move(normalized), mid});
      }
    } catch (const std::exception &) {
      result.error = {error_e::invalid_candidate, "remote_ice", "libdatachannel rejected a remote ICE candidate"};
      return result;
    }
    result.value = true;
    return result;
  }

  result_t<bool> peer_t::wait_ready(
    std::chrono::milliseconds timeout,
    const std::function<bool()> &cancelled
  ) const {
    return impl_->tracker.wait_ready(timeout, cancelled);
  }

  bool peer_t::send_text(std::string_view channel, std::string_view payload) {
    const auto index = channel_index(channel);
    if (!impl_ || impl_->closed || !index || *index >= impl_->channels.size()) {
      return false;
    }
    const auto &target = impl_->channels[*index];
    if (!target || !target->isOpen()) {
      return false;
    }
    try {
      // libdatachannel returns false when it buffers rather than immediately
      // transmits a message; either outcome means the payload was accepted.
      static_cast<void>(target->send(std::string {payload}));
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  bool peer_t::send_binary(std::string_view channel, const std::vector<std::uint8_t> &payload) {
    const auto index = channel_index(channel);
    if (!impl_ || impl_->closed || !index || *index >= impl_->channels.size()) {
      return false;
    }
    const auto &target = impl_->channels[*index];
    if (!target || !target->isOpen()) {
      return false;
    }
    rtc::binary converted;
    converted.reserve(payload.size());
    for (const auto value : payload) {
      converted.push_back(static_cast<std::byte>(value));
    }
    try {
      static_cast<void>(target->send(std::move(converted)));
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  std::optional<channel_message_t> peer_t::take_message() {
    if (!impl_) {
      return std::nullopt;
    }
    std::lock_guard lock {impl_->incoming_state->mutex};
    if (impl_->incoming_state->messages.empty()) {
      return std::nullopt;
    }
    auto message = std::move(impl_->incoming_state->messages.front());
    impl_->incoming_state->messages.pop_front();
    return message;
  }

  snapshot_t peer_t::snapshot() const {
    return impl_->tracker.snapshot();
  }

  void peer_t::close() {
    if (impl_) {
      impl_->close();
    }
  }

  void cleanup_runtime() {
    rtc::Cleanup().wait();
  }
}  // namespace xbox_remote::transport
