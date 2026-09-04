/**
 * @file tests/unit/rkmpp/test_rkmpp_stress.cpp
 * @brief Tests RKMPP encoder lifecycle resource stability.
 */

#include <gtest/gtest.h>

#ifdef SUNSHINE_BUILD_RKMPP

  #include "src/platform/linux/rkmpp.h"

  #include <dirent.h>

namespace {
  int count_fds() {
    int fd_count = 0;
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
      return -1;
    }
    while (readdir(dir)) {
      fd_count++;
    }
    closedir(dir);
    return fd_count;
  }
}  // namespace

TEST(RkmppStressTest, EncoderLifecycleNoFdLeaks) {
  // Repeatedly construct and destroy the public RKMPP encoder. This exercises
  // the MPP context/configuration lifetime without relying on video.cpp's
  // private session factory or a stale encode-device mock.
  platf::rkmpp::input_layout_t layout {
    1920,
    1080,
    1920,
    1080,
    MPP_FMT_YUV420SP
  };
  const platf::rkmpp::encoder_config_t config {
    .codec = platf::rkmpp::codec_e::h264,
    .input_layout = layout,
    .coded_width = 1920,
    .coded_height = 1080,
    .fps_num = 60,
    .fps_den = 1,
    .bitrate = 10'000'000,
    .gop = 60,
  };

  int initial_fds = count_fds();
  ASSERT_GT(initial_fds, 0) << "Failed to count fds";

  for (int i = 0; i < 50; ++i) {
    auto encoder = platf::rkmpp::encoder_t::create(config);
    EXPECT_EQ(encoder.encoded_frames(), 0U) << "Unexpected frame count on iteration " << i;
    const auto stats = encoder.stats();
    EXPECT_EQ(stats.frames, 0U) << "Unexpected stats on iteration " << i;
    EXPECT_EQ(stats.packets, 0U) << "Unexpected stats on iteration " << i;
    EXPECT_EQ(stats.bytes, 0U) << "Unexpected stats on iteration " << i;
    EXPECT_EQ(stats.min_packet_bytes, 0U) << "Unexpected stats on iteration " << i;
    EXPECT_EQ(stats.max_packet_bytes, 0U) << "Unexpected stats on iteration " << i;
    EXPECT_EQ(stats.output_buffer_bytes, 8U * 1024U * 1024U) << "Unexpected pool slot size on iteration " << i;
    EXPECT_EQ(stats.output_pool_capacity, platf::rkmpp::detail::output_pool_slot_count) << "Unexpected pool capacity on iteration " << i;
    EXPECT_EQ(stats.output_pool_peak_leases, 0U) << "Unexpected active pool lease on iteration " << i;
    EXPECT_EQ(stats.output_pool_waits, 0U) << "Unexpected pool wait on iteration " << i;
    // encoder goes out of scope here and destroys its MPP context/configuration.
  }

  int final_fds = count_fds();

  // allow some minor fluctuations (e.g. from background threads), but shouldn't grow by 50*N
  EXPECT_LE(final_fds - initial_fds, 10) << "FD leak detected! Initial: " << initial_fds << ", Final: " << final_fds;
}

#else
TEST(RkmppStressTest, IsNotBuiltWhenRkmppIsDisabled) {
  GTEST_SKIP() << "RKMPP support is disabled for this build";
}
#endif
