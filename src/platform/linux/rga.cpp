/**
 * @file src/platform/linux/rga.cpp
 * @brief Synchronous librga DMA-BUF wrapper implementation.
 */
#include "src/platform/linux/rga.h"

#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <exception>
#include <limits>
#include <system_error>
#include <utility>

#if defined(SUNSHINE_BUILD_RGA)
  #if defined(SUNSHINE_LIBRGA_IM2D_HEADER_RGA)
    #include <rga/im2d.h>
  #else
    #include <im2d.h>
  #endif
#endif

namespace platf::rga {
  namespace {
    constexpr std::uint32_t k_nv12_stride_alignment = 64;

    /**
     * @brief Determine a format's packed bytes per pixel.
     *
     * @param format Wrapper pixel format.
     * @return Four for RGBA8888, three for BGR888, or one for semi-planar YUV formats.
     */
    std::uint32_t bytes_per_luma_pixel(pixel_format_e format) noexcept {
      switch (format) {
        case pixel_format_e::rgba8888:
          return 4U;
        case pixel_format_e::bgr888:
          return 3U;
        default:
          return 1U;
      }
    }

    /**
     * @brief Determine the number of allocation rows used by a format.
     *
     * @param format Wrapper pixel format.
     * @param height Visible height.
     * @return Total packed or luma-plus-chroma allocation rows.
     */
    std::uint64_t allocation_rows(pixel_format_e format, std::uint32_t height) noexcept {
      switch (format) {
        case pixel_format_e::rgba8888:
          return height;
        case pixel_format_e::nv12:
          return static_cast<std::uint64_t>(height) + height / 2U;
        case pixel_format_e::nv16:
          return static_cast<std::uint64_t>(height) * 2U;
        case pixel_format_e::nv24:
          return static_cast<std::uint64_t>(height) * 3U;
        case pixel_format_e::bgr888:
          return height;
      }
      return 0;
    }

    /**
     * @brief Check that a value is a nonzero power of two.
     *
     * @param value Candidate alignment.
     * @return true when `value` is a power of two.
     */
    bool is_power_of_two(std::uint32_t value) noexcept {
      return value != 0 && (value & (value - 1U)) == 0;
    }

    /**
     * @brief Round a value up to a power-of-two alignment.
     *
     * @param value Value to align.
     * @param alignment Required power-of-two alignment.
     * @return Aligned value, or zero on overflow or invalid alignment.
     */
    std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) noexcept {
      if (!is_power_of_two(alignment)) {
        return 0;
      }
      const auto padding = (alignment - (value & (alignment - 1U))) & (alignment - 1U);
      if (value > std::numeric_limits<std::uint32_t>::max() - padding) {
        return 0;
      }
      return value + padding;
    }

    /**
     * @brief Build a deterministic validation error.
     *
     * @param message Error detail.
     * @return Failed wrapper status.
     */
    status_t invalid_status(const char *message) {
      return {false, -1, message};
    }

    /**
     * @brief Build a normalized wrapper success status.
     *
     * @return Successful status for validation or fake-independent bookkeeping.
     */
    status_t success_status() {
      return {true, 0, ""};
    }

