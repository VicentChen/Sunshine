/**
 * @file src/xbox_remote/input_queue.h
 * @brief Bounded Xbox Remote Play input scheduling and packetization.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// local includes
#include "src/xbox_remote/protocol.h"

namespace xbox_remote::input {
  /**
   * @brief Kind of non-blocking work emitted by the input queue.
   */
  enum class item_kind_e {
    attach,  ///< Announce that the logical gamepad is present.
    state,  ///< Send one complete absolute gamepad snapshot.
    neutralize,  ///< Send a high-priority neutral snapshot.
    detach,  ///< Announce that the logical gamepad is absent.
  };

  /**
   * @brief One item returned to the WebRTC sender thread.
   */
  struct item_t {
    item_kind_e kind = item_kind_e::state;  ///< Required send operation.
    protocol::gamepad_frame_t frame {};  ///< Complete state for state-bearing items.
  };

  /**
   * @brief Bounded queue controls.
   */
  struct queue_options_t {
    std::chrono::milliseconds edge_ttl {50};  ///< Maximum age of a digital edge.
    std::size_t edge_capacity = 64;  ///< Maximum retained digital snapshots.
    std::size_t control_capacity = 8;  ///< Maximum retained attach/neutralize/detach items.
  };

  /**
   * @brief Single-owner bounded latest-state queue with a short digital journal.
   *
   * Analog-only submissions overwrite the latest-state slot. Digital button
   * transitions enter a bounded FIFO journal and therefore cannot be overwritten
   * by ordinary state submissions before expiry. Control operations are kept in
   * a separate bounded priority queue. No method blocks or performs I/O.
   */
  class outbound_queue_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Monotonic journal clock.
    using time_point_t = clock_t::time_point;  ///< Monotonic timestamp type.

    /**
     * @brief Construct an empty queue.
     *
     * @param options Queue capacities and digital-edge lifetime.
     */
    explicit outbound_queue_t(queue_options_t options = {});

    /**
     * @brief Queue logical gamepad attachment and the current absolute state.
     *
     * @param frame Current complete gamepad state.
     * @param now Current monotonic time.
     * @return @c false only when the control queue is full.
     */
    bool attach(const protocol::gamepad_frame_t &frame, time_point_t now);

    /**
     * @brief Submit a complete gamepad state without blocking.
     *
     * @param frame New complete gamepad state.
     * @param now Current monotonic time.
     */
    void submit(const protocol::gamepad_frame_t &frame, time_point_t now);

    /**
     * @brief Clear queued input and enqueue a high-priority neutral snapshot.
     *
     * @param now Current monotonic time.
     * @return @c false only when the control queue is full.
     */
    bool neutralize(time_point_t now);

    /**
     * @brief Clear queued input and enqueue logical gamepad removal.
     *
     * @param now Current monotonic time.
     * @return @c false only when the control queue is full.
     */
    bool detach(time_point_t now);

    /**
     * @brief Discard stale transitions after transport reconnect.
     *
     * The next call to @ref take returns only the current absolute state.
     */
    void on_reconnect();

    /**
     * @brief Take the next control, digital-edge, or latest-state item.
     *
     * @param now Current monotonic time used to expire journal entries.
     * @return Next item, or no value when nothing is pending.
     */
    std::optional<item_t> take(time_point_t now);

    /**
     * @brief Return how many digital edges were dropped at capacity.
     *
     * @return Cumulative dropped-edge count.
     */
    std::uint64_t dropped_edges() const;

    /**
     * @brief Return the current retained edge count.
     *
     * @return Bounded journal size.
     */
    std::size_t edge_count() const;

  private:
    struct versioned_item_t {
      item_t item;  ///< Scheduled operation.
      std::uint64_t version = 0;  ///< Submission order marker.
      time_point_t timestamp {};  ///< Journal insertion time.
    };

    void purge_expired(time_point_t now);
    bool push_control(item_kind_e kind, const protocol::gamepad_frame_t &frame, time_point_t now);

    queue_options_t options_;  ///< Bounded queue controls.
    std::deque<versioned_item_t> controls_;  ///< Priority control operations.
    std::deque<versioned_item_t> edges_;  ///< Short-lived digital transition journal.
    std::optional<versioned_item_t> latest_;  ///< Latest complete absolute state.
    std::uint64_t next_version_ = 1;  ///< Next submission marker.
    std::uint64_t emitted_version_ = 0;  ///< Newest emitted state version.
    std::uint64_t dropped_edges_ = 0;  ///< Capacity-overflow counter.
  };

  /**
   * @brief Sender-thread-owned Xbox input packetizer.
   */
  class packetizer_t {
  public:
    /**
     * @brief Construct a packetizer with a deterministic initial sequence.
     *
     * @param initial_sequence First sequence value placed on the wire.
     */
    explicit packetizer_t(std::uint32_t initial_sequence = 0);

    /**
     * @brief Encode one absolute gamepad state and advance sequence once.
     *
     * @param frame Complete gamepad state.
     * @param timestamp_ms Monotonic send timestamp in milliseconds.
     * @return Xbox-compatible gamepad packet.
     */
    std::vector<std::uint8_t> encode(const protocol::gamepad_frame_t &frame, double timestamp_ms);

    /**
     * @brief Reset sequence ownership for a new Remote Play session.
     *
     * @param initial_sequence First new-session sequence value.
     */
    void reset(std::uint32_t initial_sequence = 0);

    /**
     * @brief Return the next sequence that will be assigned.
     *
     * @return Next wire sequence.
     */
    std::uint32_t next_sequence() const;

  private:
    std::uint32_t sequence_ = 0;  ///< Sender-thread-owned sequence counter.
  };

  /**
   * @brief Expand a Sunshine-style unsigned trigger to the Xbox 16-bit range.
   *
   * @param value Trigger value in the inclusive range 0 through 255.
   * @return Exact full-range mapping using @c value * 257.
   */
  constexpr std::uint16_t expand_trigger(std::uint8_t value) {
    return static_cast<std::uint16_t>(value) * 257U;
  }

  /**
   * @brief Derive the Xbox activity mask for a complete gamepad frame.
   *
   * @param frame Complete Xbox gamepad state.
   * @return Bitwise combination of active physical controls.
   */
  std::uint32_t activity_mask(const protocol::gamepad_frame_t &frame);
}  // namespace xbox_remote::input
