/**
 * @file tests/unit/rkmpp/test_vulkan_ui.cpp
 * @brief Unit tests for the renderer-independent Vulkan UI model.
 */

// standard includes
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/vulkan_ui.h"

namespace {
  TEST(VulkanUiModel, BuildsOpaqueFocusedModalPage) {
    platf::ui::snapshot_t snapshot {
      .visible = true,
      .modal = true,
      .page = platf::ui::page_e::profile,
      .focus = 2,
      .profile_scroll_steps = 3,
      .revision = 7
    };
    const auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    EXPECT_EQ(model.width, 960U);
    EXPECT_EQ(model.height, 180U);
    EXPECT_EQ(model.revision, 7U);
    EXPECT_EQ(model.page, platf::ui::page_e::profile);
    EXPECT_EQ(model.focus, 2);
    EXPECT_EQ(model.profile_scroll_steps, 3U);
    EXPECT_EQ(model.background.alpha, 1.0F);
    EXPECT_FALSE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, RejectsInvalidRevisionFocusAndTransparency) {
    platf::ui::snapshot_t snapshot {.visible = true, .modal = true};
    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    model.revision = 0;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.revision = 2;
    model.focus = 4;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.focus = 0;
    model.ui_size_focus = 3;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.ui_size_focus = 1;
    model.background.alpha = 0.5F;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiLayout, Scales1080pAnd4kAtTheSameRelativeSize) {
    const auto hd = platf::vulkan_ui::make_layout_metrics(1920, 1080);
    EXPECT_EQ(hd.standard_panel.width, 1280U);
    EXPECT_EQ(hd.standard_panel.height, 720U);
    EXPECT_EQ(hd.profile_panel.width, 1440U);
    EXPECT_EQ(hd.profile_panel.height, 720U);
    EXPECT_EQ(hd.panel_margin, 36U);
    EXPECT_FLOAT_EQ(hd.body_font_pixels, 28.0F);
    EXPECT_FLOAT_EQ(hd.title_font_pixels, 36.0F);

    const auto uhd = platf::vulkan_ui::make_layout_metrics(3840, 2160);
    EXPECT_EQ(uhd.standard_panel.width, 2560U);
    EXPECT_EQ(uhd.standard_panel.height, 1440U);
    EXPECT_EQ(uhd.profile_panel.width, 2880U);
    EXPECT_EQ(uhd.profile_panel.height, 1440U);
    EXPECT_EQ(uhd.panel_margin, 72U);
    EXPECT_FLOAT_EQ(uhd.body_font_pixels, 56.0F);
    EXPECT_FLOAT_EQ(uhd.title_font_pixels, 72.0F);
  }

  TEST(VulkanUiLayout, AppliesThreeUserSizeTiersWithoutLosingResolutionAdaptation) {
    const auto compact = platf::vulkan_ui::make_layout_metrics(1920, 1080, platf::ui::ui_size_e::compact);
    EXPECT_EQ(compact.standard_panel.width, 1088U);
    EXPECT_EQ(compact.standard_panel.height, 612U);
    EXPECT_EQ(compact.profile_panel.width, 1224U);
    EXPECT_EQ(compact.profile_panel.height, 612U);
    EXPECT_EQ(compact.panel_margin, 30U);
    EXPECT_FLOAT_EQ(compact.body_font_pixels, 23.8F);
    EXPECT_FLOAT_EQ(compact.title_font_pixels, 30.6F);

    const auto large = platf::vulkan_ui::make_layout_metrics(3840, 2160, platf::ui::ui_size_e::large);
    EXPECT_EQ(large.standard_panel.width, 3072U);
    EXPECT_EQ(large.standard_panel.height, 1728U);
    EXPECT_EQ(large.profile_panel.width, 3456U);
    EXPECT_EQ(large.profile_panel.height, 1728U);
    EXPECT_EQ(large.panel_margin, 86U);
    EXPECT_FLOAT_EQ(large.body_font_pixels, 67.2F);
    EXPECT_FLOAT_EQ(large.title_font_pixels, 86.4F);
  }