    /**
     * @brief Validate DMA-BUF metadata before a native import.
     *
     * @param layout Metadata to validate.
     * @return Success or a descriptive failed status.
     */
    status_t validate_layout(const image_layout_t &layout) {
      if (layout.dma_buf_fd < 0) {
        return invalid_status("DMA-BUF file descriptor is invalid");
      }
      if (layout.width == 0 || layout.height == 0 || layout.stride == 0 || layout.allocation_size == 0) {
        return invalid_status("DMA-BUF dimensions, stride, and allocation size must be nonzero");
      }
      if (layout.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || layout.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || layout.stride > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return invalid_status("DMA-BUF dimensions or stride exceed librga int parameter capacity");
      }
      if ((layout.format == pixel_format_e::nv12 || layout.format == pixel_format_e::nv16) && (layout.width & 1U) != 0) {
        return invalid_status("subsampled YUV width must be even");
      }
      if (layout.format == pixel_format_e::nv12 && (layout.height & 1U) != 0) {
        return invalid_status("NV12 height must be even");
      }
      const auto minimum_stride = static_cast<std::uint64_t>(layout.width) * bytes_per_luma_pixel(layout.format);
      if (layout.stride < minimum_stride) {
        return invalid_status("DMA-BUF stride is smaller than the visible row");
      }
      if ((layout.format == pixel_format_e::rgba8888 && layout.stride % 4U != 0) || (layout.format == pixel_format_e::bgr888 && layout.stride % 3U != 0)) {
        return invalid_status("packed RGB DMA-BUF stride must contain whole pixels");
      }
      const auto rows = allocation_rows(layout.format, layout.height);
      if (rows == 0 || rows > std::numeric_limits<std::uint64_t>::max() / layout.stride) {
        return invalid_status("DMA-BUF allocation arithmetic overflow");
      }
      if (layout.allocation_size < rows * layout.stride) {
        return invalid_status("DMA-BUF allocation is too small for its image layout");
      }
      if (layout.allocation_size > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return invalid_status("DMA-BUF allocation exceeds librga importbuffer_fd(int size) capacity");
      }
      return success_status();
    }

    /**
     * @brief Validate that a rectangle lies within one visible image.
     *
     * @param layout Image metadata.
     * @param rectangle Rectangle to validate.
     * @return Success or a descriptive failed status.
     */
    status_t validate_rectangle(const image_layout_t &layout, const rectangle_t &rectangle) {
      if (rectangle.width == 0 || rectangle.height == 0) {
        return invalid_status("RGA rectangle dimensions must be nonzero");
      }
      const auto right = static_cast<std::uint64_t>(rectangle.left) + rectangle.width;
      const auto bottom = static_cast<std::uint64_t>(rectangle.top) + rectangle.height;
      if (right > layout.width || bottom > layout.height) {
        return invalid_status("RGA rectangle exceeds visible image bounds");
      }
      if (layout.format == pixel_format_e::nv12 && ((rectangle.left | rectangle.top | rectangle.width | rectangle.height) & 1U) != 0) {
        return invalid_status("NV12 RGA rectangles require even offsets and dimensions");
      }
      if (layout.format == pixel_format_e::nv16 && ((rectangle.left | rectangle.width) & 1U) != 0) {
        return invalid_status("NV16 RGA rectangles require even horizontal alignment");
      }
      return success_status();
    }

    /**
     * @brief Throw a typed error for a failed wrapper status.
     *
     * @param operation Operation name.
     * @param status Failed operation status.
     */
    void require_success(const char *operation, const status_t &status) {
      if (!status) {
        throw error_t(operation, status);
      }
    }

    /**
     * @brief Report a release failure without violating noexcept destruction.
     *
     * @param status Failed native release status.
     */
    void report_release_failure(const status_t &status) noexcept {
      if (!status) {
        std::fprintf(stderr, "rga: releasebuffer_handle failed (RGA status=%d): %s\n", status.code, status.message.empty() ? "unknown librga error" : status.message.c_str());
      }
    }

    /**
     * @brief Report an exception raised while releasing a handle during destruction.
     *
     * @param exception Exception reported by the backend.
     */
    void report_release_exception(const std::exception &exception) noexcept {
      std::fprintf(stderr, "rga: releasebuffer_handle threw during cleanup: %s\n", exception.what());
    }

    /**
     * @brief Report an unknown failure raised while releasing a handle during destruction.
     */
    void report_unknown_release_exception() noexcept {
      std::fputs("rga: releasebuffer_handle threw an unknown exception during cleanup\n", stderr);
    }

    /**
     * @brief Close one descriptor without allowing exceptions to escape.
     *
     * @param dma_buf_fd Descriptor to close.
     */
    void close_noexcept(int dma_buf_fd) noexcept {
      if (dma_buf_fd >= 0) {
        (void) ::close(dma_buf_fd);
      }
    }

