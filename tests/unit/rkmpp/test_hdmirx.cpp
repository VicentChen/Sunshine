/**
 * @file tests/unit/rkmpp/test_hdmirx.cpp
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

TEST(HdmirxBootstrapPolicy, InitializationNeverWaitsForLiveHdmi) {
  EXPECT_TRUE(platf::hdmirx::uses_synthetic_bootstrap_frame(platf::display_purpose_e::encoder_probe));
  EXPECT_TRUE(platf::hdmirx::uses_synthetic_bootstrap_frame(platf::display_purpose_e::stream));
  EXPECT_FALSE(platf::hdmirx::uses_synthetic_bootstrap_frame(static_cast<platf::display_purpose_e>(255)));
}

TEST(HdmirxTimestamp, DecodesClockType) {
  EXPECT_TRUE(platf::hdmirx::timestamp_is_monotonic(V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC));
  EXPECT_TRUE(platf::hdmirx::timestamp_is_monotonic(V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_SOE));
  EXPECT_FALSE(platf::hdmirx::timestamp_is_monotonic(V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN));
  EXPECT_FALSE(platf::hdmirx::timestamp_is_monotonic(V4L2_BUF_FLAG_TIMESTAMP_COPY));
}

TEST(HdmirxTimestamp, DecodesTimestampSource) {
  EXPECT_EQ(platf::hdmirx::timestamp_source(V4L2_BUF_FLAG_TSTAMP_SRC_EOF), platf::hdmirx::timestamp_source_e::end_of_frame);
  EXPECT_EQ(platf::hdmirx::timestamp_source(V4L2_BUF_FLAG_TSTAMP_SRC_SOE), platf::hdmirx::timestamp_source_e::start_of_exposure);
  EXPECT_EQ(platf::hdmirx::timestamp_source(V4L2_BUF_FLAG_TSTAMP_SRC_MASK), platf::hdmirx::timestamp_source_e::unknown);
  EXPECT_EQ(platf::hdmirx::timestamp_source_name(platf::hdmirx::timestamp_source_e::end_of_frame), "EOF");
  EXPECT_EQ(platf::hdmirx::timestamp_source_name(platf::hdmirx::timestamp_source_e::start_of_exposure), "SOE");
  EXPECT_EQ(platf::hdmirx::timestamp_source_name(platf::hdmirx::timestamp_source_e::unknown), "unknown");
}

TEST(HdmirxTimestamp, ConvertsMonotonicTimeval) {
  const timeval timestamp {.tv_sec = 123, .tv_usec = 456789};
  const auto converted = platf::hdmirx::v4l2_monotonic_timestamp(timestamp);
  EXPECT_EQ(converted.time_since_epoch(), std::chrono::seconds(123) + std::chrono::microseconds(456789));
}

TEST(HdmirxTimestamp, SteadyClockMatchesLinuxMonotonicClock) {
  EXPECT_TRUE(platf::hdmirx::steady_clock_matches_monotonic());
}
#else
TEST(HdmirxMppFormat, IsNotBuiltWhenRkmppIsDisabled) {
  GTEST_SKIP() << "RKMPP support is disabled for this build";
}
#endif
