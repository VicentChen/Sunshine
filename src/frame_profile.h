/**
 * @file src/frame_profile.h
 * @brief Per-frame timestamps for HDMI RX and RKMPP performance profiling.
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>

namespace video {
  /** @brief Classifies frames that may appear in RKMPP profile statistics. */
  enum class frame_profile_kind_e {
    captured,  ///< A real frame dequeued from the HDMI RX driver.
    placeholder,  ///< A synthetic no-signal or recovery placeholder.
    repeated,  ///< A previously encoded frame repeated by frame pacing.
  };

  /**
   * @brief Fixed-size timestamps carried with one frame through the video pipeline.
   *
   * Missing stages remain disengaged instead of being reported as zero duration.
   * The structure deliberately owns no buffers and performs no dynamic allocation.
   */
  struct frame_profile_t {
    using time_point = std::chrono::steady_clock::time_point;  ///< Monotonic timestamp type used by all profile stages.

    frame_profile_kind_e kind {frame_profile_kind_e::captured};  ///< Origin of this frame.
    std::uint32_t capture_sequence {};  ///< V4L2 sequence assigned by the HDMI RX driver.
    std::uint32_t capture_timestamp_flags {};  ///< V4L2 timestamp type and source flags.
    std::uint32_t hdmirx_width {};  ///< Visible HDMI RX frame width in pixels.
    std::uint32_t hdmirx_height {};  ///< Visible HDMI RX frame height in pixels.
    std::uint32_t moonlight_width {};  ///< Moonlight-requested encoded frame width in pixels.
    std::uint32_t moonlight_height {};  ///< Moonlight-requested encoded frame height in pixels.
    std::uint32_t freshness_drops {};  ///< Older complete V4L2 frames returned to the driver before this frame was selected.
    std::int64_t frame_index {-1};  ///< Sunshine frame index assigned before packetization.
    bool rga_used {};  ///< Whether the frame passed through RGA conversion or fill.

    std::optional<time_point> capture;  ///< V4L2 CLOCK_MONOTONIC timestamp.
    std::optional<time_point> dequeued;  ///< Time immediately after VIDIOC_DQBUF succeeded.
    std::optional<time_point> capture_queue_exit;  ///< Time the encoder thread began processing the captured image.
    std::optional<time_point> rga_begin;  ///< Time immediately before RGA work began.
    std::optional<time_point> rga_end;  ///< Time immediately after RGA work completed.
    std::optional<time_point> mpp_import_begin;  ///< Time immediately before a non-cached MPP DMA-BUF import.
    std::optional<time_point> mpp_import_end;  ///< Time immediately after a non-cached MPP DMA-BUF import.
    std::optional<time_point> mpp_output_buffer_begin;  ///< Time immediately before MPP allocates an output buffer.
    std::optional<time_point> mpp_output_buffer_end;  ///< Time immediately after MPP allocates an output buffer.
    std::optional<time_point> mpp_output_packet_begin;  ///< Time immediately before MPP initializes the output packet wrapper.
    std::optional<time_point> mpp_output_packet_end;  ///< Time immediately after MPP initializes the output packet wrapper.
    std::optional<time_point> mpp_submit_begin;  ///< Time immediately before encode_put_frame.
    std::optional<time_point> mpp_submit_end;  ///< Time immediately after encode_put_frame returned.
    std::optional<time_point> mpp_output;  ///< Time a complete non-empty MppPacket became available.
    std::optional<time_point> packetize_begin;  ///< Time the network thread began processing the encoded frame.
    std::optional<time_point> send_end;  ///< Time the final send_batch call for the frame returned.
  };

  /** @brief Durations calculated from a completed frame profile. */
  enum class frame_profile_metric_e : std::uint8_t {
    rx_driver_age,  ///< V4L2 timestamp to successful dequeue.
    capture_queue,  ///< Successful dequeue to encoder-thread processing.
    rga,  ///< RGA conversion or placeholder fill.
    mpp_import,  ///< Non-cached MPP DMA-BUF import.
    mpp_output_buffer_acquire,  ///< MPP output-buffer allocation.
    mpp_output_packet_init,  ///< MPP output-packet wrapper initialization.
    mpp_submit,  ///< Time spent in encode_put_frame.
    mpp_output_wait,  ///< encode_put_frame return to complete MppPacket.
    mpp_encode,  ///< encode_put_frame entry to complete MppPacket.
    encoded_queue,  ///< Complete MppPacket to network-thread processing.
    packetize_send,  ///< Network-thread processing to final send return.
    protocol_host,  ///< V4L2 timestamp to network-thread processing.
    host_send,  ///< V4L2 timestamp to final send return.
    count,  ///< Number of metric identifiers; not a measured duration.
  };

  /** @brief One metric's statistics for a completed bounded window. */
  struct frame_profile_metric_snapshot_t {
    std::uint32_t count {};  ///< Number of valid samples.
    std::uint32_t missing {};  ///< Number of frames missing required timestamps.
    std::uint32_t invalid {};  ///< Number of frames whose end preceded their start.
    std::int64_t minimum_us {};  ///< Minimum duration in microseconds.
    std::int64_t p50_us {};  ///< Nearest-rank 50th percentile in microseconds.
    std::int64_t p95_us {};  ///< Nearest-rank 95th percentile in microseconds.
    std::int64_t p99_us {};  ///< Nearest-rank 99th percentile in microseconds.
    std::int64_t maximum_us {};  ///< Maximum duration in microseconds.
  };

  /** @brief Immutable statistics emitted when one profile window closes. */
  struct frame_profile_snapshot_t {
    static constexpr auto metric_count = static_cast<std::size_t>(frame_profile_metric_e::count);  ///< Number of metric snapshots.
    std::array<frame_profile_metric_snapshot_t, metric_count> metrics;  ///< Per-stage duration summaries.
    std::uint32_t captured_frames {};  ///< Real HDMI RX frames observed.
    std::uint32_t hdmirx_width {};  ///< Latest sampled HDMI RX frame width in pixels.
    std::uint32_t hdmirx_height {};  ///< Latest sampled HDMI RX frame height in pixels.
    std::uint32_t moonlight_width {};  ///< Latest sampled Moonlight-requested encoded width in pixels.
    std::uint32_t moonlight_height {};  ///< Latest sampled Moonlight-requested encoded height in pixels.
    std::uint32_t placeholder_frames {};  ///< Synthetic placeholder frames observed.
    std::uint32_t repeated_frames {};  ///< Repeated frames observed.
    std::uint32_t rga_bypass_frames {};  ///< Real frames that bypassed RGA.
    std::uint64_t freshness_drops {};  ///< Total older V4L2 frames proactively discarded to keep the newest frame.
    std::uint32_t dropped_samples {};  ///< Samples discarded after the bounded buffer filled.
  };

  /**
   * @brief Return a stable display name for a profile metric.
   *
   * @param metric Metric to describe.
   * @return Static metric name.
   */
  constexpr std::string_view frame_profile_metric_name(frame_profile_metric_e metric) noexcept {
    switch (metric) {
      case frame_profile_metric_e::rx_driver_age:
        return "RX driver age";
      case frame_profile_metric_e::capture_queue:
        return "Capture queue";
      case frame_profile_metric_e::rga:
        return "RGA";
      case frame_profile_metric_e::mpp_import:
        return "MPP input import";
      case frame_profile_metric_e::mpp_output_buffer_acquire:
        return "MPP output buffer acquire";
      case frame_profile_metric_e::mpp_output_packet_init:
        return "MPP output packet init";
      case frame_profile_metric_e::mpp_submit:
        return "MPP submit";
      case frame_profile_metric_e::mpp_output_wait:
        return "MPP output wait";
      case frame_profile_metric_e::mpp_encode:
        return "MPP encode";
      case frame_profile_metric_e::encoded_queue:
        return "Encoded queue";
      case frame_profile_metric_e::packetize_send:
        return "Packetize/send";
      case frame_profile_metric_e::protocol_host:
        return "Protocol host";
      case frame_profile_metric_e::host_send:
        return "Host to send";
      default:
        return "unknown";
    }
  }

  /**
   * @brief Quantize a host-processing duration for Moonlight's short header.
   *
   * @param duration Non-negative duration to encode.
   * @return Saturated latency in 0.1 ms units.
   */
  inline std::uint16_t duration_to_protocol_latency(std::chrono::steady_clock::duration duration) noexcept {
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    if (microseconds <= 0) {
      return 0;
    }
    const auto rounded = (microseconds + 50) / 100;
    return static_cast<std::uint16_t>(std::min<std::int64_t>(rounded, std::numeric_limits<std::uint16_t>::max()));
  }

  /**
   * @brief Allocation-free bounded collector for completed frame profiles.
   *
   * A single network thread owns the collector. Snapshot creation sorts only
   * the fixed local buffers and resets them for the next window.
   */
  class frame_profile_window_t {
  public:
    static constexpr std::size_t sample_capacity = 512;  ///< Maximum samples retained for each metric.

    /**
     * @brief Collect all applicable durations from one completed frame.
     *
     * @param profile Completed per-frame timestamps.
     */
    void collect(const frame_profile_t &profile) noexcept {
      if (profile.kind == frame_profile_kind_e::placeholder) {
        ++placeholder_frames_;
        return;
      }
      if (profile.kind == frame_profile_kind_e::repeated) {
        ++repeated_frames_;
        return;
      }

      ++captured_frames_;
      freshness_drops_ += profile.freshness_drops;
      if (profile.hdmirx_width != 0 && profile.hdmirx_height != 0) {
        hdmirx_width_ = profile.hdmirx_width;
        hdmirx_height_ = profile.hdmirx_height;
      }
      if (profile.moonlight_width != 0 && profile.moonlight_height != 0) {
        moonlight_width_ = profile.moonlight_width;
        moonlight_height_ = profile.moonlight_height;
      }
      collect_metric(frame_profile_metric_e::rx_driver_age, profile.capture, profile.dequeued);
      collect_metric(frame_profile_metric_e::capture_queue, profile.dequeued, profile.capture_queue_exit);
      if (profile.rga_used) {
        collect_metric(frame_profile_metric_e::rga, profile.rga_begin, profile.rga_end);
      } else {
        ++rga_bypass_frames_;
      }
      collect_metric(frame_profile_metric_e::mpp_import, profile.mpp_import_begin, profile.mpp_import_end);
      collect_metric(frame_profile_metric_e::mpp_output_buffer_acquire, profile.mpp_output_buffer_begin, profile.mpp_output_buffer_end);
      collect_metric(frame_profile_metric_e::mpp_output_packet_init, profile.mpp_output_packet_begin, profile.mpp_output_packet_end);
      collect_metric(frame_profile_metric_e::mpp_submit, profile.mpp_submit_begin, profile.mpp_submit_end);
      collect_metric(frame_profile_metric_e::mpp_output_wait, profile.mpp_submit_end, profile.mpp_output);
      collect_metric(frame_profile_metric_e::mpp_encode, profile.mpp_submit_begin, profile.mpp_output);
      collect_metric(frame_profile_metric_e::encoded_queue, profile.mpp_output, profile.packetize_begin);
      collect_metric(frame_profile_metric_e::packetize_send, profile.packetize_begin, profile.send_end);
      collect_metric(frame_profile_metric_e::protocol_host, profile.capture, profile.packetize_begin);
      collect_metric(frame_profile_metric_e::host_send, profile.capture, profile.send_end);
    }

    /**
     * @brief Finish the current window and clear the collector.
     *
     * @return Immutable summary of every collected metric and frame class.
     */
    frame_profile_snapshot_t snapshot_and_reset() noexcept {
      frame_profile_snapshot_t snapshot;
      snapshot.captured_frames = captured_frames_;
      snapshot.hdmirx_width = hdmirx_width_;
      snapshot.hdmirx_height = hdmirx_height_;
      snapshot.moonlight_width = moonlight_width_;
      snapshot.moonlight_height = moonlight_height_;
      snapshot.placeholder_frames = placeholder_frames_;
      snapshot.repeated_frames = repeated_frames_;
      snapshot.rga_bypass_frames = rga_bypass_frames_;
      snapshot.freshness_drops = freshness_drops_;
      snapshot.dropped_samples = dropped_samples_;

      for (std::size_t index = 0; index < metric_count; ++index) {
        auto &window = metrics_[index];
        auto &metric = snapshot.metrics[index];
        metric.count = static_cast<std::uint32_t>(window.used);
        metric.missing = window.missing;
        metric.invalid = window.invalid;
        if (window.used != 0) {
          std::sort(window.samples.begin(), window.samples.begin() + window.used);
          metric.minimum_us = window.samples.front();
          metric.p50_us = percentile(window, 50);
          metric.p95_us = percentile(window, 95);
          metric.p99_us = percentile(window, 99);
          metric.maximum_us = window.samples[window.used - 1];
        }
      }

      reset();
      return snapshot;
    }

  private:
    static constexpr auto metric_count = frame_profile_snapshot_t::metric_count;

    /** @brief Fixed samples and error counters for one metric. */
    struct metric_window_t {
      std::array<std::int64_t, sample_capacity> samples {};  ///< Valid microsecond samples.
      std::size_t used {};  ///< Number of populated samples.
      std::uint32_t missing {};  ///< Missing timestamp pairs.
      std::uint32_t invalid {};  ///< Negative timestamp pairs.
    };

    /**
     * @brief Collect one timestamp pair into a metric window.
     *
     * @param metric Destination metric.
     * @param start Optional stage start.
     * @param end Optional stage end.
     */
    void collect_metric(frame_profile_metric_e metric, const std::optional<frame_profile_t::time_point> &start, const std::optional<frame_profile_t::time_point> &end) noexcept {
      auto &window = metrics_[static_cast<std::size_t>(metric)];
      if (!start || !end) {
        ++window.missing;
        return;
      }
      if (*end < *start) {
        ++window.invalid;
        return;
      }
      if (window.used == sample_capacity) {
        ++dropped_samples_;
        return;
      }
      window.samples[window.used++] = std::chrono::duration_cast<std::chrono::microseconds>(*end - *start).count();
    }

    /**
     * @brief Calculate a nearest-rank percentile from sorted samples.
     *
     * @param window Sorted non-empty metric window.
     * @param percentile_value Percentile in the inclusive range 1..100.
     * @return Selected sample in microseconds.
     */
    static std::int64_t percentile(const metric_window_t &window, std::size_t percentile_value) noexcept {
      const auto rank = (window.used * percentile_value + 99) / 100;
      return window.samples[rank - 1];
    }

    /** @brief Clear all samples and counters for the next window. */
    void reset() noexcept {
      metrics_ = {};
      captured_frames_ = 0;
      hdmirx_width_ = 0;
      hdmirx_height_ = 0;
      moonlight_width_ = 0;
      moonlight_height_ = 0;
      placeholder_frames_ = 0;
      repeated_frames_ = 0;
      rga_bypass_frames_ = 0;
      freshness_drops_ = 0;
      dropped_samples_ = 0;
    }

    std::array<metric_window_t, metric_count> metrics_ {};  ///< Per-stage bounded windows.
    std::uint32_t captured_frames_ {};  ///< Real frames collected in this window.
    std::uint32_t hdmirx_width_ {};  ///< Latest HDMI RX width collected in this window.
    std::uint32_t hdmirx_height_ {};  ///< Latest HDMI RX height collected in this window.
    std::uint32_t moonlight_width_ {};  ///< Latest Moonlight requested width collected in this window.
    std::uint32_t moonlight_height_ {};  ///< Latest Moonlight requested height collected in this window.
    std::uint32_t placeholder_frames_ {};  ///< Placeholder frames collected in this window.
    std::uint32_t repeated_frames_ {};  ///< Repeated frames collected in this window.
    std::uint32_t rga_bypass_frames_ {};  ///< Real frames that skipped RGA.
    std::uint64_t freshness_drops_ {};  ///< Older V4L2 frames proactively discarded in this window.
    std::uint32_t dropped_samples_ {};  ///< Samples discarded due to capacity.
  };

  /** @brief Thread-safe publication point for the latest completed profile window. */
  class frame_profile_snapshot_store_t {
  public:
    /**
     * @brief Publish a completed statistics window for log and overlay consumers.
     *
     * @param snapshot Snapshot to copy into fixed storage.
     * @return Monotonically increasing publication generation.
     */
    std::uint64_t publish(const frame_profile_snapshot_t &snapshot) noexcept {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot_ = snapshot;
      return ++generation_;
    }

    /**
     * @brief Copy the latest snapshot only when it is newer than the caller's generation.
     *
     * @param generation Caller-owned generation, updated on success.
     * @param snapshot Destination for the fixed-size snapshot copy.
     * @return True when a newer snapshot was copied.
     */
    bool read_newer(std::uint64_t &generation, frame_profile_snapshot_t &snapshot) noexcept {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation == generation_) {
        return false;
      }
      generation = generation_;
      snapshot = snapshot_;
      return true;
    }

  private:
    std::mutex mutex_;  ///< Protects the fixed snapshot and generation.
    frame_profile_snapshot_t snapshot_ {};  ///< Most recently completed window.
    std::uint64_t generation_ {};  ///< Zero before the first publication.
  };

  /** @brief Return the process-wide fixed snapshot store shared by stream and encoder threads. */
  inline frame_profile_snapshot_store_t &frame_profile_snapshot_store() noexcept {
    static frame_profile_snapshot_store_t store;
    return store;
  }
}  // namespace video