  TEST(VulkanUiLayout, UsesTheLimitingAxisAndPreservesProfileTopology) {
    const auto ultrawide = platf::vulkan_ui::make_layout_metrics(2560, 1080);
    EXPECT_EQ(ultrawide.standard_panel.width, 1280U);
    EXPECT_EQ(ultrawide.standard_panel.height, 720U);
    EXPECT_EQ(ultrawide.profile_panel.width, 1440U);
    EXPECT_EQ(ultrawide.profile_panel.height, 720U);
    EXPECT_EQ(
      platf::vulkan_ui::panel_for_page(ultrawide, platf::ui::page_e::main_menu).height,
      ultrawide.standard_panel.height
    );
    EXPECT_EQ(
      platf::vulkan_ui::panel_for_page(ultrawide, platf::ui::page_e::profile).height,
      ultrawide.profile_panel.height
    );

    const auto hd720 = platf::vulkan_ui::make_layout_metrics(1280, 720);
    EXPECT_EQ(hd720.standard_panel.width, 852U);
    EXPECT_EQ(hd720.standard_panel.height, 480U);
    EXPECT_EQ(hd720.profile_panel.width, 960U);
    EXPECT_EQ(hd720.profile_panel.height, 480U);
    EXPECT_EQ(hd720.panel_margin, 24U);
    EXPECT_THROW(platf::vulkan_ui::make_layout_metrics(1024, 576), std::runtime_error);
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
    const auto metrics = platf::vulkan_ui::make_layout_metrics(1920, 1080);
    const auto region = platf::vulkan_ui::make_bgr888_copy_region(
      target,
      metrics.standard_panel.width,
      metrics.standard_panel.height,
      metrics.panel_margin
    );
    EXPECT_EQ(region.panel_left, 320U);
    EXPECT_EQ(region.panel_top, 324U);
    EXPECT_EQ(region.buffer_offset, 1'867'200U);
    EXPECT_EQ(region.buffer_row_length, 1920U);
    EXPECT_EQ(region.buffer_image_height, 1080U);
    EXPECT_EQ(region.buffer_offset % 4U, 0U);
  }

