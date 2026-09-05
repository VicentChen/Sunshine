/**
 * @file src/platform/linux/rkmpp_preprocess.h
 * @brief Contracts shared by the RKMPP preprocess and encode workers.
 */
#pragma once

#include "src/platform/linux/rkmpp.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace platf::rkmpp {
  /** @brief Route used to produce one immutable RKMPP input. */
  enum class prepared_route_e : std::uint8_t {
    direct,  ///< HDMI RX DMA-BUF submitted without a full-frame copy.
    rga,  ///< Session-private target populated by RGA before submission.
    placeholder  ///< Session-private target containing the no-signal image.
  };

  /** @brief Format-aware UI preparation selected for one live frame. */
  enum class ui_preprocess_path_e : std::uint8_t {
    hidden,  ///< UI is hidden and does not alter the video path.
    direct_bgr,  ///< Vulkan covers an exclusively leased BGR capture DMA-BUF.
    direct_nv12,  ///< RGA converts a completed Vulkan panel into an exclusively leased NV12 ROI.
    private_bgr  ///< RGA produces a private BGR target before Vulkan covers it.
  };

  /**
   * @brief Select the safe UI path without conflating visibility with scaling.
   *
   * Matching capture can be covered in place only when the current frame is
   * not shared by multiple encoder sessions. NV12 additionally requires an
   * explicitly validated plane layout and color range.
   *
   * @param input_format Native MPP layout of the captured frame.
   * @param dimensions_match Whether capture and coded dimensions are equal.
   * @param ui_visible Whether the controller currently exposes a UI page.
   * @param direct_write_safe Whether only one session can mutate the frame.
   * @param nv12_layout_safe Whether the native plane supports BT.709 limited ROI writes.
   * @return Format-aware preparation path for the current frame.
   */
  constexpr ui_preprocess_path_e select_ui_preprocess_path(
    MppFrameFormat input_format,
    bool dimensions_match,
    bool ui_visible,
    bool direct_write_safe,
    bool nv12_layout_safe = false
  ) noexcept {
    if (!ui_visible) {
      return ui_preprocess_path_e::hidden;
    }
    if (input_format == MPP_FMT_BGR888 && dimensions_match && direct_write_safe) {
      return ui_preprocess_path_e::direct_bgr;
    }
    if (input_format == MPP_FMT_YUV420SP && dimensions_match && direct_write_safe && nv12_layout_safe) {
      return ui_preprocess_path_e::direct_nv12;
    }
    return ui_preprocess_path_e::private_bgr;
  }

  /**
   * @brief Retry a frame without optional UI when its contiguous allocation fails.
   *
   * The prepare callback must retain the raw input until it returns successfully.
   * Only memory exhaustion in a visible-UI attempt permits one retry; errors in
   * the required video path propagate to the session failure handler.
   *
   * @param ui_visible Whether this frame requests UI composition.
   * @param prepare Prepare the same raw frame with the requested UI state.
   * @param disable_ui Release optional UI resources and report the allocation error.
   * @return The prepared frame, including an empty result after cancellation.
   */
  template<class Prepare, class DisableUi>
  auto prepare_with_optional_ui(bool ui_visible, Prepare &&prepare, DisableUi &&disable_ui) -> decltype(prepare(ui_visible)) {
    try {
      return prepare(ui_visible);
    } catch (const std::system_error &error) {
      if (!ui_visible || error.code() != std::errc::not_enough_memory) {
        throw;
      }
      disable_ui(error);
    }
    return prepare(false);
  }

  /**
   * @brief Immutable handoff from the preprocess worker to the encode worker.
   *
   * The holder pins either the source capture frame or one private RGA target.
   * The encode worker is the only consumer and may submit this object at most
   * once. Replacing or destroying it releases the holder immediately.
   */
  struct prepared_frame_t {
    input_layout_t layout;  ///< Exact layout accepted by the next MPP session.
    int dma_buf_fd {-1};  ///< Borrowed descriptor kept valid by `holder`.
    std::uint64_t allocation_size {};  ///< Complete producer allocation.
    std::int64_t pts {};  ///< Producer presentation timestamp.
    std::uint64_t generation {};  ///< Producer allocation generation.
    input_buffer_key_t cache_key;  ///< Stable import identity within `generation`.
    input_holder_t holder;  ///< Direct-capture or private-target lifetime pin.
    prepared_route_e route {prepared_route_e::direct};  ///< Preparation route.
    bool request_idr {};  ///< Sticky IDR request attached to this submission.
    std::optional<video::frame_profile_t> profile;  ///< Profile moved across the thread boundary.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;  ///< Capture timestamp forwarded to the packet.

    /**
     * @brief Build the short-lived MPP input view for synchronous submission.
     *
     * @return Input frame whose profile pointer refers to this object.
     */
    input_frame_t input_frame() {
      return {
        .layout = layout,
        .dma_buf_fd = dma_buf_fd,
        .allocation_size = allocation_size,
        .pts = pts,
        .holder = holder,
        .profile = profile ? &*profile : nullptr,
        .cache_key = cache_key
      };
    }
  };
}  // namespace platf::rkmpp
