/**
 * @file src/platform/linux/vulkan_ui.h
 * @brief Opaque Vulkan UI rendering into an externally allocated DMA-BUF.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf::vulkan_ui {
  /** @brief Linear floating-point RGBA color used by the render model. */
  struct color_t {
    float red {};
    float green {};
    float blue {};
    float alpha {1.0F};
  };

  /** @brief One opaque rectangle in panel pixel coordinates. */
  struct rectangle_t {
    std::uint32_t left {};
    std::uint32_t top {};
    std::uint32_t width {};
    std::uint32_t height {};
    color_t color;
  };

  /**
   * @brief Renderer-independent UI snapshot.
   *
   * A revision identifies the complete immutable snapshot. Reusing the same
   * revision must not submit another Vulkan render.
   */
  struct render_model_t {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint64_t revision {};
    color_t background;
    std::vector<rectangle_t> rectangles;
  };

  /**
   * @brief Validate bounds and opacity before submitting a model to Vulkan.
   *
   * @param model Model to validate.
   * @return Empty on success, otherwise a stable diagnostic string.
   */
  std::optional<std::string> validate_render_model(const render_model_t &model);

  /**
   * @brief Build the horizontal static Vulkan UI bar used by the stage 5 gate.
   *
   * @param width Panel width in pixels.
   * @param height Panel height in pixels.
   * @param revision Revision assigned to the resulting immutable snapshot.
   * @return Opaque background, menu rows, focus highlight, and bitmap text.
   */
  render_model_t make_gate5_static_model(std::uint32_t width, std::uint32_t height, std::uint64_t revision = 1);

  /** @brief Long-lived Vulkan renderer bound to one external RGBA DMA-BUF. */
  class renderer_t {
  public:
    /** @brief Renderers own unique Vulkan and imported-memory state. */
    renderer_t(const renderer_t &) = delete;
    /** @brief Renderers own unique Vulkan and imported-memory state. */
    renderer_t &operator=(const renderer_t &) = delete;
    /** @brief Release Vulkan objects before the borrowed DMA-BUF can close. */
    ~renderer_t();

    /**
     * @brief Create a renderer and import an externally allocated DMA-BUF.
     *
     * Vulkan imports a duplicated descriptor and therefore never consumes the
     * caller's descriptor. The caller must still keep the original DMA-BUF
     * alive for the renderer and its RGA import.
     *
     * @param dma_buf_fd Borrowed external DMA-BUF descriptor.
     * @param allocation_size Available bytes in the DMA-BUF.
     * @param width Visible RGBA width.
     * @param height Visible RGBA height.
     * @param stride RGBA byte stride.
     * @return Initialized long-lived renderer.
     * @throws std::runtime_error When Vulkan or DMA-BUF import fails.
     */
    static std::unique_ptr<renderer_t> create(int dma_buf_fd, std::uint64_t allocation_size, std::uint32_t width, std::uint32_t height, std::uint32_t stride);

    /**
     * @brief Render a changed snapshot and synchronously release it to RGA.
     *
     * @param model Complete immutable UI snapshot.
     * @return true when Vulkan submitted work, false when the revision was already cached.
     * @throws std::runtime_error When validation or Vulkan execution fails.
     */
    bool render(const render_model_t &model);

    /** @brief Return the most recently completed model revision, or zero before rendering. */
    std::uint64_t rendered_revision() const noexcept;

    /** @brief Return the selected hardware Vulkan device name. */
    std::string device_name() const;

  private:
    class impl_t;
    explicit renderer_t(std::unique_ptr<impl_t> impl) noexcept;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace platf::vulkan_ui
