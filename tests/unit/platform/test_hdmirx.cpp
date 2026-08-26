/**
 * @file tests/unit/platform/test_hdmirx.cpp
 * @brief Unit tests for HDMI RX format mapping.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_RKMPP
  #include <linux/videodev2.h>

  #include <src/platform/linux/hdmirx.h>

TEST(HdmirxMppFormat, MapsSupportedFormats) {
  EXPECT_EQ(platf::hdmirx::fourcc_to_mpp_format(V4L2_PIX_FMT_BGR24), MPP_FMT_BGR888);
  EXPECT_EQ(platf::hdmirx::fourcc_to_mpp_format(V4L2_PIX_FMT_NV24), MPP_FMT_YUV444SP);
  EXPECT_EQ(platf::hdmirx::fourcc_to_mpp_format(V4L2_PIX_FMT_NV16), MPP_FMT_YUV422SP);
  EXPECT_EQ(platf::hdmirx::fourcc_to_mpp_format(V4L2_PIX_FMT_NV12), MPP_FMT_YUV420SP);
}

TEST(HdmirxMppFormat, RejectsUnknownFormats) {
  EXPECT_EQ(platf::hdmirx::fourcc_to_mpp_format(v4l2_fourcc('T', 'E', 'S', 'T')), std::nullopt);
}

TEST(HdmirxMppFormat, ValidatesCompleteCaptureMetadata) {
  platf::hdmirx::capture_format_t format;
  format.width = 1920;
  format.height = 1080;
  format.fourcc = V4L2_PIX_FMT_BGR24;
  format.planes.push_back({5760, 6220800});
  format.mpp_format = MPP_FMT_BGR888;
  EXPECT_TRUE(platf::hdmirx::capture_format_is_valid(format));

  format.planes.front().sizeimage = 0;
  EXPECT_FALSE(platf::hdmirx::capture_format_is_valid(format));
  format.planes.front().sizeimage = 6220800;
  format.fourcc = v4l2_fourcc('T', 'E', 'S', 'T');
  EXPECT_FALSE(platf::hdmirx::capture_format_is_valid(format));
}
#else
TEST(HdmirxMppFormat, IsNotBuiltWhenRkmppIsDisabled) {
  GTEST_SKIP() << "RKMPP support is disabled for this build";
}
#endif
