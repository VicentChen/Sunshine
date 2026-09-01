/**
 * @file src/platform/linux/rga.h
 * @brief Minimal synchronous librga DMA-BUF wrapper for the RKMPP pipeline.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace platf::rga {
  /**
   * @brief Pixel layouts accepted by the RGA wrapper.
   */
  enum class pixel_format_e {
    rgba8888,  ///< Packed RGBA, eight bits per component.
    bgr888,  ///< Packed BGR, eight bits per component.
    nv24,  ///< Semi-planar YUV 4:4:4.
    nv16,  ///< Semi-planar YUV 4:2:2.
    nv12,  ///< Semi-planar YUV 4:2:0.
  };

  /**
   * @brief Explicit color conversion requested for a process operation.
   */
  enum class color_space_e {
    default_,  ///< Let librga select its default conversion.
    rgb_to_yuv_bt709_limited,  ///< Convert RGB to Rec.709 limited-range YUV.
  };

  /**
   * @brief A rectangular region inside an image.
   */
  struct rectangle_t {
    std::uint32_t left {};  ///< Horizontal offset in pixels.
    std::uint32_t top {};  ///< Vertical offset in pixels.
    std::uint32_t width {};  ///< Rectangle width in pixels.
    std::uint32_t height {};  ///< Rectangle height in pixels.
  };

  /**
   * @brief DMA-BUF image metadata used for validation and RGA wrapping.
   *
   * `stride` is expressed in bytes.  The allocation must include every row
   * and chroma plane implied by `format`; no pixel memory is mapped by this
   * wrapper.
   */
  struct image_layout_t {
    int dma_buf_fd {-1};  ///< Owned or borrowed DMA-BUF file descriptor.
    std::uint32_t width {};  ///< Visible width in pixels.
    std::uint32_t height {};  ///< Visible height in pixels.
    std::uint32_t stride {};  ///< Bytes between consecutive luma or packed rows.
    std::uint64_t allocation_size {};  ///< DMA-BUF allocation size in bytes.
    pixel_format_e format {pixel_format_e::nv12};  ///< Pixel layout.
  };

  /**
   * @brief Native RGA operation outcome.
   */
  struct status_t {
    bool succeeded {};  ///< Normalized operation result independent of native numeric success values.
    int code {};  ///< Native `IM_STATUS` value preserved for diagnostics.
    std::string message;  ///< Native error text, normally from `imStrError()`.

    /**
     * @brief Determine whether this outcome represents success.
     *
     * @return true when the operation met its API-specific success criterion.
     */
    explicit operator bool() const noexcept;
  };

  /**
   * @brief Exception raised for a failed librga operation.
   */
  class error_t : public std::runtime_error {
  public:
    /**
     * @brief Construct an operation error.
     *
     * @param operation Operation that failed.
     * @param status Native status and error text.
     */
    error_t(const char *operation, const status_t &status);
  };

  /**
   * @brief Abstract librga boundary used by RAII tests and the production backend.
   *
   * An implementation must not retain the wrapper objects passed to its calls.
   * The production backend translates each operation to synchronous librga
   * calls; tests may use a deterministic fake without RGA hardware.
   */
  class backend_t {
  public:
    /** @brief Destroy a backend. */
    virtual ~backend_t() = default;

    /**
     * @brief Import one DMA-BUF into librga.
     *
     * @param layout Validated DMA-BUF image metadata.
     * @param handle Receives the nonzero native RGA handle on success.
     * @return Native status and error text.
     */
    virtual status_t import_dma_buf(const image_layout_t &layout, std::uintptr_t &handle) = 0;

    /**
     * @brief Release a previously imported RGA handle.
     *
     * @param handle Native handle returned by `import_dma_buf`.
     * @return Native status and error text.  Destruction logs a failed status
     * without throwing.
     */
    virtual status_t release_dma_buf(std::uintptr_t handle) = 0;

    /**
     * @brief Validate the concrete full-frame fill job before executing it.
     *
     * @param destination Imported destination image.
     * @param destination_layout Destination image metadata.
     * @param rectangle Destination rectangle.
     * @return Native status and error text.
     */
    virtual status_t check_fill(std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &rectangle) = 0;

    /**
     * @brief Execute a synchronous hardware fill.
     *
     * @param destination Imported destination image.
     * @param destination_layout Destination image metadata.
     * @param rectangle Destination rectangle.
     * @param color Packed RGA color value.
     * @return Native status and error text.
     */
    virtual status_t fill(std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &rectangle, std::uint32_t color) = 0;

    /**
     * @brief Validate the concrete full-frame resize job before executing it.
     *
     * @param source Imported source image.
     * @param source_layout Source image metadata.
     * @param destination Imported destination image.
     * @param destination_layout Destination image metadata.
     * @return Native status and error text.
     */
    virtual status_t check_process(std::uintptr_t source, const image_layout_t &source_layout, const rectangle_t &source_rect, std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &destination_rect, color_space_e color_space) = 0;

    virtual status_t process(std::uintptr_t source, const image_layout_t &source_layout, const rectangle_t &source_rect, std::uintptr_t destination, const image_layout_t &destination_layout, const rectangle_t &destination_rect, color_space_e color_space) = 0;

    virtual status_t check_resize(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) = 0;

    /**
     * @brief Execute a synchronous hardware resize.
     *
     * @param source Imported source image.
     * @param source_layout Source image metadata.
     * @param destination Imported destination image.
     * @param destination_layout Destination image metadata.
     * @return Native status and error text.
     */
    virtual status_t resize(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) = 0;

    /**
     * @brief Validate the concrete full-frame color-conversion job before execution.
     *
     * @param source Imported source image.
     * @param source_layout Source image metadata.
     * @param destination Imported destination image.
     * @param destination_layout Destination image metadata.
     * @return Native status and error text.
     */
    virtual status_t check_color_convert(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) = 0;

    /**
     * @brief Execute a synchronous hardware color conversion.
     *
     * @param source Imported source image.
     * @param source_layout Source image metadata.
     * @param destination Imported destination image.
     * @param destination_layout Destination image metadata.
     * @return Native status and error text.
     */
    virtual status_t color_convert(std::uintptr_t source, const image_layout_t &source_layout, std::uintptr_t destination, const image_layout_t &destination_layout) = 0;
  };

  /**
   * @brief Abstract DMA-BUF allocation source for RGA output images.
   */
  class dma_allocator_t {
  public:
    /** @brief Destroy an allocator. */
    virtual ~dma_allocator_t() = default;

    /**
     * @brief Allocate an owned DMA-BUF.
     *
     * @param size Requested allocation size in bytes.
     * @return A nonnegative owned DMA-BUF descriptor.
     * @throws std::runtime_error When allocation fails.
     */
    virtual int allocate(std::uint64_t size) = 0;

    /**
     * @brief Close an owned DMA-BUF descriptor.
     *
     * @param dma_buf_fd Descriptor returned by `allocate`.
     */
    virtual void close(int dma_buf_fd) noexcept = 0;
  };

  /**
   * @brief Import-only RAII owner for one RGA DMA-BUF handle.
   */
  class imported_buffer_t {
  public:
    /** @brief Construct an empty imported-buffer owner. */
    imported_buffer_t() = default;
    /** @brief Imported handles have unique ownership. */
    imported_buffer_t(const imported_buffer_t &) = delete;
    /** @brief Imported handles have unique ownership. */
    imported_buffer_t &operator=(const imported_buffer_t &) = delete;
    /** @brief Transfer handle ownership. */
    imported_buffer_t(imported_buffer_t &&other) noexcept;
    /** @brief Transfer handle ownership after releasing any current handle. */
    imported_buffer_t &operator=(imported_buffer_t &&other) noexcept;
    /** @brief Release the imported RGA handle before object destruction. */
    ~imported_buffer_t();

    /**
     * @brief Validate and import a borrowed DMA-BUF.
     *
     * The caller retains ownership of `layout.dma_buf_fd` and must keep it open
     * until this object is destroyed or moved away. `backend` must outlive this
     * object and every object created by moving it.
     *
     * @param backend Backend that owns the native RGA handle.
     * @param layout Borrowed DMA-BUF metadata.
     * @return An owning imported RGA handle.
     * @throws error_t When validation or librga import fails.
     */
    static imported_buffer_t import(backend_t &backend, image_layout_t layout);

    /**
     * @brief Access the validated image metadata.
     *
     * @return Borrowed metadata valid for this object's lifetime.
     */
    const image_layout_t &layout() const noexcept;

    /**
     * @brief Determine whether this object owns an imported RGA handle.
     *
     * @return true when a handle is owned.
     */
    explicit operator bool() const noexcept;

  private:
    /**
     * @brief Construct a successfully imported buffer.
     *
     * @param backend Native-handle backend.
     * @param layout Validated layout.
     * @param handle Imported native handle.
     */
    imported_buffer_t(backend_t *backend, image_layout_t layout, std::uintptr_t handle) noexcept;

    void reset() noexcept;  ///< Release the handle while its DMA-BUF remains open.

    backend_t *backend_ {};  ///< Backend used to release the native handle.
    image_layout_t layout_ {};  ///< Validated DMA-BUF metadata.
    std::uintptr_t handle_ {};  ///< Owned native RGA handle.

    friend class target_buffer_t;
    friend void fill(imported_buffer_t &, const rectangle_t &, std::uint32_t);
    friend void resize(const imported_buffer_t &, imported_buffer_t &);
    friend void color_convert(const imported_buffer_t &, imported_buffer_t &);
    friend void process(const imported_buffer_t &, const rectangle_t &, imported_buffer_t &, const rectangle_t &, color_space_e);
  };

  /**
   * @brief RAII owner for an RGA-writable and MPP-importable output DMA-BUF.
   *
   * The selected production allocator is `/dev/dma_heap/cma`.  It creates a
   * DMA-BUF without mapping pixels, and the existing RKMPP EXT_DMA import path
   * accepts the resulting descriptor.  The imported RGA handle is always
   * released before this class closes the descriptor.
   */
  class target_buffer_t {
  public:
    /** @brief Construct an empty target owner. */
    target_buffer_t() = default;
    /** @brief Target buffers have unique DMA-BUF ownership. */
    target_buffer_t(const target_buffer_t &) = delete;
    /** @brief Target buffers have unique DMA-BUF ownership. */
    target_buffer_t &operator=(const target_buffer_t &) = delete;
    /** @brief Transfer target DMA-BUF and RGA-handle ownership. */
    target_buffer_t(target_buffer_t &&other) noexcept;
    /** @brief Transfer target DMA-BUF and RGA-handle ownership. */
    target_buffer_t &operator=(target_buffer_t &&other) noexcept;
    /** @brief Release RGA state, then close the owned DMA-BUF. */
    ~target_buffer_t();

    /**
     * @brief Allocate and import an NV12 target DMA-BUF.
     *
     * @param backend Backend that will import the DMA-BUF.
     * @param allocator DMA-BUF allocator that retains descriptor ownership.
     * @param width Visible even width in pixels.
     * @param height Visible even height in pixels.
     * @param stride Byte stride, or zero for the minimum 64-byte-aligned stride.
     * @return An owning target buffer.
     * Both `backend` and `allocator` must outlive the returned object.
     *
     * @throws error_t When allocation, validation, or RGA import fails.
     */
    static target_buffer_t allocate_nv12(backend_t &backend, dma_allocator_t &allocator, std::uint32_t width, std::uint32_t height, std::uint32_t stride = 0);

    /**
     * @brief Allocate and import a packed RGBA8888 DMA-BUF.
     *
     * This is the external-allocation direction required by the Mali Vulkan
     * driver: a later Vulkan backend may import and write the same DMA-BUF,
     * while Gate 4 can populate it once with RGA.
     *
     * @param backend Backend that will import the DMA-BUF.
     * @param allocator DMA-BUF allocator that retains descriptor ownership.
     * @param width Visible width in pixels.
     * @param height Visible height in pixels.
     * @param stride Byte stride, or zero for tightly packed RGBA rows.
     * @return An owning RGBA target buffer.
     */
    static target_buffer_t allocate_rgba8888(backend_t &backend, dma_allocator_t &allocator, std::uint32_t width, std::uint32_t height, std::uint32_t stride = 0);

    /**
     * @brief Allocate and import a packed BGR888 DMA-BUF.
     *
     * @param backend Backend that will import the DMA-BUF.
     * @param allocator DMA-BUF allocator that retains descriptor ownership.
     * @param width Visible width in pixels.
     * @param height Visible height in pixels.
     * @param stride Byte stride, or zero for tightly packed BGR rows.
     * @return An owning BGR target buffer.
     */
    static target_buffer_t allocate_bgr888(backend_t &backend, dma_allocator_t &allocator, std::uint32_t width, std::uint32_t height, std::uint32_t stride = 0);

    /**
     * @brief Access the imported target buffer.
     *
     * @return Imported RGA buffer whose DMA-BUF remains owned by this object.
     */
    imported_buffer_t &rga_buffer() noexcept;

    /**
     * @brief Access the target image metadata.
     *
     * @return Metadata for the owned DMA-BUF.
     */
    const image_layout_t &layout() const noexcept;

  private:
    /**
     * @brief Construct an allocated target owner.
     *
     * @param allocator Allocator that owns descriptor close semantics.
     * @param dma_buf_fd Owned descriptor.
     * @param buffer Imported RGA buffer.
     */
    target_buffer_t(dma_allocator_t *allocator, int dma_buf_fd, imported_buffer_t buffer) noexcept;

    void reset() noexcept;  ///< Release RGA handle before closing the descriptor.

    dma_allocator_t *allocator_ {};  ///< Allocator used to close `dma_buf_fd_`.
    int dma_buf_fd_ {-1};  ///< Owned target DMA-BUF descriptor.
    imported_buffer_t buffer_ {};  ///< Imported RGA handle released before the descriptor.
  };

  /**
   * @brief Validate a V4L2 fourcc for an RGA source image.
   *
   * This mapping deliberately states no runtime support guarantee: callers
   * must still use a concrete `imcheck()` through `resize` or `color_convert`.
   *
   * @param fourcc V4L2 pixel format value.
   * @return Matching wrapper format, or `std::nullopt` when unsupported.
   */
  std::optional<pixel_format_e> format_from_v4l2_fourcc(std::uint32_t fourcc) noexcept;

  /**
   * @brief Determine whether librga was compiled into this binary.
   *
   * @return true only when CMake verified the required im2d API.
   */
  bool is_compiled() noexcept;

  /**
   * @brief Construct the production synchronous librga backend.
   *
   * @return A production backend when librga was compiled in.
   * @throws std::runtime_error When librga support is disabled at build time.
   */
  std::unique_ptr<backend_t> make_backend();

  /**
   * @brief Construct the production CMA DMA-HEAP allocator.
   *
   * @return An allocator for `/dev/dma_heap/cma`.
   */
  std::unique_ptr<dma_allocator_t> make_cma_dma_allocator();

  /**
   * @brief Validate and synchronously fill a destination rectangle using RGA.
   *
   * @param destination Imported destination buffer.
   * @param rectangle Rectangle to fill.
   * @param color Packed RGA color value.
   * @throws error_t When validation, `imcheck()`, or `imfill()` fails.
   */
  void fill(imported_buffer_t &destination, const rectangle_t &rectangle, std::uint32_t color);

  /**
   * @brief Validate and synchronously resize one complete image using RGA.
   *
   * @param source Imported source buffer.
   * @param destination Imported destination buffer.
   * @throws error_t When validation, `imcheck()`, or `imresize()` fails.
   */
  void process(const imported_buffer_t &source, const rectangle_t &source_rect, imported_buffer_t &destination, const rectangle_t &destination_rect, color_space_e color_space = color_space_e::default_);

  void resize(const imported_buffer_t &source, imported_buffer_t &destination);

  /**
   * @brief Validate and synchronously convert one complete image using RGA.
   *
   * @param source Imported source buffer.
   * @param destination Imported destination buffer.
   * @throws error_t When validation, `imcheck()`, or `imcvtcolor()` fails.
   */
  void color_convert(const imported_buffer_t &source, imported_buffer_t &destination);
}  // namespace platf::rga