    /**
     * @brief Allocate physically contiguous DMA-BUFs from the Rockchip CMA heap.
     */
    class cma_dma_allocator_t final : public dma_allocator_t {
    public:
      /** @brief Construct a CMA DMA-HEAP allocator. */
      cma_dma_allocator_t() = default;

      int allocate(std::uint64_t size) override {
        if (size == 0 || size > std::numeric_limits<__u64>::max()) {
          throw std::runtime_error("CMA DMA-HEAP allocation size is invalid");
        }
        const auto heap_fd = ::open("/dev/dma_heap/cma", O_RDWR | O_CLOEXEC);
        if (heap_fd < 0) {
          throw std::system_error(errno, std::generic_category(), "failed to open /dev/dma_heap/cma");
        }
        dma_heap_allocation_data allocation {};
        allocation.len = size;
        allocation.fd_flags = O_CLOEXEC | O_RDWR;
        if (::ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) == -1) {
          const auto error = errno;
          close_noexcept(heap_fd);
          throw std::system_error(error, std::generic_category(), "DMA_HEAP_IOCTL_ALLOC(/dev/dma_heap/cma) failed");
        }
        close_noexcept(heap_fd);
        if (allocation.fd == std::numeric_limits<__u32>::max() || allocation.fd > static_cast<__u32>(std::numeric_limits<int>::max())) {
          // Linux file descriptors are represented as int. A value outside that
          // range cannot name a closable userspace descriptor, so it cannot be
          // safely passed to close() on this error path.
          throw std::runtime_error("DMA_HEAP_IOCTL_ALLOC returned a DMA-BUF descriptor outside int capacity");
        }
        return static_cast<int>(allocation.fd);
      }

      void close(int dma_buf_fd) noexcept override {
        close_noexcept(dma_buf_fd);
      }
    };

