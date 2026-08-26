/**
 * @file src/platform/linux/hdmirx.h
 * @brief V4L2 HDMI RX capture and DMA-BUF export helpers.
 *
 * This is deliberately a small, Linux-only producer.  It never changes the
 * receiver's negotiated format: ownership of that decision remains with the
 * HDMI RX driver and its upstream source.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <linux/videodev2.h>
#include <mpp_frame.h>

#include "src/platform/common.h"

namespace platf::hdmirx {
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
    /**
     * V4L2's CLOCK_MONOTONIC timestamp, represented on Linux's
     * steady_clock timeline.  Frames lacking TIMESTAMP_MONOTONIC are rejected.
     */
    std::chrono::steady_clock::time_point timestamp() const noexcept;
    std::uint32_t timestamp_flags() const noexcept;
    const std::vector<frame_plane_t> &planes() const noexcept;

  private:
    friend class hdmirx_capture_t;
    captured_frame_t(std::shared_ptr<detail::capture_state_t> state, std::uint32_t index, std::uint32_t sequence, std::chrono::steady_clock::time_point timestamp, std::uint32_t timestamp_flags, std::vector<frame_plane_t> planes) noexcept;

    void release_noexcept() noexcept;

    std::shared_ptr<detail::capture_state_t> state_;
    std::uint32_t index_ {};
    std::uint32_t sequence_ {};
    std::chrono::steady_clock::time_point timestamp_ {};
    std::uint32_t timestamp_flags_ {};
    std::vector<frame_plane_t> planes_;
    bool released_ {true};
  };

  // A captured frame is held here until synchronous RKMPP encoding returns.
  struct hdmirx_img_t final: img_t {
    std::optional<captured_frame_t> frame;
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
    static hdmirx_capture_t open(const std::string &device = "/dev/video0", std::uint32_t requested_buffers = 6);

    hdmirx_capture_t() = default;
    hdmirx_capture_t(const hdmirx_capture_t &) = delete;
    hdmirx_capture_t &operator=(const hdmirx_capture_t &) = delete;
    hdmirx_capture_t(hdmirx_capture_t &&) noexcept = default;
    hdmirx_capture_t &operator=(hdmirx_capture_t &&other) noexcept;
    ~hdmirx_capture_t();

    captured_frame_t dequeue(std::chrono::milliseconds timeout);
    /** Stop the stream; outstanding frames remain valid but will not QBUF. */
    void shutdown() noexcept;
    const capture_format_t &format() const;
    const std::optional<v4l2_dv_timings> &timings() const;
    const device_info_t &device_info() const;
    std::uint32_t buffer_count() const;

  private:
    explicit hdmirx_capture_t(std::shared_ptr<detail::capture_state_t> state) noexcept;
    std::shared_ptr<detail::capture_state_t> state_;
  };
}  // namespace platf::hdmirx

namespace platf { std::shared_ptr<display_t> hdmirx_display(const video::config_t &config); }
