/**
 * @file tests/unit/rkmpp/test_rkmpp_input.cpp
 * @brief Unit tests for RKMPP's generic DMA-BUF input contract.
 */
#ifdef SUNSHINE_BUILD_RKMPP

  #include <algorithm>
  #include <array>
  #include <gtest/gtest.h>
  #include <limits>
  #include <src/platform/linux/rkmpp.h>
  #include <utility>
  #include <vector>

namespace {
  /**
   * @brief Build a valid 1080p NV12 input layout.
   *
   * @return Generic direct-DMA-BUF layout.
   */
  platf::rkmpp::input_layout_t nv12_layout() {
    return {1920, 1080, 1920, 1080, MPP_FMT_YUV420SP};
  }

  /**
   * @brief Build a validated generic frame with a test DMA-BUF descriptor.
   *
   * @param layout Producer layout to attach to the frame.
   * @param holder Producer lifetime pin to attach to the frame.
   * @return Input frame suitable for metadata validation.
   */
  platf::rkmpp::input_frame_t input_frame(platf::rkmpp::input_layout_t layout, platf::rkmpp::input_holder_t holder) {
    return {layout, 42, platf::rkmpp::detail::minimum_allocation_size(layout), 1234, std::move(holder)};
  }

  TEST(RkmppInputLayout, ValidatesDirectAndConvertedLayoutsIndependentlyFromCodedSize) {
    const auto direct = nv12_layout();
    const platf::rkmpp::input_layout_t converted {1280, 720, 1280, 720, MPP_FMT_YUV420SP};
    EXPECT_EQ(platf::rkmpp::validate_input_layout(direct), platf::rkmpp::input_status_e::ok);
    EXPECT_EQ(platf::rkmpp::validate_input_layout(converted), platf::rkmpp::input_status_e::ok);

    const platf::rkmpp::encoder_config_t direct_config {platf::rkmpp::codec_e::h264, direct, 1920, 1080};
    const platf::rkmpp::encoder_config_t converted_config {platf::rkmpp::codec_e::h265, converted, 1280, 720};
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(direct_config), platf::rkmpp::encoder_config_status_e::ok);
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(converted_config), platf::rkmpp::encoder_config_status_e::ok);
    EXPECT_EQ(direct_config.coded_width, 1920U);
    EXPECT_EQ(converted_config.coded_width, 1280U);
  }

  TEST(RkmppInputLayout, RejectsLayoutsThatCannotCoverVisiblePixels) {
    auto layout = nv12_layout();
    layout.horizontal_stride = 1919;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::stride_too_small);

    layout = nv12_layout();
    layout.vertical_stride = 1079;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::stride_too_small);
  }

  TEST(RkmppInputLayout, EnforcesFormatSpecificChromaAndPackedAlignment) {
    auto layout = nv12_layout();
    layout.visible_width = 1919;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::stride_too_small);

    layout = nv12_layout();
    layout.visible_height = 1079;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::stride_too_small);

    layout = nv12_layout();
    ++layout.horizontal_stride;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::stride_too_small);

    layout = nv12_layout();
    ++layout.vertical_stride;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::stride_too_small);

    const platf::rkmpp::input_layout_t nv16 {1921, 1080, 1921, 1080, MPP_FMT_YUV422SP};
    EXPECT_EQ(platf::rkmpp::validate_input_layout(nv16), platf::rkmpp::input_status_e::stride_too_small);

    const platf::rkmpp::input_layout_t bgr {1920, 1080, 5761, 1080, MPP_FMT_BGR888};
    EXPECT_EQ(platf::rkmpp::detail::minimum_allocation_size(bgr), 0U);
    EXPECT_EQ(platf::rkmpp::validate_input_layout(bgr), platf::rkmpp::input_status_e::stride_too_small);
  }

  TEST(RkmppInputLayout, DerivesProducerPlaneLayoutsWithoutTruncatingPlaneRatios) {
    const auto bgr = platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_BGR888, 5760, 5'760U * 1'080U);
    const auto nv12 = platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV420SP, 1920, 1'920U * 1'620U);
    const auto nv16 = platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV422SP, 1920, 1'920U * 2'160U);
    const auto nv24 = platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV444SP, 1920, 1'920U * 3'240U);
    ASSERT_TRUE(bgr.has_value());
    ASSERT_TRUE(nv12.has_value());
    ASSERT_TRUE(nv16.has_value());
    ASSERT_TRUE(nv24.has_value());
    EXPECT_EQ(bgr->vertical_stride, 1080U);
    EXPECT_EQ(nv12->vertical_stride, 1080U);
    EXPECT_EQ(nv16->vertical_stride, 1080U);
    EXPECT_EQ(nv24->vertical_stride, 1080U);

    EXPECT_FALSE(platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_BGR888, 5759, 5759U * 1080U));
    EXPECT_FALSE(platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV420SP, 1920, 1'920U * 1'620U - 1U));
    EXPECT_FALSE(platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV422SP, 1920, 1'920U * 2'160U - 1'920U));
    EXPECT_FALSE(platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV444SP, 1920, 1'920U * 3'240U - 1'920U));
  }

  TEST(RkmppInputLayout, RejectsUnsupportedFormatAndUndersizedAllocation) {
    auto layout = nv12_layout();
    layout.format = MPP_FMT_YUV420P;
    EXPECT_EQ(platf::rkmpp::validate_input_layout(layout), platf::rkmpp::input_status_e::unsupported_format);

    const auto valid_layout = nv12_layout();
    auto frame = input_frame(valid_layout, std::make_shared<int>(1));
    --frame.allocation_size;
    EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, valid_layout), platf::rkmpp::input_status_e::allocation_too_small);
  }

  TEST(RkmppInputFrame, RejectsMissingLifetimePinBadDescriptorAndLayoutMismatch) {
    const auto layout = nv12_layout();
    auto frame = input_frame(layout, {});
    EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, layout), platf::rkmpp::input_status_e::missing_holder);

    frame = input_frame(layout, std::make_shared<int>(1));
    frame.dma_buf_fd = -1;
    EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, layout), platf::rkmpp::input_status_e::invalid_dma_buf);

    frame = input_frame(layout, std::make_shared<int>(1));
    frame.layout.visible_width = 1280;
    EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, layout), platf::rkmpp::input_status_e::layout_mismatch);

    frame = input_frame(layout, std::make_shared<int>(1));
    frame.layout = *platf::rkmpp::make_input_layout_from_plane(1920, 1080, MPP_FMT_YUV420SP, 1984, 1984U * 1620U);
    EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, layout), platf::rkmpp::input_status_e::layout_mismatch);
  }

  TEST(RkmppInputFrame, PreservesLargeAllocationMetadataUntilMppAbiValidation) {
    const auto layout = nv12_layout();
    auto frame = input_frame(layout, std::make_shared<int>(1));
    frame.allocation_size = std::uint64_t {1} << 32U;
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
      EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, layout), platf::rkmpp::input_status_e::ok);
    } else {
      EXPECT_EQ(platf::rkmpp::validate_input_frame(frame, layout), platf::rkmpp::input_status_e::allocation_not_representable);
    }
  }

  TEST(RkmppInputCacheKey, DistinguishesReusedFileDescriptorsAcrossCaptureGenerations) {
    const platf::rkmpp::input_buffer_key_t first_generation {7, 2};
    const platf::rkmpp::input_buffer_key_t same_slot {7, 2};
    const platf::rkmpp::input_buffer_key_t recovered_generation {8, 2};
    const platf::rkmpp::input_buffer_key_t another_slot {7, 3};

    EXPECT_EQ(first_generation, same_slot);
    EXPECT_NE(first_generation, recovered_generation);
    EXPECT_NE(first_generation, another_slot);
  }

  TEST(RkmppInputLayout, ReportsConverterRequiredWithoutCreatingWrongEncoderConfiguration) {
    const auto input = nv12_layout();
    const platf::rkmpp::encoder_config_t mismatch {platf::rkmpp::codec_e::h264, input, 1280, 720};
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(mismatch), platf::rkmpp::encoder_config_status_e::converter_required);
  }

  TEST(RkmppEncoderConfig, RejectsEveryInvalidConfigurationStatus) {
    const auto input = nv12_layout();
    auto config = platf::rkmpp::encoder_config_t {platf::rkmpp::codec_e::h264, input, 1920, 1080};

    config.codec = static_cast<platf::rkmpp::codec_e>(99);
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_codec);

    config = {platf::rkmpp::codec_e::h264, input, 1920, 1080};
    config.input_layout.horizontal_stride = 1919;
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_input);

    for (const auto &coded_size : {std::pair {0U, 1080U}, std::pair {1921U, 1080U}, std::pair {1920U, 1079U}, std::pair {32'770U, 1080U}}) {
      config = {platf::rkmpp::codec_e::h264, input, coded_size.first, coded_size.second};
      EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_coded_size);
    }

    config = {platf::rkmpp::codec_e::h264, input, 1920, 1080};
    config.fps_num = 0;
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_rate_control);
    config.fps_num = 60;
    config.fps_den = 0;
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_rate_control);
    config.fps_den = 1;
    config.gop = 0;
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_rate_control);
    config.gop = 60;
    config.bitrate = 0;
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_rate_control);
    config.bitrate = std::numeric_limits<std::uint32_t>::max();
    EXPECT_EQ(platf::rkmpp::validate_encoder_config(config), platf::rkmpp::encoder_config_status_e::invalid_rate_control);
  }

  TEST(RkmppOutputIdr, SkipsNonIntraPacketsAndVerifiesIntraPackets) {
    const std::vector<std::uint8_t> h264_idr {0, 0, 1, 7, 0, 0, 1, 8, 0, 0, 1, 5};
    const std::vector<std::uint8_t> h265_idr {0, 0, 1, 64, 0, 0, 1, 66, 0, 0, 1, 68, 0, 0, 1, 38};
    const std::vector<std::uint8_t> non_idr {0, 0, 1, 1};

    EXPECT_FALSE(platf::rkmpp::detail::output_is_idr(false, h264_idr.data(), h264_idr.size(), platf::rkmpp::codec_e::h264));
    EXPECT_TRUE(platf::rkmpp::detail::output_is_idr(true, h264_idr.data(), h264_idr.size(), platf::rkmpp::codec_e::h264));
    EXPECT_TRUE(platf::rkmpp::detail::output_is_idr(true, h265_idr.data(), h265_idr.size(), platf::rkmpp::codec_e::h265));
    EXPECT_FALSE(platf::rkmpp::detail::output_is_idr(true, non_idr.data(), non_idr.size(), platf::rkmpp::codec_e::h264));
  }

  TEST(RkmppOsdRegion, ValidatesMacroblockAlignmentBoundsAndStorage) {
    std::array<std::uint8_t, 256U * 64U> pixels {};
    auto region = platf::rkmpp::osd_region_t {0, 0, 256, 64, pixels};
    EXPECT_EQ(platf::rkmpp::validate_osd_region(region, 1920, 1080), platf::rkmpp::osd_region_status_e::ok);

    region.x = 1;
    EXPECT_EQ(platf::rkmpp::validate_osd_region(region, 1920, 1080), platf::rkmpp::osd_region_status_e::unaligned);
    region.x = 1792;
    EXPECT_EQ(platf::rkmpp::validate_osd_region(region, 1920, 1080), platf::rkmpp::osd_region_status_e::outside_frame);
    region = {0, 0, 0, 64, {}};
    EXPECT_EQ(platf::rkmpp::validate_osd_region(region, 1920, 1080), platf::rkmpp::osd_region_status_e::empty);
    region = {0, 0, 256, 64, std::span<const std::uint8_t> {pixels}.first(pixels.size() - 1U)};
    EXPECT_EQ(platf::rkmpp::validate_osd_region(region, 1920, 1080), platf::rkmpp::osd_region_status_e::size_mismatch);
  }

  TEST(RkmppOsdBitmap, RendersWaitingAndSnapshotTextIntoFixedStorage) {
    platf::rkmpp::frame_profile_overlay_bitmap_t bitmap;
    EXPECT_EQ(bitmap.pixels().size(), static_cast<std::size_t>(platf::rkmpp::frame_profile_overlay_bitmap_t::width) * platf::rkmpp::frame_profile_overlay_bitmap_t::height);
    EXPECT_GT(std::count(bitmap.pixels().begin(), bitmap.pixels().end(), std::uint8_t {0}), 100);
    EXPECT_GT(std::count(bitmap.pixels().begin(), bitmap.pixels().end(), std::uint8_t {7}), 0);

    bitmap.render_status("XBOX REMOTE PLAY", "AUTHENTICATING");
    EXPECT_GT(std::count(bitmap.pixels().begin(), bitmap.pixels().end(), std::uint8_t {7}), 0);
    EXPECT_GT(std::count(bitmap.pixels().begin(), bitmap.pixels().end(), std::uint8_t {5}), 0);

    video::frame_profile_snapshot_t snapshot;
    snapshot.captured_frames = 300;
    snapshot.hdmirx_width = 1920;
    snapshot.hdmirx_height = 1080;
    snapshot.moonlight_width = 1920;
    snapshot.moonlight_height = 1080;
    for (auto &metric : snapshot.metrics) {
      metric.count = 300;
      metric.p50_us = 1'000;
      metric.p95_us = 2'000;
      metric.p99_us = 3'000;
    }
    bitmap.render(snapshot);
    EXPECT_GT(std::count(bitmap.pixels().begin(), bitmap.pixels().end(), std::uint8_t {7}), 100);
    EXPECT_GT(std::count(bitmap.pixels().begin(), bitmap.pixels().end(), std::uint8_t {5}), 0);
  }

  TEST(RkmppInputFrame, HolderKeepsProducerAliveUntilSynchronousConsumerDropsItsCopy) {
    bool destroyed = false;
    auto holder = platf::rkmpp::input_holder_t(new int(1), [&destroyed](void *value) {
      delete static_cast<int *>(value);
      destroyed = true;
    });
    const auto layout = nv12_layout();
    auto frame = input_frame(layout, holder);
    auto synchronous_consumer = frame.holder;
    frame.holder.reset();
    holder.reset();
    EXPECT_FALSE(destroyed);
    synchronous_consumer.reset();
    EXPECT_TRUE(destroyed);
  }

  /** @brief An unencoded input reset returns its sole producer lease for immediate reuse. */
  TEST(RkmppInputFrame, ResetReleasesUnencodedProducerLeaseAndClearsMetadata) {
    bool released = false;
    auto holder = platf::rkmpp::input_holder_t(new int(1), [&released](void *value) {
      delete static_cast<int *>(value);
      released = true;
    });
    auto frame = input_frame(nv12_layout(), std::move(holder));
    video::frame_profile_t profile;
    frame.profile = &profile;
    frame.cache_key = platf::rkmpp::input_buffer_key_t {7, 1};

    frame.reset();

    EXPECT_TRUE(released);
    EXPECT_EQ(frame.layout, platf::rkmpp::input_layout_t {});
    EXPECT_EQ(frame.dma_buf_fd, -1);
    EXPECT_EQ(frame.allocation_size, 0U);
    EXPECT_EQ(frame.pts, 0);
    EXPECT_FALSE(frame.holder);
    EXPECT_EQ(frame.profile, nullptr);
    EXPECT_FALSE(frame.cache_key);
  }
}  // namespace

#endif
