/**
 * @file src/platform/linux/hdmirx.h
 * @brief V4L2 HDMI RX capture and DMA-BUF export helpers.
 *
 * This is deliberately a small, Linux-only producer.  It never changes the
 * receiver's negotiated format: ownership of that decision remains with the
 * HDMI RX driver and its upstream source.
 */
#pragma once

#include "src/platform/common.h"
#include "src/platform/linux/input_state_machine.h"

#include <chrono>
#include <cstdint>
#include <linux/videodev2.h>
#include <memory>
#include <mpp_frame.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace platf::hdmirx {
  /**
   * @brief Decide whether display initialization must use a synthetic frame.
   *
   * Encoder capability and session construction are independent from the
   * source's instantaneous HDMI signal state. Both read-only probes and real
   * streams therefore bootstrap with a target-sized placeholder. Only the
   * capture loop may wait for and publish real HDMI frames.
   *
   * @param purpose Purpose assigned to the display instance.
   * @return true for every supported HDMI RX display purpose.
   */
  constexpr bool uses_synthetic_bootstrap_frame(display_purpose_e purpose) noexcept {
    return purpose == display_purpose_e::encoder_probe || purpose == display_purpose_e::stream;
  }

  /** @brief Point in a captured frame at which the V4L2 timestamp was taken. */
  enum class timestamp_source_e {
    end_of_frame,  ///< The timestamp represents the end of frame reception.
    start_of_exposure,  ///< The timestamp represents the start of exposure/frame reception.
    unknown,  ///< The driver returned an unrecognized timestamp-source value.
  };

  struct plane_layout_t {
    std::uint32_t bytesperline {};
    std::uint32_t sizeimage {};
    std::uint32_t allocation_size {};
  };

  struct capture_format_t {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t fourcc {};
    std::uint32_t field {};
    std::uint32_t colorspace {};
    std::vector<plane_layout_t> planes;
    MppFrameFormat mpp_format {};
  };

  struct device_info_t {
    std::string driver;
    std::string card;
  };

  struct frame_plane_t {
    std::uint32_t bytesused {};
    std::uint32_t data_offset {};
    std::uint32_t payload_bytes {};
    std::uint32_t bytesperline {};
    std::uint32_t sizeimage {};
    // QUERYBUF allocation length; this may exceed the populated payload.
    std::uint32_t allocation_size {};
    int dma_buf_fd {-1};
  };

  /**
   * Translate an HDMI RX V4L2 fourcc to the directly importable MPP format.
   * No fallback is provided: callers must reject a new or unsupported driver
   * format instead of silently requesting a CPU conversion path.
   */
  std::optional<MppFrameFormat> fourcc_to_mpp_format(std::uint32_t fourcc) noexcept;

  /** Validate the format metadata returned by VIDIOC_G_FMT. */
  bool capture_format_is_valid(const capture_format_t &format) noexcept;

  /**
   * @brief Test whether V4L2 marked a buffer timestamp as CLOCK_MONOTONIC.
   *
   * @param flags Flags returned in `v4l2_buffer::flags`.
   * @return True only for `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC`.
   */
  bool timestamp_is_monotonic(std::uint32_t flags) noexcept;

  /**
   * @brief Decode the V4L2 timestamp source carried by a captured buffer.
   *
   * @param flags Flags returned in `v4l2_buffer::flags`.
   * @return Decoded source relative to the captured frame.
   */
  timestamp_source_e timestamp_source(std::uint32_t flags) noexcept;

  /**
   * @brief Return a stable diagnostic name for a timestamp source.
   *
   * @param source Source to describe.
   * @return Static diagnostic name.
   */
  std::string_view timestamp_source_name(timestamp_source_e source) noexcept;

  /**
   * @brief Convert a V4L2 CLOCK_MONOTONIC timeval to the application timeline.
   *
   * @param timestamp V4L2 timestamp to convert.
   * @return Timestamp on Linux's `std::chrono::steady_clock` timeline.
   */
  std::chrono::steady_clock::time_point v4l2_monotonic_timestamp(const timeval &timestamp) noexcept;

  /**
   * @brief Verify that steady_clock and CLOCK_MONOTONIC use compatible epochs.
   *
   * @return True when the two clocks differ by no more than the scheduling
   * delay required to sample them.
   */
  bool steady_clock_matches_monotonic() noexcept;

  /**
   * @brief A recoverable V4L2 source-change notification.
   *
   * This is deliberately distinct from malformed buffer metadata and device
   * I/O failures. Callers must enter their placeholder/recovery policy rather
   * than ending the Sunshine session.
   */
  class source_change_error_t final: public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
  };

  namespace detail {
    struct capture_state_t;
  }

  /**
   * A dequeued V4L2 buffer.  Its DMA-BUF fds remain valid until release() or
   * destruction.  Releasing the frame requeues its V4L2 buffer exactly once.
   */
  class captured_frame_t {
  public:
    captured_frame_t() = default;
    captured_frame_t(const captured_frame_t &) = delete;
    captured_frame_t &operator=(const captured_frame_t &) = delete;
    captured_frame_t(captured_frame_t &&other) noexcept;
    captured_frame_t &operator=(captured_frame_t &&other) noexcept;
    ~captured_frame_t();

    void release();
    bool released() const noexcept;
    std::uint32_t sequence() const noexcept;
    /** @brief Return the capture allocation generation that owns this buffer. */
    std::uint64_t generation() const noexcept;
    /** @brief Return this buffer's stable index inside its allocation generation. */
    std::uint32_t buffer_index() const noexcept;
    /**
     * V4L2's CLOCK_MONOTONIC timestamp, represented on Linux's
     * steady_clock timeline.  Frames lacking TIMESTAMP_MONOTONIC are rejected.
     */
    std::chrono::steady_clock::time_point timestamp() const noexcept;
    /** @brief Return the time immediately before the successful VIDIOC_DQBUF call began. */
    std::chrono::steady_clock::time_point dequeue_begin_timestamp() const noexcept;
    /** @brief Return the time immediately after VIDIOC_DQBUF succeeded. */
    std::chrono::steady_clock::time_point dequeue_timestamp() const noexcept;
    std::uint32_t timestamp_flags() const noexcept;
    /**
     * @brief Return the number of older frames dropped before this frame was selected.
     *
     * The count is local to one `dequeue()` call and represents frames
     * immediately requeued while draining an already-backlogged V4L2 queue.
     *
     * @return Older complete frames proactively returned to the driver.
     */
    std::uint32_t freshness_drops() const noexcept;
    const std::vector<frame_plane_t> &planes() const noexcept;

  private:
    friend class hdmirx_capture_t;
    captured_frame_t(std::shared_ptr<detail::capture_state_t> state, std::uint64_t generation, std::uint32_t index, std::uint32_t sequence, std::chrono::steady_clock::time_point timestamp, std::chrono::steady_clock::time_point dequeue_begin_timestamp, std::chrono::steady_clock::time_point dequeue_timestamp, std::uint32_t timestamp_flags, std::uint32_t freshness_drops, std::vector<frame_plane_t> planes) noexcept;

    void release_noexcept() noexcept;

    std::shared_ptr<detail::capture_state_t> state_;
    std::uint64_t generation_ {};
    std::uint32_t index_ {};
    std::uint32_t sequence_ {};
    std::chrono::steady_clock::time_point timestamp_ {};
    std::chrono::steady_clock::time_point dequeue_begin_timestamp_ {};
    std::chrono::steady_clock::time_point dequeue_timestamp_ {};
    std::uint32_t timestamp_flags_ {};
    std::uint32_t freshness_drops_ {};
    std::vector<frame_plane_t> planes_;
    bool released_ {true};
  };

  /**
   * @brief HDMI RX image whose shared frame lease pins a dequeued DMA-BUF.
   *
   * RKMPP receives the same shared lease as its generic input-frame holder.
   * The buffer is therefore requeued only after synchronous encode consumption.
   */
  struct hdmirx_img_t final: img_t {
    std::shared_ptr<captured_frame_t> frame;
    // Snapshot of the V4L2 layout that produced frame. A source-change
    // recovery can reopen the receiver with a new width, stride or format;
    // consumers must never infer those properties from the encoder setup.
    std::optional<capture_format_t> capture_format;
    // A state-machine placeholder. It deliberately carries no HDMI RX lease:
    // the encoder must fill its own target-sized DMA-BUF through RGA.
    bool placeholder {};
    // Consumed by the RKMPP encode session to request an IDR for the first
    // placeholder and the first recovered real frame.
    bool request_idr {};
    input_sm::state_e connection_state {input_sm::state_e::starting};  ///< HDMI RX state for the UI snapshot.
    std::uint32_t moonlight_width {};  ///< Requested Moonlight width for the UI snapshot.
    std::uint32_t moonlight_height {};  ///< Requested Moonlight height for the UI snapshot.
    std::uint32_t input_width {};  ///< Current HDMI input width, or zero when unavailable.
    std::uint32_t input_height {};  ///< Current HDMI input height, or zero when unavailable.
  };

  /**
   * Owns a rk_hdmirx VIDEO_CAPTURE_MPLANE stream.
   *
   * The object uses V4L2_MEMORY_MMAP solely for the queueing/export protocol;
   * it intentionally does not map or copy pixels.  Every requested plane is
   * exported with VIDIOC_EXPBUF as a DMA-BUF.
   */
  class hdmirx_capture_t {
  public:
    /**
     * @brief Open and start an RK3588 HDMI RX capture stream.
     *
     * @param device V4L2 multi-planar HDMI RX device path.
     * @param requested_buffers Number of capture slots requested from the driver.
     * @return Active HDMI RX capture object.
     */
    static hdmirx_capture_t open(const std::string &device = "/dev/video0", std::uint32_t requested_buffers = 4);

    hdmirx_capture_t() = default;
    hdmirx_capture_t(const hdmirx_capture_t &) = delete;
    hdmirx_capture_t &operator=(const hdmirx_capture_t &) = delete;
    hdmirx_capture_t(hdmirx_capture_t &&) noexcept = default;
    hdmirx_capture_t &operator=(hdmirx_capture_t &&other) noexcept;
    ~hdmirx_capture_t();

    /**
     * @brief Return the newest complete V4L2 frame available before the deadline.
     *
     * The call blocks until one frame is ready, then drains already-ready
     * frames without blocking. Every older drained frame is immediately
     * requeued before the newest frame is returned.
     *
     * @param timeout Maximum time to wait for the first frame.
     * @return Newest complete frame and its local freshness-drop count.
     * @throws std::runtime_error When the stream stops, times out, or returns invalid metadata.
     */
    captured_frame_t dequeue(std::chrono::milliseconds timeout);

    /**
     * @brief Reopen the receiver after a source change once all leases return.
     *
     * The replacement performs fresh QUERY_DV_TIMINGS, G_FMT, MMAP queue and
     * DMA-BUF export setup. Returns false while an old frame is still leased;
     * callers should keep their placeholder policy active and retry later.
     */
    bool recover_after_source_change();

    /** Re-query the receiver's actual DV timings without changing its queue. */
    std::optional<v4l2_dv_timings> refresh_timings();
    /** Stop the stream; outstanding frames remain valid but will not QBUF. */
    void shutdown() noexcept;
    int fd() const;
    const capture_format_t &format() const;
    const std::optional<v4l2_dv_timings> &timings() const;
    const device_info_t &device_info() const;
    std::uint32_t buffer_count() const;

  private:
    explicit hdmirx_capture_t(std::shared_ptr<detail::capture_state_t> state) noexcept;
    std::shared_ptr<detail::capture_state_t> state_;
  };
}  // namespace platf::hdmirx

namespace platf {
  std::shared_ptr<display_t> hdmirx_display(const video::config_t &config, display_purpose_e purpose);
}
