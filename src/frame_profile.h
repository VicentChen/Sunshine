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
    std::optional<time_point> ui_render_begin;  ///< Time immediately before a changed Vulkan UI snapshot was rendered.
    std::optional<time_point> ui_render_end;  ///< Time immediately after the changed Vulkan UI snapshot finished rendering.
    std::optional<time_point> ui_compose_begin;  ///< Time immediately before the cached UI panel was composed with RGA.
    std::optional<time_point> ui_compose_end;  ///< Time immediately after the cached UI panel was composed with RGA.
    std::optional<time_point> mpp_import_begin;  ///< Time immediately before a non-cached MPP DMA-BUF import.
    std::optional<time_point> mpp_import_end;  ///< Time immediately after a non-cached MPP DMA-BUF import.
    std::optional<time_point> mpp_output_buffer_begin;  ///< Time immediately before MPP allocates an output buffer.
    std::optional<time_point> mpp_output_buffer_end;  ///< Time immediately after MPP allocates an output buffer.
    std::optional<time_point> mpp_output_packet_begin;  ///< Time immediately before MPP initializes the output packet wrapper.
    std::optional<time_point> mpp_output_packet_end;  ///< Time immediately after MPP initializes the output packet wrapper.
    std::optional<time_point> mpp_prep_begin;  ///< Time immediately before frame metadata and submission state are prepared.
    std::optional<time_point> mpp_prep_end;  ///< Time immediately before the blocking encode call begins.
    std::optional<time_point> mpp_encode_begin;  ///< Time immediately before blocking encode_put_frame begins.
    std::optional<time_point> mpp_encode_end;  ///< Time immediately after blocking encode_put_frame returns.
    std::optional<time_point> mpp_packet_get_begin;  ///< Time immediately before polling encode_get_packet.
    std::optional<time_point> mpp_packet_get_end;  ///< Time a complete non-empty packet is returned by encode_get_packet.
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
    mpp_prep,  ///< Application and MPP submission preparation before blocking encode.
    mpp_encode,  ///< Blocking encode_put_frame, including driver queue and hardware work.
    mpp_packet_get,  ///< Time spent retrieving the completed output packet.
    mpp_total,  ///< MPP preparation entry to complete MppPacket.
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

  /** @brief Concrete work spans retained for the real-time frame timeline. */
  enum class frame_profile_timeline_stage_e : std::uint8_t {
    rx_driver_age,  ///< Observable RX EOF timestamp to successful dequeue.
    capture_queue,  ///< Successful dequeue to encoder-thread processing.
    rga,  ///< Video conversion, scaling, fill, or letterbox work.
    ui_render,  ///< Changed Dear ImGui snapshot rendered through Vulkan.
    ui_compose,  ///< Cached Vulkan UI panel composed into the video through RGA.
    mpp_import,  ///< Non-cached MPP DMA-BUF import.
    mpp_output_buffer_acquire,  ///< MPP output-buffer allocation.
    mpp_output_packet_init,  ///< MPP output-packet wrapper initialization.
    mpp_prep,  ///< Frame metadata and MPP submission preparation.
    mpp_encode,  ///< Blocking encode_put_frame including hardware completion.
    mpp_packet_get,  ///< Retrieval of the completed MppPacket.
    encoded_queue,  ///< Complete MppPacket to network-thread processing.
    packetize_send,  ///< Network-thread processing to final send return.
    count  ///< Number of concrete timeline stages.
  };

  /** @brief Execution resources used to place overlapping spans on stable rows. */
  enum class frame_profile_timeline_lane_e : std::uint8_t {
    capture,  ///< HDMI RX dequeue and capture queue.
    rga,  ///< RGA video conversion or fill.
    ui,  ///< Vulkan rendering and RGA UI composition.
    mpp,  ///< MPP preparation, submission, and output wait.
    network,  ///< Encoded queue, packetization, and send.
    count  ///< Number of timeline lanes.
  };

  /** @brief One valid concrete stage interval relative to its frame's RX EOF origin. */
  struct frame_profile_timeline_span_t {
    frame_profile_timeline_stage_e stage {frame_profile_timeline_stage_e::rx_driver_age};  ///< Stable stage identifier.
    frame_profile_timeline_lane_e lane {frame_profile_timeline_lane_e::capture};  ///< Execution row used by the renderer.
    std::int64_t start_us {};  ///< Stage start relative to RX EOF in microseconds.
    std::int64_t end_us {};  ///< Stage end relative to RX EOF in microseconds.

    bool operator==(const frame_profile_timeline_span_t &) const = default;
  };

  /** @brief Fixed-size timeline for one completed captured frame. */
  struct frame_profile_timeline_frame_t {
    static constexpr auto stage_count = static_cast<std::size_t>(frame_profile_timeline_stage_e::count);
    static constexpr auto max_spans = stage_count;

    std::array<frame_profile_timeline_span_t, max_spans> spans;  ///< Valid spans in stage order.
    std::uint32_t capture_sequence {};  ///< V4L2 sequence for correlation.
    std::int64_t frame_index {-1};  ///< Sunshine frame index for correlation.
    std::int64_t origin_offset_us {};  ///< RX EOF relative to the current stream epoch.
    std::int64_t end_us {};  ///< Final send return relative to RX EOF.
    std::uint32_t missing_stage_mask {};  ///< Concrete stages lacking either timestamp.
    std::uint32_t invalid_stage_mask {};  ///< Concrete stages with negative or out-of-frame bounds.
    std::uint8_t span_count {};  ///< Number of populated entries in spans.
    bool rga_bypass {};  ///< Whether video conversion/fill bypassed RGA.

    bool operator==(const frame_profile_timeline_frame_t &) const = default;
  };

  /** @brief Immutable oldest-to-newest ring snapshot consumed by the Vulkan UI. */
  struct frame_profile_timeline_snapshot_t {
    static constexpr std::size_t frame_capacity = 32;

    std::array<frame_profile_timeline_frame_t, frame_capacity> frames;  ///< Completed frames ordered from oldest to newest.
    std::uint32_t frame_count {};  ///< Number of populated frames.
    std::uint32_t rejected_frames {};  ///< Captured profiles rejected because the frame boundary was missing or invalid.
    std::uint64_t stream_generation {};  ///< Generation changed when the producing video sender resets.

    bool operator==(const frame_profile_timeline_snapshot_t &) const = default;
  };

  /** @brief Return the stable display name for a concrete timeline stage. */
  constexpr std::string_view frame_profile_timeline_stage_name(frame_profile_timeline_stage_e stage) noexcept {
    switch (stage) {
      case frame_profile_timeline_stage_e::rx_driver_age:
        return "RX EOF-DQ";
      case frame_profile_timeline_stage_e::capture_queue:
        return "CAP QUEUE";
      case frame_profile_timeline_stage_e::rga:
        return "RGA";
      case frame_profile_timeline_stage_e::ui_render:
        return "UI RENDER";
      case frame_profile_timeline_stage_e::ui_compose:
        return "UI COMPOSE";
      case frame_profile_timeline_stage_e::mpp_import:
        return "MPP IMPORT";
      case frame_profile_timeline_stage_e::mpp_output_buffer_acquire:
        return "MPP OUT BUF";
      case frame_profile_timeline_stage_e::mpp_output_packet_init:
        return "MPP PACKET INIT";
      case frame_profile_timeline_stage_e::mpp_prep:
        return "MPP PREP";
      case frame_profile_timeline_stage_e::mpp_encode:
        return "MPP ENCODE";
      case frame_profile_timeline_stage_e::mpp_packet_get:
        return "MPP PACKET GET";
      case frame_profile_timeline_stage_e::encoded_queue:
        return "ENC QUEUE";
      case frame_profile_timeline_stage_e::packetize_send:
        return "PACKET-SEND";
      default:
        return "UNKNOWN";
    }
  }

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
      case frame_profile_metric_e::mpp_prep:
        return "MPP prep";
      case frame_profile_metric_e::mpp_encode:
        return "MPP encode";
      case frame_profile_metric_e::mpp_packet_get:
        return "MPP packet get";
      case frame_profile_metric_e::mpp_total:
        return "MPP total";
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
      collect_metric(frame_profile_metric_e::mpp_prep, profile.mpp_prep_begin, profile.mpp_prep_end);
      collect_metric(frame_profile_metric_e::mpp_encode, profile.mpp_encode_begin, profile.mpp_encode_end);
      collect_metric(frame_profile_metric_e::mpp_packet_get, profile.mpp_packet_get_begin, profile.mpp_packet_get_end);
      collect_metric(frame_profile_metric_e::mpp_total, profile.mpp_prep_begin, profile.mpp_output);
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

  /** @brief Build one renderer-ready timeline frame from completed raw timestamps. */
  inline std::optional<frame_profile_timeline_frame_t> make_frame_profile_timeline_frame(
    const frame_profile_t &profile,
    frame_profile_t::time_point stream_epoch
  ) noexcept {
    if (profile.kind != frame_profile_kind_e::captured || !profile.capture || !profile.send_end || *profile.send_end < *profile.capture) {
      return std::nullopt;
    }

    frame_profile_timeline_frame_t frame {
      .capture_sequence = profile.capture_sequence,
      .frame_index = profile.frame_index,
      .origin_offset_us = std::chrono::duration_cast<std::chrono::microseconds>(*profile.capture - stream_epoch).count(),
      .end_us = std::chrono::duration_cast<std::chrono::microseconds>(*profile.send_end - *profile.capture).count(),
      .rga_bypass = !profile.rga_used
    };
    auto append = [&](frame_profile_timeline_stage_e stage, frame_profile_timeline_lane_e lane, const std::optional<frame_profile_t::time_point> &start, const std::optional<frame_profile_t::time_point> &end) {
      const auto bit = 1U << static_cast<std::uint8_t>(stage);
      if (!start || !end) {
        frame.missing_stage_mask |= bit;
        return;
      }
      const auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(*start - *profile.capture).count();
      const auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(*end - *profile.capture).count();
      if (start_us < 0 || end_us < start_us || end_us > frame.end_us) {
        frame.invalid_stage_mask |= bit;
        return;
      }
      frame.spans[frame.span_count++] = {stage, lane, start_us, end_us};
    };

    append(frame_profile_timeline_stage_e::rx_driver_age, frame_profile_timeline_lane_e::capture, profile.capture, profile.dequeued);
    append(frame_profile_timeline_stage_e::capture_queue, frame_profile_timeline_lane_e::capture, profile.dequeued, profile.capture_queue_exit);
    if (profile.rga_used) {
      append(frame_profile_timeline_stage_e::rga, frame_profile_timeline_lane_e::rga, profile.rga_begin, profile.rga_end);
    }
    if (profile.ui_render_begin || profile.ui_render_end) {
      append(frame_profile_timeline_stage_e::ui_render, frame_profile_timeline_lane_e::ui, profile.ui_render_begin, profile.ui_render_end);
    }
    if (profile.ui_compose_begin || profile.ui_compose_end) {
      append(frame_profile_timeline_stage_e::ui_compose, frame_profile_timeline_lane_e::ui, profile.ui_compose_begin, profile.ui_compose_end);
    }
    append(frame_profile_timeline_stage_e::mpp_import, frame_profile_timeline_lane_e::mpp, profile.mpp_import_begin, profile.mpp_import_end);
    append(frame_profile_timeline_stage_e::mpp_output_buffer_acquire, frame_profile_timeline_lane_e::mpp, profile.mpp_output_buffer_begin, profile.mpp_output_buffer_end);
    append(frame_profile_timeline_stage_e::mpp_output_packet_init, frame_profile_timeline_lane_e::mpp, profile.mpp_output_packet_begin, profile.mpp_output_packet_end);
    append(frame_profile_timeline_stage_e::mpp_prep, frame_profile_timeline_lane_e::mpp, profile.mpp_prep_begin, profile.mpp_prep_end);
    append(frame_profile_timeline_stage_e::mpp_encode, frame_profile_timeline_lane_e::mpp, profile.mpp_encode_begin, profile.mpp_encode_end);
    append(frame_profile_timeline_stage_e::mpp_packet_get, frame_profile_timeline_lane_e::mpp, profile.mpp_packet_get_begin, profile.mpp_packet_get_end);
    append(frame_profile_timeline_stage_e::encoded_queue, frame_profile_timeline_lane_e::network, profile.mpp_output, profile.packetize_begin);
    append(frame_profile_timeline_stage_e::packetize_send, frame_profile_timeline_lane_e::network, profile.packetize_begin, profile.send_end);
    return frame;
  }

  /** @brief Thread-safe fixed ring of completed frames for the real-time Timeline page. */
  class frame_profile_timeline_store_t {
  public:
    /** @brief Reset the producer epoch and begin a new stream generation. */
    void reset() noexcept {
      std::lock_guard<std::mutex> lock(mutex_);
      frames_ = {};
      next_ = 0;
      used_ = 0;
      rejected_frames_ = 0;
      epoch_.reset();
      ++stream_generation_;
      ++publication_generation_;
    }

    /** @brief Publish one completed captured frame into the bounded ring. */
    bool publish(const frame_profile_t &profile) noexcept {
      if (profile.kind != frame_profile_kind_e::captured) {
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (!profile.capture || !profile.send_end || *profile.send_end < *profile.capture) {
        ++rejected_frames_;
        ++publication_generation_;
        return false;
      }
      if (!epoch_) {
        epoch_ = profile.capture;
      }
      auto frame = make_frame_profile_timeline_frame(profile, *epoch_);
      if (!frame) {
        ++rejected_frames_;
        ++publication_generation_;
        return false;
      }
      frames_[next_] = *frame;
      next_ = (next_ + 1U) % frame_profile_timeline_snapshot_t::frame_capacity;
      used_ = std::min<std::size_t>(used_ + 1U, frame_profile_timeline_snapshot_t::frame_capacity);
      ++publication_generation_;
      return true;
    }

    /** @brief Copy a newer oldest-to-newest Timeline snapshot for a reader. */
    bool read_newer(std::uint64_t &generation, frame_profile_timeline_snapshot_t &snapshot) noexcept {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation == publication_generation_) {
        return false;
      }
      snapshot = {};
      snapshot.frame_count = static_cast<std::uint32_t>(used_);
      snapshot.rejected_frames = rejected_frames_;
      snapshot.stream_generation = stream_generation_;
      const auto oldest = used_ == frame_profile_timeline_snapshot_t::frame_capacity ? next_ : 0U;
      for (std::size_t index = 0; index < used_; ++index) {
        snapshot.frames[index] = frames_[(oldest + index) % frame_profile_timeline_snapshot_t::frame_capacity];
      }
      generation = publication_generation_;
      return true;
    }

  private:
    std::mutex mutex_;  ///< Protects the fixed ring and producer epoch.
    std::array<frame_profile_timeline_frame_t, frame_profile_timeline_snapshot_t::frame_capacity> frames_;  ///< Circular completed-frame storage.
    std::optional<frame_profile_t::time_point> epoch_;  ///< RX EOF of the first accepted frame in the current stream.
    std::size_t next_ {};  ///< Slot overwritten by the next accepted frame.
    std::size_t used_ {};  ///< Number of valid ring slots.
    std::uint32_t rejected_frames_ {};  ///< Invalid captured profiles in this stream.
    std::uint64_t stream_generation_ {};  ///< Producer-reset generation.
    std::uint64_t publication_generation_ {};  ///< Any observable store change.
  };

  /** @brief Return the process-wide completed-frame Timeline store. */
  inline frame_profile_timeline_store_t &frame_profile_timeline_store() noexcept {
    static frame_profile_timeline_store_t store;
    return store;
  }
}  // namespace video
