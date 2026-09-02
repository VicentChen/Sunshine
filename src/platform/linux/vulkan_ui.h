/**
 * @file src/platform/linux/vulkan_ui.h
 * @brief Opaque Vulkan UI rendering into an externally allocated DMA-BUF.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// local includes
#include "ui_controller.h"

namespace platf::vulkan_ui {
  /** @brief Linear floating-point RGBA color used by the render model. */
  struct color_t {
    float red {};
    float green {};
    float blue {};
    float alpha {1.0F};
  };

  /** @brief One opaque UI surface size selected for a page family. */
  struct panel_layout_t {
    std::uint32_t width {};
    std::uint32_t height {};
  };

  /** @brief Resolution-derived sizes shared by the model, renderer, and ROI compositor. */
  struct layout_metrics_t {
    panel_layout_t standard_panel;  ///< Main menu and connection-status surface.
    panel_layout_t profile_panel;  ///< Wide, low surface preserving the Profile topology.
    std::uint32_t panel_margin {};  ///< Bottom safe-area margin in output pixels.
    float scale {};  ///< Linear scale relative to 1920x1080.
    float body_font_pixels {};
    float title_font_pixels {};
    float window_padding_x {};
    float window_padding_y {};
    float item_spacing_y {};
    float timeline_label_width {};
    float timeline_axis_height {};
    float timeline_min_lane_height {};
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
    platf::ui::page_e page {platf::ui::page_e::main_menu};  ///< Modal page to render.
    std::uint8_t focus {};  ///< Focused item in the three-entry main menu.
    platf::ui::connection_status_t connection;  ///< Sanitized connection values for the status page.
    platf::ui::profile_status_t profile;  ///< Latest completed-window values for the Profile page.
  };

  /** @brief One clipped renderer-independent bar in the rolling Timeline viewport. */
  struct timeline_bar_t {
    video::frame_profile_timeline_stage_e stage {video::frame_profile_timeline_stage_e::rx_driver_age};
    video::frame_profile_timeline_lane_e lane {video::frame_profile_timeline_lane_e::capture};
    std::int64_t frame_index {-1};
    std::int64_t start_us {};  ///< Unclipped start relative to the frame's RX EOF.
    std::int64_t end_us {};  ///< Unclipped end relative to the frame's RX EOF.
    float left {};  ///< Clipped normalized viewport coordinate.
    float right {};  ///< Clipped normalized viewport coordinate.
  };

  /** @brief Fixed geometry derived from recent completed frames without Vulkan state. */
  struct timeline_geometry_t {
    static constexpr std::size_t bar_capacity = video::frame_profile_timeline_snapshot_t::frame_capacity * video::frame_profile_timeline_frame_t::max_spans;

    std::array<timeline_bar_t, bar_capacity> bars;
    std::size_t bar_count {};
    std::int64_t view_start_us {};
    std::int64_t view_end_us {};
    std::int64_t latest_frame_index {-1};
    std::int64_t latest_frame_end_us {};
  };

  /** @brief One packed BGR888 capture DMA-BUF that Vulkan may cover in place. */
  struct bgr888_dma_buf_t {
    int dma_buf_fd {-1};  ///< Borrowed capture descriptor.
    std::uint64_t allocation_size {};  ///< Available bytes in the capture allocation.
    std::uint32_t width {};  ///< Visible frame width.
    std::uint32_t height {};  ///< Visible frame height.
    std::uint32_t stride {};  ///< Packed BGR byte stride.
    std::uint64_t generation {};  ///< Capture generation used to invalidate imports.
    std::uint32_t slot {};  ///< Stable buffer index within one generation.
  };

  /** @brief Validated Vulkan image-to-buffer copy geometry for a BGR888 panel. */
  struct bgr888_copy_region_t {
    std::uint64_t buffer_offset {};
    std::uint32_t buffer_row_length {};
    std::uint32_t buffer_image_height {};
    std::uint32_t panel_left {};
    std::uint32_t panel_top {};
  };

  /**
   * @brief Validate bounds and opacity before submitting a model to Vulkan.
   *
   * @param model Model to validate.
   * @return Empty on success, otherwise a stable diagnostic string.
   */
  std::optional<std::string> validate_render_model(const render_model_t &model);

  /** @brief Derive adaptive UI surfaces and typography from the encoded Moonlight output. */
  layout_metrics_t make_layout_metrics(std::uint32_t output_width, std::uint32_t output_height);

  /** @brief Select the surface family for one rendered page. */
  panel_layout_t panel_for_page(const layout_metrics_t &metrics, platf::ui::page_e page) noexcept;

  /**
   * @brief Build one renderer-independent Dear ImGui modal-page snapshot.
   *
   * @param width Panel width in pixels.
   * @param height Panel height in pixels.
   * @param snapshot Complete controller and automatic-status snapshot.
   * @return Opaque status-page state. Dear ImGui owns visible widget geometry.
   */
  render_model_t make_render_model(std::uint32_t width, std::uint32_t height, const platf::ui::snapshot_t &snapshot);

  /** @brief Convert the completed-frame ring into clipped rolling Timeline geometry. */
  timeline_geometry_t make_timeline_geometry(const video::frame_profile_timeline_snapshot_t &timeline);

  /** @brief Validate a packed BGR888 target and place one bottom-centered panel. */
  bgr888_copy_region_t make_bgr888_copy_region(const bgr888_dma_buf_t &target, std::uint32_t panel_width, std::uint32_t panel_height, std::uint32_t panel_margin);

  /** @brief Long-lived Vulkan renderer bound to one external BGR888 panel DMA-BUF. */
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
     * @param width Visible BGR width.
     * @param height Visible RGBA height.
     * @param stride packed BGR byte stride.
     * @return Initialized long-lived renderer.
     * @throws std::runtime_error When Vulkan or DMA-BUF import fails.
     */
    static std::unique_ptr<renderer_t> create(
      int dma_buf_fd,
      std::uint64_t allocation_size,
      std::uint32_t width,
      std::uint32_t height,
      std::uint32_t stride,
      const layout_metrics_t &metrics
    );

    /**
     * @brief Render a changed snapshot and synchronously release it to RGA.
     *
     * @param model Complete immutable UI snapshot.
     * @return true when Vulkan submitted work, false when the revision was already cached.
     * @throws std::runtime_error When validation or Vulkan execution fails.
     */
    bool render(const render_model_t &model);

    /** @brief Publish a changed cached panel to the external BGR DMA-BUF used by RGA fallback. */
    bool publish();

    /**
     * @brief Copy the cached opaque BGR panel directly into one capture DMA-BUF.
     *
     * The target is imported as a Vulkan buffer and retained by generation and
     * slot. Completion is synchronous so MPP may consume the same DMA-BUF when
     * this call returns.
     */
    bool cover_bgr888(const bgr888_dma_buf_t &target, std::uint32_t panel_margin);

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