  TEST(VulkanUiDmaBuf, AlignsCompactProfileStrideForExternalMemoryImport) {
    const auto compact = platf::vulkan_ui::make_layout_metrics(1920, 1080, platf::ui::ui_size_e::compact);
    ASSERT_EQ(compact.profile_panel.width, 1224U);
    const auto stride = platf::vulkan_ui::make_bgr888_panel_stride(compact.profile_panel.width);
    EXPECT_EQ(stride, 3840U);
    EXPECT_GE(stride, compact.profile_panel.width * 3U);
    EXPECT_EQ(stride % (64U * 3U), 0U);
    EXPECT_THROW(platf::vulkan_ui::make_bgr888_panel_stride(0), std::runtime_error);
    EXPECT_THROW(platf::vulkan_ui::make_bgr888_panel_stride(std::numeric_limits<std::uint32_t>::max()), std::runtime_error);
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
        .moonlight_fps_x100 = 5994,
        .input_width = 1920,
        .input_height = 1080
      },
      .revision = 9
    };
    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    EXPECT_EQ(model.connection.gamepad_stage, "handshake");
    EXPECT_EQ(model.connection.moonlight_fps_x100, 5994U);
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

    model.profile_scroll_steps = platf::ui::profile_scroll_step_limit + 1U;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.profile_scroll_steps = 0;

    model.profile.metrics[static_cast<std::size_t>(platf::ui::profile_metric_e::host_send)].p99_us = 32000;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, BuildsOnlyTheLatestFrameAlongsideTheRecentAverage) {
    video::frame_profile_timeline_snapshot_t timeline {.frame_count = 2, .stream_generation = 3};
    timeline.frames[0] = {
      .spans = {{{video::frame_profile_timeline_stage_e::mpp_packet_get, video::frame_profile_timeline_lane_e::mpp, 3000, 9000}}},
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
    ASSERT_EQ(geometry.bar_count, 1U);
    EXPECT_EQ(geometry.frame_count, 2U);
    EXPECT_EQ(geometry.view_start_us, 0);
    EXPECT_EQ(geometry.view_end_us, 11000);
    EXPECT_EQ(geometry.latest_frame_index, 11);
    EXPECT_EQ(geometry.latest_frame_end_us, 11000);
    EXPECT_EQ(geometry.bars[0].frame_index, 11);
    EXPECT_EQ(geometry.bars[0].stage, video::frame_profile_timeline_stage_e::rga);
    EXPECT_EQ(geometry.bars[0].start_us, 1000);
    EXPECT_EQ(geometry.bars[0].end_us, 5000);
    EXPECT_LT(geometry.bars[0].left, geometry.bars[0].right);
    ASSERT_EQ(geometry.average_bar_count, 2U);
    EXPECT_EQ(geometry.average_bars[0].sample_count, 1U);
    EXPECT_EQ(geometry.average_bars[1].sample_count, 1U);
  }

  TEST(VulkanUiModel, BuildsRxEofAlignedAverageTimeline) {
    video::frame_profile_timeline_snapshot_t timeline {.frame_count = 2};
    timeline.frames[0] = {
      .spans = {{{video::frame_profile_timeline_stage_e::mpp_encode, video::frame_profile_timeline_lane_e::mpp, 100, 200}}},
      .frame_index = 1,
      .origin_offset_us = 500000,
      .end_us = 200,
      .span_count = 1
    };
    timeline.frames[1] = {
      .spans = {{{video::frame_profile_timeline_stage_e::mpp_encode, video::frame_profile_timeline_lane_e::mpp, 200, 500}}},
      .frame_index = 2,
      .origin_offset_us = 516667,
      .end_us = 500,
      .span_count = 1
    };

    const auto geometry = platf::vulkan_ui::make_timeline_geometry(timeline);
    ASSERT_EQ(geometry.average_bar_count, 1U);
    const auto &average = geometry.average_bars[0];
    EXPECT_EQ(average.stage, video::frame_profile_timeline_stage_e::mpp_encode);
    EXPECT_EQ(average.lane, video::frame_profile_timeline_lane_e::mpp);
    EXPECT_EQ(average.sample_count, 2U);
    EXPECT_EQ(average.start_us, 150);
    EXPECT_EQ(average.end_us, 350);
    EXPECT_EQ(geometry.average_view_end_us, 1000);
    EXPECT_LT(average.left, average.right);
  }

  TEST(VulkanUiModel, RetainsEveryStageWhenAveragingAFullHistory) {
    video::frame_profile_timeline_snapshot_t timeline {};
    timeline.frame_count = timeline.frames.size();
    for (std::size_t frame_index = 0; frame_index < timeline.frames.size(); ++frame_index) {
      auto &frame = timeline.frames[frame_index];
      frame.frame_index = frame_index;
      frame.end_us = 20000;
      frame.span_count = frame.spans.size();
      for (std::size_t stage = 0; stage < frame.spans.size(); ++stage) {
        const auto start = static_cast<std::int64_t>(stage * 1000 + frame_index * 2);
        frame.spans[stage] = {
          static_cast<video::frame_profile_timeline_stage_e>(stage),
          video::frame_profile_timeline_lane_e::ui,
          start,
          start + 100 + static_cast<std::int64_t>(frame_index * 2)
        };
      }
    }

    const auto geometry = platf::vulkan_ui::make_timeline_geometry(timeline);
    ASSERT_EQ(geometry.bar_count, video::frame_profile_timeline_frame_t::max_spans);
    ASSERT_EQ(geometry.average_bar_count, video::frame_profile_timeline_frame_t::max_spans);
    const auto last_frame = static_cast<std::int64_t>(timeline.frame_count - 1U);
    for (std::size_t stage = 0; stage < geometry.bar_count; ++stage) {
      SCOPED_TRACE(stage);
      const auto base = static_cast<std::int64_t>(stage * 1000);
      const auto &latest = geometry.bars[stage];
      const auto &average = geometry.average_bars[stage];
      EXPECT_EQ(latest.stage, static_cast<video::frame_profile_timeline_stage_e>(stage));
      EXPECT_EQ(latest.frame_index, last_frame);
      EXPECT_EQ(latest.start_us, base + last_frame * 2);
      EXPECT_EQ(latest.end_us, base + 100 + last_frame * 4);
      EXPECT_EQ(average.stage, latest.stage);
      EXPECT_EQ(average.sample_count, timeline.frame_count);
      EXPECT_EQ(average.start_us, base + last_frame);
      EXPECT_EQ(average.end_us, base + 100 + last_frame * 2);
    }
  }

  TEST(VulkanUiModel, RejectsOutOfRangeStageBeforeIndexingTimelineAccumulators) {
    video::frame_profile_timeline_snapshot_t timeline {.frame_count = 1};
    timeline.frames[0].span_count = 1;
    timeline.frames[0].spans[0].stage = video::frame_profile_timeline_stage_e::count;
    const auto geometry = platf::vulkan_ui::make_timeline_geometry(timeline);
    EXPECT_EQ(geometry.frame_count, 0U);
    EXPECT_EQ(geometry.bar_count, 0U);
    EXPECT_EQ(geometry.average_bar_count, 0U);
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
      .spans = {{{video::frame_profile_timeline_stage_e::mpp_encode, video::frame_profile_timeline_lane_e::mpp, 3000, 4000}}},
      .frame_index = 1,
      .end_us = 5000,
      .span_count = 1
    };
    auto model = platf::vulkan_ui::make_render_model(960, 180, snapshot);
    model.profile.timeline.frames[0].spans[0].end_us = 2000;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

}  // namespace
