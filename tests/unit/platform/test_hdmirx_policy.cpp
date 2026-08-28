/**
 * @file tests/unit/platform/test_hdmirx_policy.cpp
 * @brief Unit tests for hardware-independent HDMI RX sizing policy.
 */
#include "../../tests_common.h"

#include <limits>

#include <src/platform/hdmirx_policy.h>

namespace {
  using platf::hdmirx::hdmi_mode_t;
  using platf::hdmirx::pixel_format_e;
  using platf::hdmirx::refresh_rate_t;
  using platf::hdmirx::resolution_t;

  TEST(HdmirxPolicyViewport, EqualDimensionsUseFullFrame) {
    const auto viewport = platf::hdmirx::make_viewport({1920, 1080}, {1920, 1080});
    ASSERT_TRUE(viewport.has_value());
    EXPECT_FALSE(platf::hdmirx::needs_conversion({1920, 1080}, {1920, 1080}));
    EXPECT_EQ(viewport->source, (platf::hdmirx::rectangle_t {0, 0, 1920, 1080}));
    EXPECT_EQ(viewport->destination, (platf::hdmirx::rectangle_t {0, 0, 1920, 1080}));
    EXPECT_TRUE(platf::hdmirx::viewport_covers_target(*viewport, {1920, 1080}));
  }

  TEST(HdmirxPolicyViewport, DownsamplesWithAspectPreservation) {
    const auto four_k = platf::hdmirx::make_viewport({3840, 2160}, {1920, 1080});
    const auto full_hd = platf::hdmirx::make_viewport({1920, 1080}, {1280, 720});
    ASSERT_TRUE(four_k.has_value());
    ASSERT_TRUE(full_hd.has_value());
    EXPECT_EQ(four_k->destination, (platf::hdmirx::rectangle_t {0, 0, 1920, 1080}));
    EXPECT_EQ(full_hd->destination, (platf::hdmirx::rectangle_t {0, 0, 1280, 720}));
    EXPECT_TRUE(platf::hdmirx::needs_conversion({3840, 2160}, {1920, 1080}));
  }

  TEST(HdmirxPolicyViewport, AllowsUpsampling) {
    const auto viewport = platf::hdmirx::make_viewport({1280, 720}, {1920, 1080});
    ASSERT_TRUE(viewport.has_value());
    EXPECT_EQ(viewport->destination, (platf::hdmirx::rectangle_t {0, 0, 1920, 1080}));
  }

  TEST(HdmirxPolicyViewport, CentersPillarboxAndLetterbox) {
    const auto pillarbox = platf::hdmirx::make_viewport({1440, 1080}, {1920, 1080});
    const auto letterbox = platf::hdmirx::make_viewport({2560, 1080}, {1920, 1080});
    ASSERT_TRUE(pillarbox.has_value());
    ASSERT_TRUE(letterbox.has_value());
    EXPECT_EQ(pillarbox->destination, (platf::hdmirx::rectangle_t {240, 0, 1440, 1080}));
    EXPECT_EQ(letterbox->destination, (platf::hdmirx::rectangle_t {0, 135, 1920, 810}));
    EXPECT_FALSE(platf::hdmirx::viewport_covers_target(*pillarbox, {1920, 1080}));
    EXPECT_FALSE(platf::hdmirx::viewport_covers_target(*letterbox, {1920, 1080}));
  }

  TEST(HdmirxPolicyViewport, RejectsInvalidAndOversizedDimensions) {
    EXPECT_FALSE(platf::hdmirx::is_valid_resolution({0, 1080}));
    EXPECT_FALSE(platf::hdmirx::is_valid_resolution({1080, 0}));
    EXPECT_FALSE(platf::hdmirx::is_valid_resolution({std::numeric_limits<std::uint32_t>::max(), 1080}));
    EXPECT_FALSE(platf::hdmirx::make_viewport({1920, 1080}, {0, 720}));
    EXPECT_FALSE(platf::hdmirx::make_viewport({1920, 1080}, {33'000, 720}));
  }

  TEST(HdmirxPolicyNv12, RejectsOddDimensionsAndAlignsStride) {
    EXPECT_FALSE(platf::hdmirx::make_viewport({1279, 720}, {1920, 1080}, pixel_format_e::nv12));
    EXPECT_FALSE(platf::hdmirx::make_viewport({1280, 720}, {1921, 1080}, pixel_format_e::nv12));
    const auto layout = platf::hdmirx::make_nv12_layout({1920, 1080}, 0, 64);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->stride, 1920U);
    EXPECT_EQ(layout->allocation_size, 3ULL * 1920 * 1080 / 2);
    EXPECT_FALSE(platf::hdmirx::make_nv12_layout({1920, 1080}, 1919, 64));
    EXPECT_FALSE(platf::hdmirx::make_nv12_layout({1920, 1080}, 1920, 3));
  }

  TEST(HdmirxPolicyNv12, KeepsChromaOffsetsEven) {
    const auto viewport = platf::hdmirx::make_viewport({1024, 768}, {1920, 1080}, pixel_format_e::nv12);
    ASSERT_TRUE(viewport.has_value());
    EXPECT_EQ(viewport->destination.left % 2, 0U);
    EXPECT_EQ(viewport->destination.top % 2, 0U);
    EXPECT_EQ(viewport->destination.width % 2, 0U);
    EXPECT_EQ(viewport->destination.height % 2, 0U);
  }

  TEST(HdmirxPolicyMode, ChoosesSmallestSufficientMode) {
    const std::vector<hdmi_mode_t> modes {
      {{3840, 2160}, {60, 1}, true},
      {{2560, 1440}, {60, 1}, true},
      {{1920, 1080}, {60, 1}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {1920, 1080});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t {1920, 1080}));
  }

  TEST(HdmirxPolicyMode, ChoosesLargestWhenNoModeIsSufficient) {
    const std::vector<hdmi_mode_t> modes {
      {{1280, 720}, {60, 1}, true},
      {{1920, 1080}, {60, 1}, true},
      {{1600, 900}, {60, 1}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {2560, 1440});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t {1920, 1080}));
  }