#if defined(SUNSHINE_BUILD_RGA)
    /**
     * @brief Convert wrapper format to the corresponding librga format.
     *
     * @param format Wrapper pixel format.
     * @return librga `RK_FORMAT_*` value.
     */
    int native_format(pixel_format_e format) noexcept {
      switch (format) {
        case pixel_format_e::rgba8888:
          return RK_FORMAT_RGBA_8888;
        case pixel_format_e::bgr888:
          return RK_FORMAT_BGR_888;
        case pixel_format_e::nv24:
          return RK_FORMAT_YCbCr_444_SP;
        case pixel_format_e::nv16:
          return RK_FORMAT_YCbCr_422_SP;
        case pixel_format_e::nv12:
          return RK_FORMAT_YCbCr_420_SP;
      }
      return RK_FORMAT_YCbCr_420_SP;
    }

    /** @brief Configure librga buffer color spaces without changing transform usage bits. */
    void configure_color_space(rga_buffer_t &source, rga_buffer_t &destination, color_space_e color_space) noexcept {
      if (color_space == color_space_e::rgb_to_yuv_bt709_limited) {
        imsetColorSpace(&source, IM_RGB_FULL_RANGE);
        imsetColorSpace(&destination, IM_YUV_BT709_LIMIT_RANGE);
      }
    }

    /**
     * @brief Convert wrapper metadata to a native RGA buffer.
     *
     * @param handle Imported native handle.
     * @param layout Validated image metadata.
     * @return Native RGA buffer descriptor.
     */
    rga_buffer_t native_buffer(std::uintptr_t handle, const image_layout_t &layout) {
      const auto stride_pixels = layout.stride / bytes_per_luma_pixel(layout.format);
      return wrapbuffer_handle(static_cast<rga_buffer_handle_t>(handle), layout.width, layout.height, native_format(layout.format), stride_pixels, layout.height);
    }

    /**
     * @brief Create a full-frame native rectangle.
     *
     * @param layout Image metadata.
     * @return Native full-frame rectangle.
     */
    im_rect full_rectangle(const image_layout_t &layout) noexcept {
      return {0, 0, static_cast<int>(layout.width), static_cast<int>(layout.height)};
    }

    /**
     * @brief Convert a wrapper rectangle to a native rectangle.
     *
     * @param rectangle Valid wrapper rectangle.
     * @return Native rectangle.
     */
    im_rect native_rectangle(const rectangle_t &rectangle) noexcept {
      return {static_cast<int>(rectangle.left), static_cast<int>(rectangle.top), static_cast<int>(rectangle.width), static_cast<int>(rectangle.height)};
    }

    /**
     * @brief Convert a librga result to a wrapper status with `imStrError()` text.
     *
     * @param result librga operation outcome.
     * @return Wrapper status.
     */
    status_t native_status(IM_STATUS result, IM_STATUS expected_success) {
      const char *message = imStrError(result);
      return {result == expected_success, static_cast<int>(result), message == nullptr ? "librga returned no error text" : message};
    }

    /**
     * @brief Production backend that performs synchronous librga operations.
     */
    class librga_backend_t final : public backend_t {
    public:
      status_t import_dma_buf(const image_layout_t &layout, std::uintptr_t &handle) override {
        const auto native_handle = importbuffer_fd(layout.dma_buf_fd, static_cast<int>(layout.allocation_size));
        if (native_handle == 0) {
          return native_status(IM_STATUS_FAILED, IM_STATUS_SUCCESS);
        }
        handle = static_cast<std::uintptr_t>(native_handle);
        return {true, static_cast<int>(IM_STATUS_SUCCESS), ""};
      }

      status_t release_dma_buf(std::uintptr_t handle) override {
        return native_status(releasebuffer_handle(static_cast<rga_buffer_handle_t>(handle)), IM_STATUS_SUCCESS);
      }

      status_t check_fill(std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &rectangle) override {
        const auto native_destination = native_buffer(destination, destination_layout);
        return native_status(imcheck(rga_buffer_t {}, native_destination, im_rect {}, native_rectangle(rectangle), IM_COLOR_FILL), IM_STATUS_NOERROR);
      }

      status_t fill(std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &rectangle, std::uint32_t color) override {
        return native_status(imfill(native_buffer(destination, destination_layout), native_rectangle(rectangle), color, IM_SYNC), IM_STATUS_SUCCESS);
      }

      status_t check_process(std::uintptr_t source, const image_layout_t &source_layout, const rectangle_t &source_rect, std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &destination_rect, color_space_e color_space) override {
        auto native_source = native_buffer(source, source_layout);
        auto native_destination = native_buffer(destination, destination_layout);
        configure_color_space(native_source, native_destination, color_space);
        return native_status(imcheck(native_source, native_destination, native_rectangle(source_rect), native_rectangle(destination_rect)), IM_STATUS_NOERROR);
      }

      status_t process(std::uintptr_t source, const image_layout_t &source_layout, const rectangle_t &source_rect, std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &destination_rect, color_space_e color_space) override {
        auto native_source = native_buffer(source, source_layout);
        auto native_destination = native_buffer(destination, destination_layout);
        configure_color_space(native_source, native_destination, color_space);
        return native_status(improcess(native_source, native_destination, {}, native_rectangle(source_rect), native_rectangle(destination_rect), {}, IM_SYNC), IM_STATUS_SUCCESS);
      }

      status_t check_resize(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) override {
        return native_status(imcheck(native_buffer(source, source_layout), native_buffer(destination, destination_layout), full_rectangle(source_layout), full_rectangle(destination_layout)), IM_STATUS_NOERROR);
      }

      status_t resize(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) override {
        return native_status(imresize(native_buffer(source, source_layout), native_buffer(destination, destination_layout), 0.0, 0.0, IM_INTERP_LINEAR, IM_SYNC), IM_STATUS_SUCCESS);
      }

      status_t check_color_convert(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) override {
        return native_status(imcheck(native_buffer(source, source_layout), native_buffer(destination, destination_layout), full_rectangle(source_layout), full_rectangle(destination_layout)), IM_STATUS_NOERROR);
      }

      status_t color_convert(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) override {
        return native_status(imcvtcolor(native_buffer(source, source_layout), native_buffer(destination, destination_layout), native_format(source_layout.format), native_format(destination_layout.format), IM_COLOR_SPACE_DEFAULT, IM_SYNC), IM_STATUS_SUCCESS);
      }
    };
