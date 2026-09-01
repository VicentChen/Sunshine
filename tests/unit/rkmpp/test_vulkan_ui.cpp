/**
 * @file tests/unit/rkmpp/test_vulkan_ui.cpp
 * @brief Unit tests for the renderer-independent Vulkan UI model.
 */

// standard includes
#include <cstddef>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/vulkan_ui.h"

namespace {
  TEST(VulkanUiModel, BuildsOpaqueFocusedModalPage) {
    platf::ui::snapshot_t snapshot {.visible = true, .modal = true, .page = platf::ui::page_e::profile, .focus = 2, .revision = 7};
    const auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    EXPECT_EQ(model.width, 960U);
    EXPECT_EQ(model.height, 180U);
    EXPECT_EQ(model.revision, 7U);
    EXPECT_EQ(model.page, platf::ui::page_e::profile);
    EXPECT_EQ(model.focus, 2);
    EXPECT_EQ(model.background.alpha, 1.0F);
    EXPECT_FALSE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, RejectsInvalidRevisionFocusAndTransparency) {
    platf::ui::snapshot_t snapshot {.visible = true, .modal = true};
    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    model.revision = 0;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.revision = 2;
    model.focus = 3;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.focus = 0;
    model.background.alpha = 0.5F;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, RejectsPanelsTooSmallForTheStaticLayout) {
    const platf::ui::snapshot_t snapshot {.visible = true, .modal = true};
    EXPECT_THROW(platf::vulkan_ui::make_render_model(959, 180, snapshot), std::runtime_error);
    EXPECT_THROW(platf::vulkan_ui::make_render_model(960, 179, snapshot), std::runtime_error);
  }

  TEST(VulkanUiDmaBuf, PlacesPackedBgrPanelAtBottomCenter) {
    const platf::vulkan_ui::bgr888_dma_buf_t target {
      .dma_buf_fd = 7,
      .allocation_size = 6'220'800,
      .width = 1920,
      .height = 1080,
      .stride = 5760,
      .generation = 4,
      .slot = 2
    };
    const auto region = platf::vulkan_ui::make_bgr888_copy_region(target, 960, 180, 32);
    EXPECT_EQ(region.panel_left, 480U);
    EXPECT_EQ(region.panel_top, 868U);
    EXPECT_EQ(region.buffer_offset, 5'001'120U);
    EXPECT_EQ(region.buffer_row_length, 1920U);
    EXPECT_EQ(region.buffer_image_height, 1080U);
    EXPECT_EQ(region.buffer_offset % 4U, 0U);
  }

  TEST(VulkanUiDmaBuf, RejectsInvalidPackedBgrLayouts) {
    platf::vulkan_ui::bgr888_dma_buf_t target {
      .dma_buf_fd = 7,
      .allocation_size = 6'220'800,
      .width = 1920,
      .height = 1080,
      .stride = 5760
    };
    target.stride = 5759;
    EXPECT_THROW(platf::vulkan_ui::make_bgr888_copy_region(target, 960, 180, 32), std::runtime_error);
    target.stride = 5760;
    target.allocation_size = 6'000'000;
    EXPECT_THROW(platf::vulkan_ui::make_bgr888_copy_region(target, 960, 180, 32), std::runtime_error);
  }

  TEST(VulkanUiModel, CarriesSanitizedConnectionStatusAndRejectsOversizedText) {
    platf::ui::snapshot_t snapshot {
      .visible = true,
      .page = platf::ui::page_e::connection_status,
      .connection = {
        .video_state = "negotiating",
        .gamepad_state = "starting",
        .gamepad_stage = "handshake",
        .moonlight_width = 3840,
        .moonlight_height = 2160,
        .input_width = 1920,
        .input_height = 1080
      },
      .revision = 9
    };
    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    EXPECT_EQ(model.connection.gamepad_stage, "handshake");
    EXPECT_FALSE(platf::vulkan_ui::validate_render_model(model));
    model.connection.failure_kind.assign(65, 'x');
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, CarriesCompletedProfileWindowAndRejectsInvalidPercentiles) {
    platf::ui::snapshot_t snapshot {
      .visible = true,
      .modal = true,
      .page = platf::ui::page_e::profile,
      .profile = {
        .captured_frames = 169,
        .placeholder_frames = 2,
        .rga_bypass_frames = 169,
        .freshness_drops = 131,
        .hdmirx_width = 1920,
        .hdmirx_height = 1080,
        .moonlight_width = 1920,
        .moonlight_height = 1080,
        .available = true
      },
      .revision = 11
    };
    auto &host_send = snapshot.profile.metrics[static_cast<std::size_t>(platf::ui::profile_metric_e::host_send)];
    host_send = {.count = 169, .p50_us = 26000, .p95_us = 33000, .p99_us = 35000};

    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    EXPECT_TRUE(model.profile.available);
    EXPECT_EQ(model.profile.captured_frames, 169U);
    EXPECT_EQ(model.profile.rga_bypass_frames, 169U);
    EXPECT_EQ(model.profile.freshness_drops, 131U);
    EXPECT_EQ(model.profile.metrics[static_cast<std::size_t>(platf::ui::profile_metric_e::host_send)].p95_us, 33000);
    EXPECT_FALSE(platf::vulkan_ui::validate_render_model(model));

    model.profile.metrics[static_cast<std::size_t>(platf::ui::profile_metric_e::host_send)].p99_us = 32000;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, BuildsRollingTimelineGeometryAndPreservesRelativeBounds) {
    video::frame_profile_timeline_snapshot_t timeline {.frame_count = 2, .stream_generation = 3};
    timeline.frames[0] = {
      .spans = {{{video::frame_profile_timeline_stage_e::mpp_output_wait, video::frame_profile_timeline_lane_e::mpp, 3000, 9000}}},
      .frame_index = 10,
      .origin_offset_us = 0,
      .end_us = 12000,
      .span_count = 1
    };
    timeline.frames[1] = {
      .spans = {{{video::frame_profile_timeline_stage_e::rga, video::frame_profile_timeline_lane_e::rga, 1000, 5000}}},
      .frame_index = 11,
      .origin_offset_us = 16667,
      .end_us = 11000,
      .span_count = 1
    };

    const auto geometry = platf::vulkan_ui::make_timeline_geometry(timeline);
    ASSERT_EQ(geometry.bar_count, 2U);
    EXPECT_EQ(geometry.latest_frame_index, 11);
    EXPECT_EQ(geometry.latest_frame_end_us, 11000);
    EXPECT_EQ(geometry.bars[0].start_us, 3000);
    EXPECT_EQ(geometry.bars[0].end_us, 9000);
    EXPECT_LT(geometry.bars[0].left, geometry.bars[0].right);
    EXPECT_LT(geometry.bars[0].right, geometry.bars[1].left);
  }

  TEST(VulkanUiModel, RejectsInvalidTimelineStageBounds) {
    platf::ui::snapshot_t snapshot {
      .visible = true,
      .modal = true,
      .page = platf::ui::page_e::profile,
      .revision = 12
    };
    snapshot.profile.timeline.frame_count = 1;
    snapshot.profile.timeline.frames[0] = {
      .spans = {{{video::frame_profile_timeline_stage_e::mpp_submit, video::frame_profile_timeline_lane_e::mpp, 3000, 4000}}},
      .frame_index = 1,
      .end_us = 5000,
      .span_count = 1
    };
    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    model.profile.timeline.frames[0].spans[0].end_us = 2000;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

}  // namespace