  TEST(HdmirxPolicyMode, UsesAspectAndRefreshTieBreakersDeterministically) {
    const std::vector<hdmi_mode_t> modes {
      {{1280, 1281}, {60, 1}, true},
      {{1281, 1280}, {50, 1}, true},
      {{1281, 1280}, {60, 1}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {1280, 720}, refresh_rate_t {59, 1});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t {1281, 1280}));
    EXPECT_EQ(selected->refresh_rate.numerator, 60U);

    const std::vector<hdmi_mode_t> reversed(modes.rbegin(), modes.rend());
    EXPECT_EQ(platf::hdmirx::select_hdmi_mode(reversed, {1280, 720}, refresh_rate_t {59, 1}), selected);
  }

  TEST(HdmirxPolicyMode, IgnoresUnverifiedAndEmptyCandidates) {
    EXPECT_FALSE(platf::hdmirx::select_hdmi_mode({}, {1920, 1080}));
    const std::vector<hdmi_mode_t> default_unverified {{{1920, 1080}, {60, 1}}};
    EXPECT_FALSE(platf::hdmirx::select_hdmi_mode(default_unverified, {1920, 1080}));
    const std::vector<hdmi_mode_t> unverified {{{1920, 1080}, {60, 1}, false}};
    EXPECT_FALSE(platf::hdmirx::select_hdmi_mode(unverified, {1920, 1080}));
    const std::vector<hdmi_mode_t> invalid_refresh {{{1920, 1080}, {0, 1}, true}};
    EXPECT_FALSE(platf::hdmirx::select_hdmi_mode(invalid_refresh, {1920, 1080}));
    EXPECT_FALSE(platf::hdmirx::select_hdmi_mode({{{1920, 1080}, {60, 1}, true}}, {0, 1080}));
    EXPECT_FALSE(platf::hdmirx::select_hdmi_mode({{{1920, 1080}, {60, 1}, true}}, {1920, 1080}, refresh_rate_t {60, 0}));
  }

  TEST(HdmirxPolicyMode, ComparesRefreshDistanceAsARealRational) {
    const std::vector<hdmi_mode_t> modes {
      {{1920, 1080}, {59, 1}, true},
      {{1920, 1080}, {60'000, 1'001}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {1920, 1080}, refresh_rate_t {60, 1});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->refresh_rate, (refresh_rate_t {60'000, 1'001}));
  }

  TEST(HdmirxPolicyMode, ComparesAspectDistanceAsARealRatio) {
    const std::vector<hdmi_mode_t> modes {
      {{923, 480}, {60, 1}, true},
      {{852, 520}, {60, 1}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {1920, 1080});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t {852, 520}));
  }

  TEST(HdmirxPolicyMode, FallbackUsesAspectBeforeDimensionDelta) {
    const std::vector<hdmi_mode_t> modes {
      {{500, 200}, {60, 1}, true},
      {{400, 250}, {60, 1}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {1920, 1080});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t {400, 250}));
  }

  TEST(HdmirxPolicyMode, UsesRawRefreshFieldsForEquivalentRates) {
    const std::vector<hdmi_mode_t> modes {
      {{1920, 1080}, {60'000, 1'000}, true},
      {{1920, 1080}, {60, 1}, true},
    };
    const auto selected = platf::hdmirx::select_hdmi_mode(modes, {1920, 1080}, refresh_rate_t {60, 1});
    const std::vector<hdmi_mode_t> reversed(modes.rbegin(), modes.rend());
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->refresh_rate, (refresh_rate_t {60, 1}));
    EXPECT_EQ(platf::hdmirx::select_hdmi_mode(reversed, {1920, 1080}, refresh_rate_t {60, 1}), selected);
  }

  TEST(HdmirxPolicyNv12, RejectsTooSmallAlignedViewport) {
    EXPECT_FALSE(platf::hdmirx::make_viewport({2, 4}, {4, 2}, pixel_format_e::nv12));
    EXPECT_FALSE(platf::hdmirx::make_nv12_layout({2, 2}, 0, 0));
    const auto derived = platf::hdmirx::make_nv12_layout({1922, 1080}, 0, 64);
    ASSERT_TRUE(derived.has_value());
    EXPECT_EQ(derived->stride, 1984U);
    const auto supplied = platf::hdmirx::make_nv12_layout({1920, 1080}, 1984, 64);
    ASSERT_TRUE(supplied.has_value());
    EXPECT_EQ(supplied->stride, 1984U);
  }
}  // namespace
