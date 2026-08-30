/**
 * @file src/xbox_remote/transport.h
 * @brief Isolated WebRTC transport used by the Xbox compatibility probe.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/xbox_remote/protocol.h"

namespace xbox_remote::transport {
  /**
   * @brief Sanitized transport error categories.
   */
  enum class error_e {
    none,  ///< Operation succeeded.
    cancelled,  ///< Caller cancelled the operation.
    timeout,  ///< An asynchronous transport deadline expired.
    invalid_sdp,  ///< Local or remote SDP violated the probe contract.
    invalid_candidate,  ///< A remote ICE candidate could not be accepted.
    peer_failed,  ///< ICE, DTLS, SCTP, or a required channel failed.
  };

  /**
   * @brief Caller-visible transport failure without SDP or candidate contents.
   */
  struct failure_t {
    error_e code = error_e::none;  ///< Machine-readable category.
    std::string stage;  ///< Fixed transport stage.
    std::string message;  ///< Non-sensitive diagnostic.
  };

  /**
   * @brief Result returned by transport operations.
   *
   * @tparam T Successful value type.
   */
  template<typename T>
  struct result_t {
    T value {};  ///< Successful value.
    failure_t error {};  ///< Sanitized failure.

    /**
     * @brief Check whether the operation succeeded.
     *
     * @return @c true when no error is present.
     */
    explicit operator bool() const {
      return error.code == error_e::none;
    }
  };

  /**
   * @brief Coarse peer-connection state for validation and tests.
   */
  enum class peer_state_e {
    created,  ///< Peer exists but is not connected.
    connecting,  ///< ICE/DTLS connection is in progress.
    connected,  ///< Peer connection is established.
    disconnected,  ///< Peer lost connectivity.
    failed,  ///< Peer entered a terminal failure.
    closed,  ///< Peer was explicitly closed.
  };

  /**
   * @brief Coarse ICE state for validation and tests.
   */
  enum class ice_state_e {
    created,  ///< ICE has not started.
    checking,  ///< ICE checks are in progress.
    connected,  ///< ICE has a selected pair.
    completed,  ///< ICE gathering/checks completed.
    disconnected,  ///< ICE connectivity was lost.
    failed,  ///< ICE failed.
    closed,  ///< ICE was closed.
  };

  /**
   * @brief Thread-safe probe state snapshot.
   */
  struct snapshot_t {
    peer_state_e peer = peer_state_e::created;  ///< Latest peer state.
    ice_state_e ice = ice_state_e::created;  ///< Latest ICE state.
    std::array<bool, protocol::data_channels.size()> channels_open {};  ///< Current channel open flags in protocol order.
    std::array<bool, protocol::data_channels.size()> channels_ever_opened {};  ///< Whether each negotiated channel opened at least once.
    std::uint64_t video_packets = 0;  ///< Received video RTP packet count.
    std::uint64_t video_bytes = 0;  ///< Received video RTP byte count.
    std::uint64_t audio_packets = 0;  ///< Received audio RTP packet count.
    std::uint64_t audio_bytes = 0;  ///< Received audio RTP byte count.
    std::size_t remote_candidate_count = 0;  ///< Unique accepted remote candidates.
    std::string failure_stage;  ///< Fixed failure stage, empty when healthy.

    /**
     * @brief Check whether all required data channels are open.
     *
     * @return @c true when every channel is open.
     */
    bool all_channels_open() const;

    /**
     * @brief Check whether every negotiated data channel opened at least once.
     *
     * @return @c true after all four Xbox channels have opened.
     */
    bool all_channels_ever_opened() const;

    /**
     * @brief Check whether the channels required for control and input remain open.
     *
     * @return @c true when control, input, and message are currently open.
     */
    bool critical_channels_open() const;

    /**
     * @brief Check whether peer, ICE, and negotiated channels satisfy the hard gate.
     *
     * @return @c true when the transport is ready.
     */
    bool ready() const;
  };

  /**
   * @brief Thread-safe event tracker shared by the real and fake transports.
   */
  class state_tracker_t {
  public:
    /**
     * @brief Construct an empty tracker.
     */
    state_tracker_t();

    /**
     * @brief Record a peer state transition.
     *
     * @param state New peer state.
     */
    void observe_peer(peer_state_e state) const;

    /**
     * @brief Record an ICE state transition.
     *
     * @param state New ICE state.
     */
    void observe_ice(ice_state_e state) const;

    /**
     * @brief Record a required data-channel state transition.
     *
     * @param label Required channel label.
     * @param open Whether the channel is open.
     */
    void observe_channel(std::string_view label, bool open) const;

    /**
     * @brief Record one consumed RTP packet.
     *
     * @param video @c true for video, @c false for audio.
     * @param bytes Packet size.
     */
    void observe_media(bool video, std::size_t bytes) const;

    /**
     * @brief Deduplicate a remote candidate without exposing it in snapshots.
     *
     * @param candidate Raw candidate attribute.
     * @param mid SDP media identifier.
     * @return @c true only for the first observation.
     */
    bool accept_remote_candidate(std::string_view candidate, std::string_view mid) const;

    /**
     * @brief Record a sanitized terminal failure.
     *
     * @param stage Fixed failure stage.
     */
    void fail(std::string stage) const;

    /**
     * @brief Return a consistent state snapshot.
     *
     * @return Current state.
     */
    snapshot_t snapshot() const;

    /**
     * @brief Wait until the peer is ready, failed, cancelled, or timed out.
     *
     * @param timeout Maximum wait duration.
     * @param cancelled Cancellation callback.
     * @return Success, cancellation, timeout, or peer failure.
     */
    result_t<bool> wait_ready(
      std::chrono::milliseconds timeout,
      const std::function<bool()> &cancelled
    ) const;

  private:
    struct impl_t;
    std::shared_ptr<impl_t> impl_;  ///< Shared callback-safe state.
  };

  /**
   * @brief Generated local offer and separately exchanged ICE candidates.
   */
  struct offer_t {
    std::string sdp;  ///< Full video/audio/application SDP offer.
    std::vector<protocol::ice_candidate_t> candidates;  ///< Gathered local candidates.
    bool used_host_candidate_fallback = false;  ///< Whether unreachable public STUN servers were retried with host candidates only.
  };

  /**
   * @brief One inbound Xbox data-channel message retained in memory.
   */
  struct channel_message_t {
    std::string channel;  ///< Fixed negotiated channel label.
    bool binary = false;  ///< Whether the payload used the binary SCTP message type.
    std::vector<std::uint8_t> payload;  ///< Message bytes without logging or persistence.
  };

  /**
   * @brief Construction controls for offline and real transport checks.
   */
  struct options_t {
    bool use_public_stun = true;  ///< Whether to use the fixed Xbox-compatible public STUN list.
  };

  /**
   * @brief libdatachannel-backed Xbox WebRTC compatibility peer.
   */
  class peer_t {
  public:
    /**
     * @brief Construct video/audio receive tracks and four required channels.
     *
     * @param options Probe-only transport controls.
     */
    explicit peer_t(options_t options = {});

    /**
     * @brief Close and destroy the peer deterministically.
     */
    ~peer_t();

    peer_t(const peer_t &) = delete;
    peer_t &operator=(const peer_t &) = delete;

    /**
     * @brief Generate the complete local offer and gather local ICE.
     *
     * @param timeout Gathering deadline.
     * @param cancelled Cancellation callback.
     * @return SDP plus local candidates, or a sanitized failure.
     */
    result_t<offer_t> create_offer(
      std::chrono::milliseconds timeout,
      const std::function<bool()> &cancelled
    );

    /**
     * @brief Apply the Xbox SDP answer.
     *
     * @param sdp Complete remote answer.
     * @return Success or a sanitized SDP failure.
     */
    result_t<bool> set_remote_answer(std::string_view sdp);

    /**
     * @brief Add validated Xbox ICE candidates, ignoring duplicates and end markers.
     *
     * @param candidates Remote candidates.
     * @return Success or a sanitized candidate failure.
     */
    result_t<bool> add_remote_candidates(const std::vector<protocol::ice_candidate_t> &candidates);

    /**
     * @brief Wait for peer, ICE, and all four data channels.
     *
     * @param timeout Connection deadline.
     * @param cancelled Cancellation callback.
     * @return Success, cancellation, timeout, or peer failure.
     */
    result_t<bool> wait_ready(
      std::chrono::milliseconds timeout,
      const std::function<bool()> &cancelled
    ) const;

    /**
     * @brief Send one UTF-8 message on an open Xbox data channel.
     *
     * @param channel Negotiated channel label.
     * @param payload UTF-8 protocol payload.
     * @return @c true when libdatachannel accepted or buffered the message.
     */
    bool send_text(std::string_view channel, std::string_view payload);

    /**
     * @brief Send one binary message on an open Xbox data channel.
     *
     * @param channel Negotiated channel label.
     * @param payload Binary protocol payload.
     * @return @c true when libdatachannel accepted or buffered the message.
     */
    bool send_binary(std::string_view channel, const std::vector<std::uint8_t> &payload);

    /**
     * @brief Remove the oldest queued inbound data-channel message.
     *
     * @return Message with its channel label, or no value when the queue is empty.
     */
    std::optional<channel_message_t> take_message();

    /**
     * @brief Return current transport and media counters.
     *
     * @return Current state snapshot.
     */
    snapshot_t snapshot() const;

    /**
     * @brief Close the peer and reset all callbacks.
     */
    void close();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;  ///< libdatachannel implementation owner.
  };

  /**
   * @brief Stop libdatachannel global worker threads after all peers are destroyed.
   */
  void cleanup_runtime();
}  // namespace xbox_remote::transport
