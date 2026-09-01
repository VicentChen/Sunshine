/**
 * @file tests/unit/rkmpp/test_vulkan_ui.cpp
 * @brief Unit tests for the renderer-independent Vulkan UI model.
 */

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/vulkan_ui.h"

namespace {
  TEST(VulkanUiModel, BuildsOpaqueFocusedStatusPage) {
    const auto model = platf::vulkan_ui::make_status_model(960, 180, 2, 7);
    EXPECT_EQ(model.width, 960U);
    EXPECT_EQ(model.height, 180U);
    EXPECT_EQ(model.revision, 7U);
    EXPECT_EQ(model.focus, 2);
    EXPECT_EQ(model.background.alpha, 1.0F);
    EXPECT_FALSE(platf::vulkan_ui::validate_render_model(model));
  }

  TEST(VulkanUiModel, RejectsInvalidRevisionFocusAndTransparency) {
    auto model = platf::vulkan_ui::make_status_model(960, 180, 0, 1);
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
    EXPECT_THROW(platf::vulkan_ui::make_status_model(959, 180, 0), std::runtime_error);
    EXPECT_THROW(platf::vulkan_ui::make_status_model(960, 179, 0), std::runtime_error);
  }

}  // namespace