#endif
  }  // namespace

  status_t::operator bool() const noexcept {
    return succeeded;
  }

  error_t::error_t(const char *operation, const status_t &status) :
      std::runtime_error(std::string(operation) + " failed (RGA status=" + std::to_string(status.code) + "): " + (status.message.empty() ? "unknown librga error" : status.message)) {}

  imported_buffer_t::imported_buffer_t(backend_t *backend, image_layout_t layout, std::uintptr_t handle) noexcept :
      backend_(backend), layout_(layout), handle_(handle) {}

  imported_buffer_t::imported_buffer_t(imported_buffer_t &&other) noexcept :
      backend_(std::exchange(other.backend_, nullptr)), layout_(other.layout_), handle_(std::exchange(other.handle_, 0)) {}

  imported_buffer_t &imported_buffer_t::operator=(imported_buffer_t &&other) noexcept {
    if (this != &other) {
      reset();
      backend_ = std::exchange(other.backend_, nullptr);
      layout_ = other.layout_;
      handle_ = std::exchange(other.handle_, 0);
    }
    return *this;
  }

  imported_buffer_t::~imported_buffer_t() {
    reset();
  }

  imported_buffer_t imported_buffer_t::import(backend_t &backend, image_layout_t layout) {
    require_success("RGA DMA-BUF layout validation", validate_layout(layout));
    std::uintptr_t handle {};
    require_success("importbuffer_fd", backend.import_dma_buf(layout, handle));
    if (handle == 0) {
      throw error_t("importbuffer_fd", invalid_status("backend returned a zero RGA handle"));
    }
    return imported_buffer_t(&backend, layout, handle);
  }

  const image_layout_t &imported_buffer_t::layout() const noexcept {
    return layout_;
  }

  imported_buffer_t::operator bool() const noexcept {
    return backend_ != nullptr && handle_ != 0;
  }

  void imported_buffer_t::reset() noexcept {
    if (backend_ != nullptr && handle_ != 0) {
      try {
        report_release_failure(backend_->release_dma_buf(handle_));
      } catch (const std::exception &exception) {
        report_release_exception(exception);
      } catch (...) {
        report_unknown_release_exception();
      }
    }
    backend_ = nullptr;
    handle_ = 0;
  }

  target_buffer_t::target_buffer_t(dma_allocator_t *allocator, int dma_buf_fd, imported_buffer_t buffer) noexcept :
      allocator_(allocator), dma_buf_fd_(dma_buf_fd), buffer_(std::move(buffer)) {}

  target_buffer_t::target_buffer_t(target_buffer_t &&other) noexcept :
      allocator_(std::exchange(other.allocator_, nullptr)), dma_buf_fd_(std::exchange(other.dma_buf_fd_, -1)), buffer_(std::move(other.buffer_)) {}

  target_buffer_t &target_buffer_t::operator=(target_buffer_t &&other) noexcept {
    if (this != &other) {
      reset();
      allocator_ = std::exchange(other.allocator_, nullptr);
      dma_buf_fd_ = std::exchange(other.dma_buf_fd_, -1);
      buffer_ = std::move(other.buffer_);
    }
    return *this;
  }

  target_buffer_t::~target_buffer_t() {
    reset();
  }

  target_buffer_t target_buffer_t::allocate_nv12(backend_t &backend, dma_allocator_t &allocator, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    if ((width & 1U) != 0 || (height & 1U) != 0 || width == 0 || height == 0) {
      throw error_t("NV12 target allocation", invalid_status("NV12 target dimensions must be positive and even"));
    }
    if (stride == 0) {
      stride = align_up(width, k_nv12_stride_alignment);
    }
    if (stride == 0 || stride < width || stride % k_nv12_stride_alignment != 0) {
      throw error_t("NV12 target allocation", invalid_status("NV12 target stride must be at least width and 64-byte aligned"));
    }
    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || stride > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      throw error_t("NV12 target allocation", invalid_status("NV12 target dimensions or stride exceed librga int parameter capacity"));
    }
    const auto allocation_rows = static_cast<std::uint64_t>(height) + height / 2U;
    if (static_cast<std::uint64_t>(stride) > std::numeric_limits<std::uint64_t>::max() / allocation_rows) {
      throw error_t("NV12 target allocation", invalid_status("NV12 target allocation arithmetic overflow"));
    }
    const auto allocation_size = static_cast<std::uint64_t>(stride) * allocation_rows;
    if (allocation_size > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      throw error_t("NV12 target allocation", invalid_status("NV12 target allocation exceeds librga importbuffer_fd(int size) capacity"));
    }
    const auto dma_buf_fd = allocator.allocate(allocation_size);
    if (dma_buf_fd < 0) {
      throw error_t("NV12 target allocation", invalid_status("allocator returned an invalid DMA-BUF descriptor"));
    }
    try {
      auto buffer = imported_buffer_t::import(backend, {dma_buf_fd, width, height, stride, allocation_size, pixel_format_e::nv12});
      return target_buffer_t(&allocator, dma_buf_fd, std::move(buffer));
    } catch (...) {
      allocator.close(dma_buf_fd);
      throw;
    }
  }

  target_buffer_t target_buffer_t::allocate_rgba8888(backend_t &backend, dma_allocator_t &allocator, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    if (width == 0 || height == 0 || width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      throw error_t("RGBA8888 target allocation", invalid_status("RGBA8888 target dimensions must be positive and fit librga int parameters"));
    }
    const auto minimum_stride = static_cast<std::uint64_t>(width) * 4U;
    if (minimum_stride > std::numeric_limits<std::uint32_t>::max()) {
      throw error_t("RGBA8888 target allocation", invalid_status("RGBA8888 target stride exceeds uint32 capacity"));
    }
    if (stride == 0) {
      stride = static_cast<std::uint32_t>(minimum_stride);
    }
    if (stride < minimum_stride || stride % 4U != 0 || stride > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      throw error_t("RGBA8888 target allocation", invalid_status("RGBA8888 target stride must hold complete four-byte pixels and fit librga int parameters"));
    }
    if (static_cast<std::uint64_t>(stride) > std::numeric_limits<std::uint64_t>::max() / height) {
      throw error_t("RGBA8888 target allocation", invalid_status("RGBA8888 target allocation arithmetic overflow"));
    }
    const auto allocation_size = static_cast<std::uint64_t>(stride) * height;
    if (allocation_size > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      throw error_t("RGBA8888 target allocation", invalid_status("RGBA8888 target allocation exceeds librga importbuffer_fd(int size) capacity"));
    }
    const auto dma_buf_fd = allocator.allocate(allocation_size);
    if (dma_buf_fd < 0) {
      throw error_t("RGBA8888 target allocation", invalid_status("allocator returned an invalid DMA-BUF descriptor"));
    }
    try {
      auto buffer = imported_buffer_t::import(backend, {dma_buf_fd, width, height, stride, allocation_size, pixel_format_e::rgba8888});
      return target_buffer_t(&allocator, dma_buf_fd, std::move(buffer));
    } catch (...) {
      allocator.close(dma_buf_fd);
      throw;
    }
  }

  imported_buffer_t &target_buffer_t::rga_buffer() noexcept {
    return buffer_;
  }

  const image_layout_t &target_buffer_t::layout() const noexcept {
    return buffer_.layout();
  }

  void target_buffer_t::reset() noexcept {
    buffer_ = {};
    if (allocator_ != nullptr && dma_buf_fd_ >= 0) {
      allocator_->close(dma_buf_fd_);
    }
    allocator_ = nullptr;
    dma_buf_fd_ = -1;
  }

  std::optional<pixel_format_e> format_from_v4l2_fourcc(std::uint32_t fourcc) noexcept {
    switch (fourcc) {
      case V4L2_PIX_FMT_BGR24:
        return pixel_format_e::bgr888;
      case V4L2_PIX_FMT_NV24:
        return pixel_format_e::nv24;
      case V4L2_PIX_FMT_NV16:
        return pixel_format_e::nv16;
      case V4L2_PIX_FMT_NV12:
        return pixel_format_e::nv12;
      default:
        return std::nullopt;
    }
  }

  bool is_compiled() noexcept {
#if defined(SUNSHINE_BUILD_RGA)
    return true;
#else
    return false;
#endif
  }

  std::unique_ptr<backend_t> make_backend() {
#if defined(SUNSHINE_BUILD_RGA)
    return std::make_unique<librga_backend_t>();
#else
    throw std::runtime_error("librga support is disabled because CMake did not find its headers, library, and required im2d API");
#endif
  }

  std::unique_ptr<dma_allocator_t> make_cma_dma_allocator() {
    return std::make_unique<cma_dma_allocator_t>();
  }

  void fill(imported_buffer_t &destination, const rectangle_t &rectangle, std::uint32_t color) {
    if (!destination) {
      throw error_t("imfill", invalid_status("destination RGA handle is empty"));
    }
    require_success("imfill rectangle validation", validate_rectangle(destination.layout_, rectangle));
    require_success("imcheck(fill)", destination.backend_->check_fill(destination.handle_, destination.layout_, rectangle));
    require_success("imfill", destination.backend_->fill(destination.handle_, destination.layout_, rectangle, color));
  }

  void process(const imported_buffer_t &source, const rectangle_t &source_rect, imported_buffer_t &destination, const rectangle_t &destination_rect, color_space_e color_space) {
    if (!source || !destination) {
      throw error_t("improcess", invalid_status("source or destination RGA handle is empty"));
    }
    if (source.backend_ != destination.backend_) {
      throw error_t("improcess", invalid_status("source and destination RGA handles belong to different backends"));
    }
    require_success("improcess source rectangle validation", validate_rectangle(source.layout_, source_rect));
    require_success("improcess destination rectangle validation", validate_rectangle(destination.layout_, destination_rect));
    require_success("imcheck(process)", source.backend_->check_process(source.handle_, source.layout_, source_rect, destination.handle_, destination.layout_, destination_rect, color_space));
    require_success("improcess", source.backend_->process(source.handle_, source.layout_, source_rect, destination.handle_, destination.layout_, destination_rect, color_space));
  }

  void resize(const imported_buffer_t &source, imported_buffer_t &destination) {
    if (!source || !destination) {
      throw error_t("imresize", invalid_status("source or destination RGA handle is empty"));
    }
    if (source.backend_ != destination.backend_) {
      throw error_t("imresize", invalid_status("source and destination RGA handles belong to different backends"));
    }
    require_success("imcheck(resize)", source.backend_->check_resize(source.handle_, source.layout_, destination.handle_, destination.layout_));
    require_success("imresize", source.backend_->resize(source.handle_, source.layout_, destination.handle_, destination.layout_));
  }

  void color_convert(const imported_buffer_t &source, imported_buffer_t &destination) {
    if (!source || !destination) {
      throw error_t("imcvtcolor", invalid_status("source or destination RGA handle is empty"));
    }
    if (source.backend_ != destination.backend_) {
      throw error_t("imcvtcolor", invalid_status("source and destination RGA handles belong to different backends"));
    }
    require_success("imcheck(color conversion)", source.backend_->check_color_convert(source.handle_, source.layout_, destination.handle_, destination.layout_));
    require_success("imcvtcolor", source.backend_->color_convert(source.handle_, source.layout_, destination.handle_, destination.layout_));
  }
}  // namespace platf::rga
