/**
 * @file tests/unit/rkmpp/test_vulkan_ui.cpp
 * @brief Unit tests for the renderer-independent Vulkan UI model.
 */

// standard includes
#include <algorithm>
#include <limits>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/vulkan_ui.h"

namespace {
  TEST(VulkanUiModel, BuildsOpaqueBoundedStage5Page) {
    const auto model = platf::vulkan_ui::make_gate5_static_model(960, 180, 7);
    EXPECT_EQ(model.width, 960U);
    EXPECT_EQ(model.height, 180U);
    EXPECT_EQ(model.revision, 7U);
    EXPECT_GT(model.rectangles.size(), 100U);
    EXPECT_FALSE(platf::vulkan_ui::validate_render_model(model));
    EXPECT_TRUE(std::all_of(model.rectangles.begin(), model.rectangles.end(), [](const auto &rectangle) {
      return rectangle.color.alpha == 1.0F;
    }));
    ASSERT_GE(model.rectangles.size(), 4U);
    EXPECT_EQ(model.rectangles[1].top, model.rectangles[2].top);
    EXPECT_EQ(model.rectangles[2].top, model.rectangles[3].top);
    EXPECT_LT(model.rectangles[1].left, model.rectangles[2].left);
    EXPECT_LT(model.rectangles[2].left, model.rectangles[3].left);
  }

  TEST(VulkanUiModel, RejectsInvalidRevisionBoundsAndTransparency) {
    auto model = platf::vulkan_ui::make_gate5_static_model(960, 180, 1);
    model.revision = 0;
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.revision = 2;
    model.rectangles.push_back({959, 179, 2, 2, {1.0F, 1.0F, 1.0F, 1.0F}});
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.rectangles.back() = {0, 0, 1, 1, {1.0F, 1.0F, 1.0F, 0.5F}};
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
    model.rectangles.back().color.alpha = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, RejectsPanelsTooSmallForTheStaticLayout) {
    EXPECT_THROW(platf::vulkan_ui::make_gate5_static_model(959, 180), std::runtime_error);
    EXPECT_THROW(platf::vulkan_ui::make_gate5_static_model(960, 179), std::runtime_error);
  }

}  // namespace
