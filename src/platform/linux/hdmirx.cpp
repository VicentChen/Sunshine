/**
 * @file src/platform/linux/hdmirx.cpp
 * @brief V4L2 HDMI RX capture and DMA-BUF export implementation.
 */
#include "src/platform/linux/hdmirx.h"
#include "src/platform/linux/edid.h"
#include "src/platform/linux/input_state_machine.h"
#include "src/platform/linux/rkmpp.h"
#include "src/logging.h"
#include "src/video.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace platf::hdmirx {
  namespace {

    constexpr auto k_buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    constexpr auto k_memory_type = V4L2_MEMORY_MMAP;

    int ioctl_retry(int fd, unsigned long request, void *argument) noexcept {
      int result;
      do {
        result = ::ioctl(fd, request, argument);
      } while (result == -1 && errno == EINTR);
      return result;
    }

    void checked_ioctl(int fd, unsigned long request, void *argument, const char *operation) {
      if (ioctl_retry(fd, request, argument) == -1) {
        throw std::system_error(errno, std::generic_category(), operation);
      }
    }

    std::string c_string(const __u8 *value) {
      return reinterpret_cast<const char *>(value);
    }

    int remaining_poll_timeout(std::chrono::steady_clock::time_point deadline) noexcept {
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return 0;
      }
      const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining + std::chrono::milliseconds(1)).count();
      return milliseconds > INT_MAX ? INT_MAX : static_cast<int>(milliseconds);
    }

    void close_fd_noexcept(int &fd) noexcept {
      if (fd >= 0) {
        (void) ::close(fd);
        fd = -1;
      }
    }
  }  // namespace

  std::optional<MppFrameFormat> fourcc_to_mpp_format(std::uint32_t fourcc) noexcept {
    switch (fourcc) {
      case V4L2_PIX_FMT_BGR24:
        return MPP_FMT_BGR888;
      case V4L2_PIX_FMT_NV24:
        return MPP_FMT_YUV444SP;
      case V4L2_PIX_FMT_NV16:
        return MPP_FMT_YUV422SP;
      case V4L2_PIX_FMT_NV12:
        return MPP_FMT_YUV420SP;
      default:
        return std::nullopt;
    }
  }

  bool capture_format_is_valid(const capture_format_t &format) noexcept {
    if (format.width == 0 || format.height == 0 || format.planes.empty() || format.planes.size() > VIDEO_MAX_PLANES || !fourcc_to_mpp_format(format.fourcc).has_value()) {
      return false;
    }

    return std::all_of(format.planes.begin(), format.planes.end(), [](const plane_layout_t &plane) {
      return plane.bytesperline != 0 && plane.sizeimage != 0;
    });
  }

  bool timestamp_is_monotonic(std::uint32_t flags) noexcept {
    return (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
  }

  timestamp_source_e timestamp_source(std::uint32_t flags) noexcept {
    switch (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) {
      case V4L2_BUF_FLAG_TSTAMP_SRC_EOF:
        return timestamp_source_e::end_of_frame;
      case V4L2_BUF_FLAG_TSTAMP_SRC_SOE:
        return timestamp_source_e::start_of_exposure;
      default:
        return timestamp_source_e::unknown;
    }
  }

  std::string_view timestamp_source_name(timestamp_source_e source) noexcept {
    switch (source) {
      case timestamp_source_e::end_of_frame:
        return "EOF";
      case timestamp_source_e::start_of_exposure:
        return "SOE";
      default:
        return "unknown";
    }
  }

  std::chrono::steady_clock::time_point v4l2_monotonic_timestamp(const timeval &timestamp) noexcept {
    const auto duration = std::chrono::seconds(timestamp.tv_sec) + std::chrono::microseconds(timestamp.tv_usec);
    return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
  }

  bool steady_clock_matches_monotonic() noexcept {
    timespec monotonic {};
    const auto steady_before = std::chrono::steady_clock::now();
    if (::clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0) {
      return false;
    }
    const auto steady_after = std::chrono::steady_clock::now();
    const auto monotonic_time = std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::seconds(monotonic.tv_sec) + std::chrono::nanoseconds(monotonic.tv_nsec)
      )
    );
    return monotonic_time >= steady_before && monotonic_time <= steady_after;
  }

  namespace detail {
    struct buffer_t {
      std::vector<std::uint32_t> lengths;
      std::vector<int> dma_buf_fds;
      bool queued {};
    };

    struct capture_state_t {
      int video_fd {-1};
      std::string device_path;
      std::uint32_t requested_buffers {};
      bool streaming {};
      bool closing {};
      bool broken {};
      bool buffers_requested {};
      capture_format_t format;
      std::optional<v4l2_dv_timings> timings;
      device_info_t device_info;
      std::optional<std::uint32_t> timestamp_metadata;
      std::vector<buffer_t> buffers;
      std::mutex mutex;

      ~capture_state_t() {
        cleanup_noexcept();
      }

      void streamoff_locked_noexcept() noexcept {
        if (video_fd >= 0 && streaming) {
          auto type = k_buffer_type;
          (void) ioctl_retry(video_fd, VIDIOC_STREAMOFF, &type);
          streaming = false;
        }
      }

      void mark_broken_locked() noexcept {
        broken = true;
        closing = true;
        streamoff_locked_noexcept();
      }

      void shutdown_noexcept() noexcept {
        std::lock_guard lock(mutex);
        closing = true;
        streamoff_locked_noexcept();
      }

      void cleanup_noexcept() noexcept {
        std::lock_guard lock(mutex);
        closing = true;
        streamoff_locked_noexcept();
        // DMA-BUF handles must no longer refer to the allocation before the
        // driver is asked to release its MMAP queue.
        for (auto &buffer : buffers) {
          for (auto &dma_buf_fd : buffer.dma_buf_fds) {
            close_fd_noexcept(dma_buf_fd);
          }
        }
        if (video_fd >= 0 && buffers_requested) {
          v4l2_requestbuffers request {};
          request.type = k_buffer_type;
          request.memory = k_memory_type;
          if (ioctl_retry(video_fd, VIDIOC_REQBUFS, &request) == -1) {
            std::fprintf(stderr, "hdmirx: VIDIOC_REQBUFS(0) cleanup failed: %s\\n", std::strerror(errno));
          }
          buffers_requested = false;
        }
        buffers.clear();
        close_fd_noexcept(video_fd);
      }

      bool queue_buffer_locked(std::uint32_t index) {
        if (closing || broken) {
          return false;
        }
        if (video_fd < 0) {
          throw std::runtime_error("HDMI RX device is no longer open");
        }
        if (index >= buffers.size()) {
          throw std::runtime_error("HDMI RX driver returned an invalid buffer index");
        }
        auto &stored = buffers[index];
        if (stored.queued) {
          throw std::runtime_error("HDMI RX buffer was released twice");
        }

        std::vector<v4l2_plane> planes(stored.lengths.size());
        for (std::size_t plane = 0; plane < planes.size(); ++plane) {
          planes[plane].length = stored.lengths[plane];
        }
        v4l2_buffer buffer {};
        buffer.type = k_buffer_type;
        buffer.memory = k_memory_type;
        buffer.index = index;
        buffer.length = static_cast<__u32>(planes.size());
        buffer.m.planes = planes.data();
        try {
          checked_ioctl(video_fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF failed while returning HDMI RX frame");
        } catch (...) {
          mark_broken_locked();
          throw;
        }
        stored.queued = true;
        return true;
      }

      bool queue_buffer(std::uint32_t index) {
        std::lock_guard lock(mutex);
        return queue_buffer_locked(index);
      }
    };
  }  // namespace detail

  captured_frame_t::captured_frame_t(std::shared_ptr<detail::capture_state_t> state, std::uint32_t index, std::uint32_t sequence, std::chrono::steady_clock::time_point timestamp, std::chrono::steady_clock::time_point dequeue_timestamp, std::uint32_t timestamp_flags, std::vector<frame_plane_t> planes) noexcept :
      state_(std::move(state)), index_(index), sequence_(sequence), timestamp_(timestamp), dequeue_timestamp_(dequeue_timestamp), timestamp_flags_(timestamp_flags), planes_(std::move(planes)), released_(false) {}

  captured_frame_t::captured_frame_t(captured_frame_t &&other) noexcept = default;

  captured_frame_t &captured_frame_t::operator=(captured_frame_t &&other) noexcept {
    if (this != &other) {
      release_noexcept();
      state_ = std::move(other.state_);
      index_ = other.index_;
      sequence_ = other.sequence_;
      timestamp_ = other.timestamp_;
      dequeue_timestamp_ = other.dequeue_timestamp_;
      timestamp_flags_ = other.timestamp_flags_;
      planes_ = std::move(other.planes_);
      released_ = other.released_;
      other.released_ = true;
    }
    return *this;
  }

  captured_frame_t::~captured_frame_t() {
    release_noexcept();
  }

  void captured_frame_t::release() {
    if (!released_ && state_) {
      (void) state_->queue_buffer(index_);
      released_ = true;
    }
  }

  bool captured_frame_t::released() const noexcept {
    return released_;
  }

  std::uint32_t captured_frame_t::sequence() const noexcept {
    return sequence_;
  }

  std::chrono::steady_clock::time_point captured_frame_t::timestamp() const noexcept {
    return timestamp_;
  }

  std::chrono::steady_clock::time_point captured_frame_t::dequeue_timestamp() const noexcept {
    return dequeue_timestamp_;
  }

  std::uint32_t captured_frame_t::timestamp_flags() const noexcept {
    return timestamp_flags_;
  }

  const std::vector<frame_plane_t> &captured_frame_t::planes() const noexcept {
    return planes_;
  }

  void captured_frame_t::release_noexcept() noexcept {
    try {
      release();
    } catch (...) {
      // Destruction must not throw.  State cleanup will stream off the device.
    }
  }

  hdmirx_capture_t::hdmirx_capture_t(std::shared_ptr<detail::capture_state_t> state) noexcept : state_(std::move(state)) {}

  hdmirx_capture_t &hdmirx_capture_t::operator=(hdmirx_capture_t &&other) noexcept {
    if (this != &other) {
      if (state_) {
        state_->shutdown_noexcept();
      }
      state_ = std::move(other.state_);
    }
    return *this;
  }

  hdmirx_capture_t::~hdmirx_capture_t() {
    shutdown();
  }

  hdmirx_capture_t hdmirx_capture_t::open(const std::string &device, std::uint32_t requested_buffers) {
    if (requested_buffers == 0) {
      throw std::invalid_argument("HDMI RX requires at least one capture buffer");
    }
    if (!steady_clock_matches_monotonic()) {
      throw std::runtime_error("Linux steady_clock is not aligned with CLOCK_MONOTONIC");
    }

    auto state = std::make_shared<detail::capture_state_t>();
    state->device_path = device;
    state->requested_buffers = requested_buffers;
    state->video_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (state->video_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "failed to open HDMI RX device " + device);
    }

    v4l2_capability capability {};
    checked_ioctl(state->video_fd, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP failed");
    const auto capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps : capability.capabilities;
    state->device_info = {c_string(capability.driver), c_string(capability.card)};
    if (state->device_info.driver != "rk_hdmirx") {
      throw std::runtime_error("HDMI RX device is not driven by rk_hdmirx: " + state->device_info.driver);
    }
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0 || (capabilities & V4L2_CAP_STREAMING) == 0) {
      throw std::runtime_error("rk_hdmirx does not expose VIDEO_CAPTURE_MPLANE streaming");
    }

    v4l2_event_subscription source_change_subscription {};
    source_change_subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    if (ioctl_retry(state->video_fd, VIDIOC_SUBSCRIBE_EVENT, &source_change_subscription) == -1 &&
        errno != EINVAL && errno != ENOTTY) {
      throw std::system_error(errno, std::generic_category(), "VIDIOC_SUBSCRIBE_EVENT(V4L2_EVENT_SOURCE_CHANGE) failed");
    }

    v4l2_dv_timings timings {};
    if (ioctl_retry(state->video_fd, VIDIOC_QUERY_DV_TIMINGS, &timings) == 0) {
      state->timings = timings;
    } else if (errno != ENOLINK && errno != ENODATA && errno != EINVAL && errno != ENOLCK) {
      throw std::system_error(errno, std::generic_category(), "VIDIOC_QUERY_DV_TIMINGS failed");
    }

    v4l2_format v4l2_format {};
    v4l2_format.type = k_buffer_type;
    checked_ioctl(state->video_fd, VIDIOC_G_FMT, &v4l2_format, "VIDIOC_G_FMT failed");
    const auto &pixel_format = v4l2_format.fmt.pix_mp;
    const auto mpp_format = fourcc_to_mpp_format(pixel_format.pixelformat);
    if (!mpp_format.has_value()) {
      throw std::runtime_error("rk_hdmirx returned unsupported V4L2 fourcc " + std::to_string(pixel_format.pixelformat));
    }
    if (pixel_format.num_planes == 0 || pixel_format.num_planes > VIDEO_MAX_PLANES) {
      throw std::runtime_error("rk_hdmirx returned an invalid V4L2 plane count");
    }
    state->format.width = pixel_format.width;
    state->format.height = pixel_format.height;
    state->format.fourcc = pixel_format.pixelformat;
    state->format.field = pixel_format.field;
    state->format.colorspace = pixel_format.colorspace;
    state->format.mpp_format = *mpp_format;
    state->format.planes.reserve(pixel_format.num_planes);
    for (std::uint32_t plane = 0; plane < pixel_format.num_planes; ++plane) {
      state->format.planes.push_back({pixel_format.plane_fmt[plane].bytesperline, pixel_format.plane_fmt[plane].sizeimage});
    }
    if (!capture_format_is_valid(state->format)) {
      throw std::runtime_error("rk_hdmirx returned incomplete V4L2 format metadata");
    }

    v4l2_requestbuffers request {};
    request.type = k_buffer_type;
    request.memory = k_memory_type;
    request.count = requested_buffers;
    checked_ioctl(state->video_fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS failed");
    state->buffers_requested = true;
    if (request.count == 0) {
      throw std::runtime_error("rk_hdmirx allocated zero capture buffers");
    }
    state->buffers.reserve(request.count);

    for (std::uint32_t index = 0; index < request.count; ++index) {
      std::vector<v4l2_plane> query_planes(state->format.planes.size());
      v4l2_buffer query {};
      query.type = k_buffer_type;
      query.memory = k_memory_type;
      query.index = index;
      query.length = static_cast<__u32>(query_planes.size());
      query.m.planes = query_planes.data();
      checked_ioctl(state->video_fd, VIDIOC_QUERYBUF, &query, "VIDIOC_QUERYBUF failed");
      if (query.length != state->format.planes.size()) {
        throw std::runtime_error("rk_hdmirx QUERYBUF plane count differs from G_FMT");
      }

      // Store ownership before VIDIOC_EXPBUF so partial export failures close
      // already-exported fds through capture_state_t's RAII cleanup.
      state->buffers.emplace_back();
      auto &stored = state->buffers.back();
      stored.lengths.reserve(query.length);
      stored.dma_buf_fds.reserve(query.length);
      for (std::uint32_t plane = 0; plane < query.length; ++plane) {
        if (query_planes[plane].length == 0) {
          throw std::runtime_error("rk_hdmirx QUERYBUF returned an empty plane");
        }
        stored.lengths.push_back(query_planes[plane].length);
        v4l2_exportbuffer export_buffer {};
        export_buffer.type = k_buffer_type;
        export_buffer.index = index;
        export_buffer.plane = plane;
        export_buffer.flags = O_CLOEXEC;
        checked_ioctl(state->video_fd, VIDIOC_EXPBUF, &export_buffer, "VIDIOC_EXPBUF failed");
        if (export_buffer.fd < 0) {
          throw std::runtime_error("rk_hdmirx exported an invalid DMA-BUF fd");
        }
        stored.dma_buf_fds.push_back(export_buffer.fd);
      }
      state->queue_buffer(index);
    }

    auto type = k_buffer_type;
    checked_ioctl(state->video_fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON failed");
    state->streaming = true;
    return hdmirx_capture_t(std::move(state));
  }

  captured_frame_t hdmirx_capture_t::dequeue(std::chrono::milliseconds timeout) {
    const auto state = state_;
    if (!state) {
      throw std::runtime_error("HDMI RX capture is not open");
    }
    if (timeout < std::chrono::milliseconds::zero()) {
      throw std::invalid_argument("HDMI RX dequeue timeout cannot be negative");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int video_fd;
      {
        std::lock_guard lock(state->mutex);
        if (state->closing || state->broken || !state->streaming) {
          throw std::runtime_error("HDMI RX stream is not active");
        }
        video_fd = state->video_fd;
      }
      // shutdown() never closes video_fd while this shared state exists, so
      // poll may run without the state mutex. STREAMOFF wakes the driver when
      // possible; the caller's timeout remains the hard upper bound.
      pollfd poll_fd {video_fd, static_cast<short>(POLLIN | POLLPRI), 0};
      int poll_result;
      do {
        poll_result = ::poll(&poll_fd, 1, remaining_poll_timeout(deadline));
      } while (poll_result == -1 && errno == EINTR);
      // Recheck after every poll outcome. shutdown() may have STREAMOFF'd the
      // device while poll was unlocked, in which case no further ioctl is safe.
      std::unique_lock lock(state->mutex);
      if (state->closing || state->broken || !state->streaming) {
        throw std::runtime_error("HDMI RX stream stopped while waiting for a frame");
      }
      if (poll_result == 0) {
        throw std::runtime_error("timed out waiting for an HDMI RX frame");
      }
      if (poll_result < 0) {
        throw std::system_error(errno, std::generic_category(), "poll failed while waiting for an HDMI RX frame");
      }
      if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error("HDMI RX device reported a poll error");
      }
      if ((poll_fd.revents & POLLPRI) != 0) {
        for (;;) {
          v4l2_event event {};
          if (ioctl_retry(state->video_fd, VIDIOC_DQEVENT, &event) == -1) {
            if (errno == EAGAIN) {
              break;
            }
            throw std::system_error(errno, std::generic_category(), "VIDIOC_DQEVENT failed");
          }
          if (event.type == V4L2_EVENT_SOURCE_CHANGE) {
            throw source_change_error_t("HDMI RX source changed");
          }
        }
      }
      if ((poll_fd.revents & POLLIN) == 0) {
        throw std::runtime_error("HDMI RX reported a V4L2 event without a frame");
      }

      std::vector<v4l2_plane> dequeue_planes(state->format.planes.size());
      v4l2_buffer dequeue_buffer {};
      dequeue_buffer.type = k_buffer_type;
      dequeue_buffer.memory = k_memory_type;
      dequeue_buffer.length = static_cast<__u32>(dequeue_planes.size());
      dequeue_buffer.m.planes = dequeue_planes.data();
      if (ioctl_retry(state->video_fd, VIDIOC_DQBUF, &dequeue_buffer) == -1) {
        if (errno == EAGAIN) {
          continue;
        }
        throw std::system_error(errno, std::generic_category(), "VIDIOC_DQBUF failed");
      }
      const auto dequeue_timestamp = std::chrono::steady_clock::now();
      if (dequeue_buffer.index >= state->buffers.size() || dequeue_buffer.length != state->format.planes.size()) {
        state->mark_broken_locked();
        throw std::runtime_error("rk_hdmirx returned invalid dequeued buffer metadata");
      }
      auto &stored = state->buffers[dequeue_buffer.index];
      if (!stored.queued) {
        state->mark_broken_locked();
        throw std::runtime_error("rk_hdmirx dequeued a buffer that was not queued");
      }
      stored.queued = false;

      std::vector<frame_plane_t> planes;
      planes.reserve(dequeue_buffer.length);
      bool valid = timestamp_is_monotonic(dequeue_buffer.flags);
      for (std::uint32_t plane = 0; plane < dequeue_buffer.length; ++plane) {
        const auto &v4l2_plane = dequeue_planes[plane];
        const auto &layout = state->format.planes[plane];
        valid = valid && v4l2_plane.data_offset <= v4l2_plane.bytesused && v4l2_plane.bytesused <= stored.lengths[plane] && v4l2_plane.bytesused <= layout.sizeimage;
      }
      if (!valid) {
        try {
          (void) state->queue_buffer_locked(dequeue_buffer.index);
        } catch (...) {
          // queue_buffer_locked already marks the state broken and stops it.
        }
        if (!stored.queued) {
          state->mark_broken_locked();
        }
        throw std::runtime_error("rk_hdmirx returned invalid plane metadata or a non-monotonic timestamp");
      }
      const auto timestamp_metadata = dequeue_buffer.flags & (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
      if (!state->timestamp_metadata) {
        state->timestamp_metadata = timestamp_metadata;
      } else if (*state->timestamp_metadata != timestamp_metadata) {
        try {
          (void) state->queue_buffer_locked(dequeue_buffer.index);
        } catch (...) {
          // queue_buffer_locked already marks the state broken and stops it.
        }
        if (!stored.queued) {
          state->mark_broken_locked();
        }
        throw std::runtime_error("rk_hdmirx changed timestamp type or source during capture");
      }
      for (std::uint32_t plane = 0; plane < dequeue_buffer.length; ++plane) {
        const auto &v4l2_plane = dequeue_planes[plane];
        const auto &layout = state->format.planes[plane];
        planes.push_back({v4l2_plane.bytesused, v4l2_plane.data_offset, v4l2_plane.bytesused - v4l2_plane.data_offset, layout.bytesperline, layout.sizeimage, stored.lengths[plane], stored.dma_buf_fds[plane]});
      }
      return captured_frame_t(state, dequeue_buffer.index, dequeue_buffer.sequence, v4l2_monotonic_timestamp(dequeue_buffer.timestamp), dequeue_timestamp, dequeue_buffer.flags, std::move(planes));
    }
  }

  std::optional<v4l2_dv_timings> hdmirx_capture_t::refresh_timings() {
    const auto state = state_;
    if (!state) {
      throw std::runtime_error("HDMI RX capture is not open");
    }

    std::lock_guard lock(state->mutex);
    if (state->closing || state->video_fd < 0) {
      throw std::runtime_error("HDMI RX capture is not active");
    }

    v4l2_dv_timings timings {};
    if (ioctl_retry(state->video_fd, VIDIOC_QUERY_DV_TIMINGS, &timings) == 0) {
      state->timings = timings;
      return timings;
    }
    if (errno == ENOLINK || errno == ENODATA || errno == EINVAL || errno == ENOLCK) {
      state->timings.reset();
      return std::nullopt;
    }
    throw std::system_error(errno, std::generic_category(), "VIDIOC_QUERY_DV_TIMINGS failed while recovering HDMI RX");
  }

  bool hdmirx_capture_t::recover_after_source_change() {
    const auto old_state = state_;
    if (!old_state) {
      throw std::runtime_error("HDMI RX capture is not open");
    }

    std::string device;
    std::uint32_t requested_buffers = 0;
    {
      std::lock_guard lock(old_state->mutex);
      if (old_state->device_path.empty() || old_state->requested_buffers == 0) {
        throw std::runtime_error("HDMI RX recovery has no reopen parameters");
      }
      if (std::any_of(old_state->buffers.begin(), old_state->buffers.end(), [](const detail::buffer_t &buffer) {
            return !buffer.queued;
          })) {
        return false;
      }
      device = old_state->device_path;
      requested_buffers = old_state->requested_buffers;
    }

    // No frame lease remains, so no stale DMA-BUF can be requeued into the
    // replacement queue. open() re-queries both DV timings and G_FMT.
    old_state->cleanup_noexcept();
    auto replacement = open(device, requested_buffers);
    state_ = std::move(replacement.state_);
    return true;
  }

  int hdmirx_capture_t::fd() const { if (!state_) throw std::runtime_error("HDMI RX capture is not open"); return state_->video_fd; }

  void hdmirx_capture_t::shutdown() noexcept {
    if (state_) {
      state_->shutdown_noexcept();
    }
  }

  const capture_format_t &hdmirx_capture_t::format() const {
    if (!state_) {
      throw std::runtime_error("HDMI RX capture is not open");
    }
    return state_->format;
  }

  const std::optional<v4l2_dv_timings> &hdmirx_capture_t::timings() const {
    if (!state_) {
      throw std::runtime_error("HDMI RX capture is not open");
    }
    return state_->timings;
  }

  const device_info_t &hdmirx_capture_t::device_info() const {
    if (!state_) {
      throw std::runtime_error("HDMI RX capture is not open");
    }
    return state_->device_info;
  }

  std::uint32_t hdmirx_capture_t::buffer_count() const {
    if (!state_) {
      throw std::runtime_error("HDMI RX capture is not open");
    }
    return static_cast<std::uint32_t>(state_->buffers.size());
  }
}  // namespace platf::hdmirx


namespace platf {
namespace {

class real_ioctl_backend_t final: public platf::edid::ioctl_backend_t {
    int fd_;
public:
    explicit real_ioctl_backend_t(int fd) : fd_(fd) {}
    void reset_fd(int fd) noexcept { fd_ = fd; }
    platf::edid::edid_result_t<std::uint32_t> get_edid(std::uint32_t pad, std::uint32_t start_block, std::uint32_t block_count, std::span<std::uint8_t> buffer) override {
        struct v4l2_edid edid = {};
        edid.pad = pad; edid.start_block = start_block; edid.blocks = block_count; edid.edid = buffer.data();
        if (ioctl(fd_, VIDIOC_G_EDID, &edid) < 0) return std::unexpected(platf::edid::edid_error_t{platf::edid::classify_errno(errno), errno, "VIDIOC_G_EDID failed"});
        return edid.blocks;
    }
    platf::edid::edid_result_t<std::uint32_t> set_edid(std::uint32_t pad, std::uint32_t start_block, std::uint32_t block_count, std::span<const std::uint8_t> data) override {
        struct v4l2_edid edid = {};
        edid.pad = pad; edid.start_block = start_block; edid.blocks = block_count; edid.edid = const_cast<std::uint8_t*>(data.data());
        if (ioctl(fd_, VIDIOC_S_EDID, &edid) < 0) return std::unexpected(platf::edid::edid_error_t{platf::edid::classify_errno(errno), errno, "VIDIOC_S_EDID failed"});
        return edid.blocks;
    }
    platf::edid::edid_result_t<void> set_audio_enabled(bool enabled) override {
        int state = enabled ? 1 : 0;
        constexpr auto k_set_audio_state = _IOW('V', BASE_VIDIOC_PRIVATE + 5, int);
        if (ioctl(fd_, k_set_audio_state, &state) < 0) return std::unexpected(platf::edid::edid_error_t{platf::edid::classify_errno(errno), errno, "RK_HDMIRX_CMD_SET_AUDIO_STATE failed"});
        return {};
    }
    platf::edid::edid_result_t<void> reset_hdmi_link() override {
        int unused = 0;
        constexpr auto k_soft_reset = _IO('V', BASE_VIDIOC_PRIVATE + 6);
        if (ioctl(fd_, k_soft_reset, &unused) < 0) return std::unexpected(platf::edid::edid_error_t{platf::edid::classify_errno(errno), errno, "RK_HDMIRX_CMD_SOFT_RESET failed"});
        return {};
    }
};

/**
 * @brief Translate one validated HDMI RX format into RKMPP's generic input layout.
 *
 * @param format HDMI RX format returned by VIDIOC_G_FMT.
 * @return Generic direct-DMA-BUF layout.
 * @throws std::runtime_error When the RX format cannot describe one MPP buffer.
 */
rkmpp::input_layout_t make_rkmpp_input_layout(const hdmirx::capture_format_t &format) {
  if (format.planes.size() != 1 || format.planes.front().bytesperline == 0 || format.planes.front().sizeimage == 0) {
    throw std::runtime_error("RKMPP direct input requires one HDMI RX plane with byte stride and allocation metadata");
  }
  const auto &plane = format.planes.front();
  const auto layout = rkmpp::make_input_layout_from_plane(format.width, format.height, format.mpp_format, plane.bytesperline, plane.sizeimage);
  if (!layout.has_value()) {
    throw std::runtime_error("HDMI RX has an unsupported or misaligned RKMPP DMA-BUF layout");
  }
  return *layout;
}

class hdmirx_encode_device_t final: public rkmpp_encode_device_t {
public:
  explicit hdmirx_encode_device_t(const hdmirx::capture_format_t &format): input_layout_(make_rkmpp_input_layout(format)) {}
  int convert(img_t &) override { return 0; }
  const void *input_layout() const override { return &input_layout_; }
private:
  rkmpp::input_layout_t input_layout_;
};

class hdmirx_display_t final: public display_t {
public:
  explicit hdmirx_display_t(const video::config_t &config):
      capture_(hdmirx::hdmirx_capture_t::open()),
      backend_(capture_.fd()),
      negotiator_(backend_, sm_, 0),
      moonlight_resolution_ {static_cast<uint32_t>(config.width), static_cast<uint32_t>(config.height)} {
    const auto &format = capture_.format();
    width = logical_width = env_width = env_logical_width = static_cast<int>(format.width);
    height = logical_height = env_height = env_logical_height = static_cast<int>(format.height);

    // read modes from EDID
    std::vector<platf::hdmirx::hdmi_mode_t> modes;
    auto orig_edid = platf::edid::read_edid(backend_, 0);
    if (orig_edid) {
        modes = platf::edid::parse_edid_modes(*orig_edid);
    }
    negotiator_.start_negotiation(moonlight_resolution_, modes);
  }

  ~hdmirx_display_t() {
      negotiator_.end_session();
  }

  capture_e capture(const push_captured_image_cb_t &push, const pull_free_image_cb_t &pull, bool *) override {
    try {
      while (true) {
        if (!settle_capture_state()) {
          std::shared_ptr<img_t> image;
          if (!pull(image) || !image) return capture_e::interrupted;
          auto rx_image = std::dynamic_pointer_cast<hdmirx::hdmirx_img_t>(image);
          if (!rx_image) return capture_e::error;
          // Never reuse a previously dequeued HDMI frame while the input is
          // negotiating or absent. The encode side will RGA-fill its own
          // target-sized DMA-BUF with deterministic NV12 green.
          rx_image->frame.reset();
          rx_image->capture_format.reset();
          rx_image->placeholder = true;
          rx_image->request_idr = sm_.consume_idr_request();
          rx_image->frame_timestamp = std::chrono::steady_clock::now();
          if (config::video.rkmpp_profile) {
            rx_image->frame_profile.emplace();
            rx_image->frame_profile->kind = video::frame_profile_kind_e::placeholder;
          } else {
            rx_image->frame_profile.reset();
          }
          if (!push(std::move(image), true)) return capture_e::ok;
          if (!sm_.wait_for_interval()) return capture_e::interrupted;
          continue;
        }
        std::shared_ptr<img_t> image;
        if (!pull(image) || !image) return capture_e::interrupted;
        std::optional<hdmirx::captured_frame_t> frame;
        try {
          frame.emplace(capture_.dequeue(std::chrono::seconds(2)));
        } catch (const hdmirx::source_change_error_t &error) {
          sm_.enter_source_change(error.what());
          recovery_pending_ = true;
          BOOST_LOG(info) << "HDMI RX source changed; waiting for frame leases before reopening";
          continue;
        } catch (const std::runtime_error &error) {
          if (std::string_view(error.what()) != "timed out waiting for an HDMI RX frame") {
            throw;
          }
          // A link loss can arrive without SOURCE_CHANGE. Treat it as a
          // recoverable no-signal transition and re-query on the next turn.
          sm_.enter_source_change(error.what());
          sm_.enter_no_signal(error.what());
          recovery_pending_ = true;
          continue;
        }
        auto rx_image = std::dynamic_pointer_cast<hdmirx::hdmirx_img_t>(image);
        if (!rx_image) return capture_e::error;
        rx_image->frame = std::make_shared<hdmirx::captured_frame_t>(std::move(*frame));
        rx_image->capture_format = capture_.format();
        rx_image->placeholder = false;
        rx_image->request_idr = sm_.consume_idr_request();
        rx_image->frame_timestamp = rx_image->frame->timestamp();
        if (config::video.rkmpp_profile) {
          rx_image->frame_profile.emplace();
          rx_image->frame_profile->kind = video::frame_profile_kind_e::captured;
          rx_image->frame_profile->capture_sequence = rx_image->frame->sequence();
          rx_image->frame_profile->capture_timestamp_flags = rx_image->frame->timestamp_flags();
          rx_image->frame_profile->hdmirx_width = rx_image->capture_format->width;
          rx_image->frame_profile->hdmirx_height = rx_image->capture_format->height;
          rx_image->frame_profile->moonlight_width = moonlight_resolution_.width;
          rx_image->frame_profile->moonlight_height = moonlight_resolution_.height;
          rx_image->frame_profile->capture = rx_image->frame->timestamp();
          rx_image->frame_profile->dequeued = rx_image->frame->dequeue_timestamp();
        } else {
          rx_image->frame_profile.reset();
        }
        if (!push(std::move(image), true)) return capture_e::ok;
      }
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "HDMI RX capture failed: " << e.what();
      return capture_e::reinit;
    }
  }
  std::shared_ptr<img_t> alloc_img() override { return std::make_shared<hdmirx::hdmirx_img_t>(); }
  int dummy_img(img_t *image) override {
    auto rx_image = dynamic_cast<hdmirx::hdmirx_img_t *>(image);
    if (!rx_image) return -1;
    try {
      if (!settle_capture_state()) {
        rx_image->frame.reset();
        rx_image->capture_format.reset();
        rx_image->placeholder = true;
        rx_image->request_idr = sm_.consume_idr_request();
        rx_image->frame_timestamp = std::chrono::steady_clock::now();
        if (config::video.rkmpp_profile) {
          rx_image->frame_profile.emplace();
          rx_image->frame_profile->kind = video::frame_profile_kind_e::placeholder;
        } else {
          rx_image->frame_profile.reset();
        }
        return 0;
      }
      // Probe with a real RX frame; this path never calls VIDIOC_S_FMT.
      rx_image->frame = std::make_shared<hdmirx::captured_frame_t>(capture_.dequeue(std::chrono::seconds(2)));
      rx_image->capture_format = capture_.format();
      rx_image->placeholder = false;
      rx_image->request_idr = sm_.consume_idr_request();
      rx_image->frame_timestamp = rx_image->frame->timestamp();
      if (config::video.rkmpp_profile) {
        rx_image->frame_profile.emplace();
        rx_image->frame_profile->kind = video::frame_profile_kind_e::captured;
        rx_image->frame_profile->capture_sequence = rx_image->frame->sequence();
        rx_image->frame_profile->capture_timestamp_flags = rx_image->frame->timestamp_flags();
        rx_image->frame_profile->hdmirx_width = rx_image->capture_format->width;
        rx_image->frame_profile->hdmirx_height = rx_image->capture_format->height;
        rx_image->frame_profile->moonlight_width = moonlight_resolution_.width;
        rx_image->frame_profile->moonlight_height = moonlight_resolution_.height;
        rx_image->frame_profile->capture = rx_image->frame->timestamp();
        rx_image->frame_profile->dequeued = rx_image->frame->dequeue_timestamp();
      } else {
        rx_image->frame_profile.reset();
      }
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "HDMI RX probe frame failed: " << e.what();
      return -1;
    }
  }
  std::unique_ptr<rkmpp_encode_device_t> make_rkmpp_encode_device() override {
    return std::make_unique<hdmirx_encode_device_t>(capture_.format());
  }
  bool is_codec_supported(std::string_view name, const video::config_t &config) override {
    return (name == "h264_rkmpp" || name == "hevc_rkmpp") && config.videoFormat <= 1 &&
           !config.dynamicRange && config.chromaSamplingType == 0;
  }
private:
  std::optional<hdmirx::resolution_t> actual_input_resolution() const {
    const auto &timings = capture_.timings();
    if (!timings || timings->type != V4L2_DV_BT_656_1120 ||
        timings->bt.width == 0 || timings->bt.height == 0) {
      return std::nullopt;
    }
    return hdmirx::resolution_t {timings->bt.width, timings->bt.height};
  }

  bool settle_capture_state() {
    if (recovery_pending_) {
      if (!capture_.recover_after_source_change()) {
        return false;
      }
      backend_.reset_fd(capture_.fd());
      recovery_pending_ = false;
    }

    const auto actual = actual_input_resolution();
    if (!actual) {
      sm_.enter_no_signal("HDMI RX has no valid DV timings");
      if (capture_.refresh_timings()) {
        // A fresh timing may have a new queue layout; rebuild before use.
        recovery_pending_ = true;
      }
      return false;
    }

    switch (sm_.state()) {
      case input_sm::state_e::starting:
      case input_sm::state_e::no_signal:
      case input_sm::state_e::source_change:
        sm_.enter_negotiating("HDMI RX timings observed");
        break;
      default:
        break;
    }
    if (sm_.state() == input_sm::state_e::negotiating) {
      return negotiator_.check_lock(actual);
    }
    return input_sm::is_streaming_state(sm_.state());
  }

  hdmirx::hdmirx_capture_t capture_;
  real_ioctl_backend_t backend_;
  platf::input_sm::state_machine_t sm_;
  platf::hdmirx::session_negotiator_t negotiator_;
  hdmirx::resolution_t moonlight_resolution_;  ///< Moonlight-requested coded resolution for the active session.
  bool recovery_pending_ {};
};

}  // namespace
std::shared_ptr<display_t> hdmirx_display(const video::config_t &config) {
  try { return std::make_shared<hdmirx_display_t>(config); }
  catch (const std::exception &e) { BOOST_LOG(error) << "Unable to open HDMI RX capture: " << e.what(); return nullptr; }
}
}  // namespace platf
