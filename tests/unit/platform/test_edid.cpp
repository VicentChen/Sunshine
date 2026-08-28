/**
 * @file tests/unit/platform/test_edid.cpp
 * @brief Unit tests for EDID data model, parser, ioctl abstraction, and restore guard.
 *
 * All tests use mock/fake ioctl backends.  No root access or /dev/video0
 * interaction is required.
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <src/platform/linux/edid.h>
#include <src/platform/hdmirx_policy.h>

namespace {

  using namespace platf::edid;
  using platf::hdmirx::hdmi_mode_t;
  using platf::hdmirx::resolution_t;

  // =========================================================================
  // Mock ioctl backend
  // =========================================================================

  /**
   * @brief Configurable mock ioctl backend for EDID tests.
   *
   * Allows tests to inject specific behaviors for get_edid and set_edid.
   */
  class mock_ioctl_backend_t final: public ioctl_backend_t {
  public:
    /// @brief Callback type for get_edid.
    using get_edid_fn = std::function<edid_result_t<std::uint32_t>(
      std::uint32_t, std::uint32_t, std::uint32_t, std::span<std::uint8_t>)>;
    /// @brief Callback type for set_edid.
    using set_edid_fn = std::function<edid_result_t<std::uint32_t>(
      std::uint32_t, std::uint32_t, std::uint32_t, std::span<const std::uint8_t>)>;

    get_edid_fn on_get_edid;
    set_edid_fn on_set_edid;

    edid_result_t<std::uint32_t> get_edid(
      std::uint32_t pad,
      std::uint32_t start_block,
      std::uint32_t block_count,
      std::span<std::uint8_t> buffer) override {
      if (on_get_edid) return on_get_edid(pad, start_block, block_count, buffer);
      return std::unexpected(edid_error_t{
        error_category_e::not_supported, ENOTTY, "get_edid not configured"});
    }

    edid_result_t<std::uint32_t> set_edid(
      std::uint32_t pad,
      std::uint32_t start_block,
      std::uint32_t block_count,
      std::span<const std::uint8_t> data) override {
      if (on_set_edid) return on_set_edid(pad, start_block, block_count, data);
      return std::unexpected(edid_error_t{
        error_category_e::not_supported, ENOTTY, "set_edid not configured"});
    }
  };

  /**
   * @brief Create a mock that stores and returns EDID data.
   *
   * @param initial_edid The initial EDID data the device holds.
   * @return A configured mock backend.
   */
  mock_ioctl_backend_t make_read_write_mock(std::vector<std::uint8_t> initial_edid) {
    auto stored = std::make_shared<std::vector<std::uint8_t>>(std::move(initial_edid));
    mock_ioctl_backend_t mock;

    mock.on_get_edid = [stored](
      std::uint32_t /*pad*/, std::uint32_t start_block,
      std::uint32_t block_count, std::span<std::uint8_t> buffer)
        -> edid_result_t<std::uint32_t> {
      if (stored->empty()) {
        return std::unexpected(edid_error_t{
          error_category_e::no_data, ENODATA, "no EDID data"});
      }
      const auto total_blocks = static_cast<std::uint32_t>(stored->size() / k_edid_block_size);
      if (start_block >= total_blocks) {
        return std::unexpected(edid_error_t{
          error_category_e::invalid_argument, EINVAL, "start_block out of range"});
      }
      const auto available = total_blocks - start_block;
      const auto to_read = std::min(block_count, available);
      const auto byte_offset = static_cast<std::size_t>(start_block) * k_edid_block_size;
      const auto byte_count = static_cast<std::size_t>(to_read) * k_edid_block_size;
      if (buffer.size() < byte_count) {
        return std::unexpected(edid_error_t{
          error_category_e::invalid_argument, EINVAL, "buffer too small"});
      }
      std::memcpy(buffer.data(), stored->data() + byte_offset, byte_count);
      return to_read;
    };

    mock.on_set_edid = [stored](
      std::uint32_t /*pad*/, std::uint32_t start_block,
      std::uint32_t block_count, std::span<const std::uint8_t> data)
        -> edid_result_t<std::uint32_t> {
      const auto byte_count = static_cast<std::size_t>(block_count) * k_edid_block_size;
      if (data.size() < byte_count) {
        return std::unexpected(edid_error_t{
          error_category_e::invalid_argument, EINVAL, "data too small"});
      }
      const auto byte_offset = static_cast<std::size_t>(start_block) * k_edid_block_size;
      const auto needed = byte_offset + byte_count;
      if (stored->size() < needed) {
        stored->resize(needed, 0);
      }
      std::memcpy(stored->data() + byte_offset, data.data(), byte_count);
      return block_count;
    };

    return mock;
  }

  void set_extension_count(std::vector<std::uint8_t> &base, std::uint8_t count) {
    ASSERT_EQ(base.size(), k_edid_block_size);
    base[126] = count;
    base[127] = 0;
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < 127; ++i) {
      sum += base[i];
    }
    base[127] = static_cast<std::uint8_t>(0U - sum);
  }

  // =========================================================================
  // Error classification tests
  // =========================================================================

  TEST(EdidErrorClassification, ClassifiesKnownErrnoValues) {
    EXPECT_EQ(classify_errno(ENOTTY), error_category_e::not_supported);
    EXPECT_EQ(classify_errno(EINVAL), error_category_e::invalid_argument);
    EXPECT_EQ(classify_errno(ENODATA), error_category_e::no_data);
    EXPECT_EQ(classify_errno(E2BIG), error_category_e::too_large);
    EXPECT_EQ(classify_errno(EACCES), error_category_e::permission_denied);
    EXPECT_EQ(classify_errno(EPERM), error_category_e::permission_denied);
    EXPECT_EQ(classify_errno(ENODEV), error_category_e::device_gone);
    EXPECT_EQ(classify_errno(ENXIO), error_category_e::device_gone);
  }

  TEST(EdidErrorClassification, FallsBackToIoError) {
    EXPECT_EQ(classify_errno(EIO), error_category_e::io_error);
    EXPECT_EQ(classify_errno(EBUSY), error_category_e::io_error);
    EXPECT_EQ(classify_errno(999), error_category_e::io_error);
  }

  TEST(EdidErrorClassification, AllCategoryNamesNonEmpty) {
    EXPECT_STRNE(error_category_name(error_category_e::not_supported), "");
    EXPECT_STRNE(error_category_name(error_category_e::invalid_argument), "");
    EXPECT_STRNE(error_category_name(error_category_e::no_data), "");
    EXPECT_STRNE(error_category_name(error_category_e::too_large), "");
    EXPECT_STRNE(error_category_name(error_category_e::permission_denied), "");
    EXPECT_STRNE(error_category_name(error_category_e::device_gone), "");
    EXPECT_STRNE(error_category_name(error_category_e::io_error), "");
  }

  // =========================================================================
  // Checksum tests
  // =========================================================================

  TEST(EdidChecksum, ValidBlockHasZeroChecksum) {
    auto edid = make_1080p_edid();
    std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
    EXPECT_EQ(compute_block_checksum(block), 0);
    EXPECT_TRUE(validate_block_checksum(block));
  }

  TEST(EdidChecksum, BadChecksumDetected) {
    auto edid = make_bad_checksum_edid();
    std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
    EXPECT_NE(compute_block_checksum(block), 0);
    EXPECT_FALSE(validate_block_checksum(block));
  }

  TEST(EdidChecksum, AllFixturesHaveValidChecksums) {
    auto check = [](const std::vector<std::uint8_t> &edid, const char *name) {
      ASSERT_EQ(edid.size(), k_edid_block_size) << name;
      std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
      EXPECT_TRUE(validate_block_checksum(block)) << name;
    };
    check(make_720p_edid(), "720p");
    check(make_1080p_edid(), "1080p");
    check(make_1440p_edid(), "1440p");
    check(make_2160p_edid(), "2160p");
  }

  TEST(EdidChecksum, ValidateEdidChecksumsMultiBlock) {
    auto edid1 = make_1080p_edid();
    auto edid2 = make_720p_edid();
    std::vector<std::uint8_t> multi;
    multi.insert(multi.end(), edid1.begin(), edid1.end());
    multi.insert(multi.end(), edid2.begin(), edid2.end());
    EXPECT_TRUE(validate_edid_checksums(multi));
  }

  TEST(EdidChecksum, ValidateEdidChecksumsRejectsTruncated) {
    auto edid = make_truncated_edid();
    EXPECT_FALSE(validate_edid_checksums(edid));
  }

  TEST(EdidChecksum, ValidateEdidChecksumsRejectsEmpty) {
    EXPECT_FALSE(validate_edid_checksums(std::span<const std::uint8_t>{}));
  }

  TEST(EdidChecksum, ValidateEdidChecksumsRejectsBadBlock) {
    auto edid = make_bad_checksum_edid();
    EXPECT_FALSE(validate_edid_checksums(edid));
  }

  TEST(EdidAudioExtension, AddsStereoLpcmCtaExtension) {
    const auto edid = with_cta_lpcm_audio_extension(make_1080p_edid(), 16);

    ASSERT_EQ(edid.size(), k_edid_block_size * 2U);
    EXPECT_TRUE(validate_edid_checksums(edid));
    EXPECT_EQ(edid[126], 1U);
    EXPECT_EQ(edid[k_edid_block_size], 0x02U);
    EXPECT_EQ(edid[k_edid_block_size + 1U], 0x03U);
    EXPECT_EQ(edid[k_edid_block_size + 3U], 0x40U);
    EXPECT_EQ(edid[k_edid_block_size + 2U], 20U);
    EXPECT_EQ(edid[k_edid_block_size + 4U], 0x41U);
    EXPECT_EQ(edid[k_edid_block_size + 5U], 0x90U);
    EXPECT_EQ(edid[k_edid_block_size + 6U], 0x23U);
    EXPECT_EQ(edid[k_edid_block_size + 7U], 0x09U);
    EXPECT_EQ(edid[k_edid_block_size + 8U], 0x07U);
    EXPECT_EQ(edid[k_edid_block_size + 9U], 0x07U);
    EXPECT_EQ(edid[k_edid_block_size + 10U], 0x83U);
    EXPECT_EQ(edid[k_edid_block_size + 11U], 0x01U);
    EXPECT_EQ(edid[k_edid_block_size + 14U], 0x65U);
    EXPECT_EQ(edid[k_edid_block_size + 15U], 0x03U);
    EXPECT_EQ(edid[k_edid_block_size + 16U], 0x0cU);
    EXPECT_EQ(edid[k_edid_block_size + 17U], 0x00U);
  }

  TEST(EdidAudioExtension, RejectsInvalidBaseBlock) {
    EXPECT_TRUE(with_cta_lpcm_audio_extension(std::span<const std::uint8_t>{}, 16).empty());
    EXPECT_TRUE(with_cta_lpcm_audio_extension(make_bad_checksum_edid(), 16).empty());
  }

  // =========================================================================
  // Timing descriptor parsing tests
  // =========================================================================

  TEST(EdidTimingParsing, Parse1080pTimingDescriptor) {
    auto edid = make_1080p_edid();
    std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
    auto timings = parse_base_block_timings(block);
    ASSERT_EQ(timings.size(), 1U);
    EXPECT_EQ(timings[0].h_active, 1920);
    EXPECT_EQ(timings[0].v_active, 1080);
    EXPECT_EQ(timings[0].pixel_clock_khz, 148500U);
  }

  TEST(EdidTimingParsing, Parse720pTimingDescriptor) {
    auto edid = make_720p_edid();
    std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
    auto timings = parse_base_block_timings(block);
    ASSERT_EQ(timings.size(), 1U);
    EXPECT_EQ(timings[0].h_active, 1280);
    EXPECT_EQ(timings[0].v_active, 720);
    EXPECT_EQ(timings[0].pixel_clock_khz, 74250U);
  }

  TEST(EdidTimingParsing, Parse1440pTimingDescriptor) {
    auto edid = make_1440p_edid();
    std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
    auto timings = parse_base_block_timings(block);
    ASSERT_EQ(timings.size(), 1U);
    EXPECT_EQ(timings[0].h_active, 2560);
    EXPECT_EQ(timings[0].v_active, 1440);
  }

  TEST(EdidTimingParsing, Parse2160pTimingDescriptor) {
    auto edid = make_2160p_edid();
    std::span<const std::uint8_t, k_edid_block_size> block{edid.data(), k_edid_block_size};
    auto timings = parse_base_block_timings(block);
    ASSERT_EQ(timings.size(), 1U);
    EXPECT_EQ(timings[0].h_active, 3840);
    EXPECT_EQ(timings[0].v_active, 2160);
  }

  TEST(EdidTimingParsing, ZeroPixelClockIsMonitorDescriptor) {
    std::array<std::uint8_t, 18> desc{};
    // All zeros -> pixel clock is 0 -> monitor descriptor.
    std::span<const std::uint8_t, 18> s{desc.data(), 18};
    EXPECT_FALSE(parse_timing_descriptor(s).has_value());
  }

  // =========================================================================
  // Timing to HDMI mode conversion
  // =========================================================================

  TEST(EdidTimingToMode, Converts1080pCorrectly) {
    timing_descriptor_t t;
    t.pixel_clock_khz = 148500;
    t.h_active = 1920;
    t.h_blanking = 280;
    t.v_active = 1080;
    t.v_blanking = 45;
    auto mode = timing_to_hdmi_mode(t);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(mode->resolution.width, 1920U);
    EXPECT_EQ(mode->resolution.height, 1080U);
    EXPECT_TRUE(mode->verified);
    // Refresh rate: 148500000 / (2200 * 1125) = 60.0 Hz
    // numerator = 148500000, denominator = 2475000
    EXPECT_GT(mode->refresh_rate.numerator, 0U);
    EXPECT_GT(mode->refresh_rate.denominator, 0U);
    // Check ~60 Hz
    double hz = static_cast<double>(mode->refresh_rate.numerator) /
                static_cast<double>(mode->refresh_rate.denominator);
    EXPECT_NEAR(hz, 60.0, 0.1);
  }

  TEST(EdidTimingToMode, RejectsZeroDimensions) {
    timing_descriptor_t t;
    t.pixel_clock_khz = 148500;
    t.h_active = 0;
    t.v_active = 1080;
    EXPECT_FALSE(timing_to_hdmi_mode(t).has_value());

    t.h_active = 1920;
    t.v_active = 0;
    EXPECT_FALSE(timing_to_hdmi_mode(t).has_value());
  }

  TEST(EdidTimingToMode, RejectsZeroPixelClock) {
    timing_descriptor_t t;
    t.pixel_clock_khz = 0;
    t.h_active = 1920;
    t.v_active = 1080;
    EXPECT_FALSE(timing_to_hdmi_mode(t).has_value());
  }

  // =========================================================================
  // EDID mode parsing (end-to-end)
  // =========================================================================

  TEST(EdidModeParsing, ParsesValidEdidModes) {
    auto edid = make_1080p_edid();
    auto modes = parse_edid_modes(edid);
    ASSERT_EQ(modes.size(), 1U);
    EXPECT_EQ(modes[0].resolution.width, 1920U);
    EXPECT_EQ(modes[0].resolution.height, 1080U);
    EXPECT_TRUE(modes[0].verified);
  }

  TEST(EdidModeParsing, RejectsEdidWithBadChecksum) {
    auto edid = make_bad_checksum_edid();
    auto modes = parse_edid_modes(edid);
    EXPECT_TRUE(modes.empty());
  }

  TEST(EdidModeParsing, RejectsTruncatedEdid) {
    auto edid = make_truncated_edid();
    auto modes = parse_edid_modes(edid);
    EXPECT_TRUE(modes.empty());
  }

  TEST(EdidModeParsing, RejectsEmptyEdid) {
    auto modes = parse_edid_modes(std::span<const std::uint8_t>{});
    EXPECT_TRUE(modes.empty());
  }

  TEST(EdidModeParsing, AllFixtureModesCorrect) {
    struct fixture_t {
      std::vector<std::uint8_t> edid;
      std::uint32_t expected_w;
      std::uint32_t expected_h;
    };
    std::vector<fixture_t> fixtures{
      {make_720p_edid(), 1280, 720},
      {make_1080p_edid(), 1920, 1080},
      {make_1440p_edid(), 2560, 1440},
      {make_2160p_edid(), 3840, 2160},
    };
    for (const auto &f : fixtures) {
      auto modes = parse_edid_modes(f.edid);
      ASSERT_EQ(modes.size(), 1U) << f.expected_w << "x" << f.expected_h;
      EXPECT_EQ(modes[0].resolution.width, f.expected_w);
      EXPECT_EQ(modes[0].resolution.height, f.expected_h);
    }
  }

  // =========================================================================
  // Fixture modes passed to Stage 1 selector
  // =========================================================================

  TEST(EdidFixtureSelector, FixtureModesPassToSelectorCorrectly) {
    // Build modes from all fixtures.
    std::vector<hdmi_mode_t> all_modes;
    for (const auto &edid : {make_720p_edid(), make_1080p_edid(),
                             make_1440p_edid(), make_2160p_edid()}) {
      auto modes = parse_edid_modes(edid);
      all_modes.insert(all_modes.end(), modes.begin(), modes.end());
    }
    ASSERT_EQ(all_modes.size(), 4U);

    // Requesting 1080p should select the 1080p mode.
    auto selected = platf::hdmirx::select_hdmi_mode(all_modes, {1920, 1080});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t{1920, 1080}));

    // Requesting 720p should select the 720p mode.
    selected = platf::hdmirx::select_hdmi_mode(all_modes, {1280, 720});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t{1280, 720}));

    // Requesting 4K should select the 4K mode.
    selected = platf::hdmirx::select_hdmi_mode(all_modes, {3840, 2160});
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->resolution, (resolution_t{3840, 2160}));
  }

  // =========================================================================
  // Multi-block EDID read and complete save
  // =========================================================================

  TEST(EdidIoctlRead, ReadsMultiBlockEdid) {
    auto block1 = make_1080p_edid();
    auto block2 = make_720p_edid();
    set_extension_count(block1, 1);
    std::vector<std::uint8_t> multi_block;
    multi_block.insert(multi_block.end(), block1.begin(), block1.end());
    multi_block.insert(multi_block.end(), block2.begin(), block2.end());

    auto mock = make_read_write_mock(multi_block);
    auto result = read_edid(mock);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->size(), 256U);
    EXPECT_EQ(*result, multi_block);
  }

  TEST(EdidIoctlRead, ReadFailsWhenNotSupported) {
    mock_ioctl_backend_t mock;
    // Default mock returns ENOTTY.
    auto result = read_edid(mock);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::not_supported);
  }

  TEST(EdidIoctlRead, ReadFailsWhenNoData) {
    mock_ioctl_backend_t mock;
    mock.on_get_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return std::unexpected(edid_error_t{
        error_category_e::no_data, ENODATA, "no data"});
    };
    auto result = read_edid(mock);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::no_data);
  }

  TEST(EdidIoctlRead, ReadFailsOnZeroBlocks) {
    mock_ioctl_backend_t mock;
    mock.on_get_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return 0U;
    };
    auto result = read_edid(mock);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::no_data);
  }

  // =========================================================================
  // Write tests
  // =========================================================================

  TEST(EdidIoctlWrite, WritesValidEdid) {
    auto edid = make_1080p_edid();
    auto mock = make_read_write_mock(edid);
    auto result = write_edid(mock, 0, edid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1U);
  }

  TEST(EdidIoctlWrite, WriteRejectsEmptyData) {
    mock_ioctl_backend_t mock;
    auto result = write_edid(mock, 0, std::span<const std::uint8_t>{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::invalid_argument);
  }

  TEST(EdidIoctlWrite, WriteRejectsNonMultipleOf128) {
    mock_ioctl_backend_t mock;
    std::vector<std::uint8_t> bad(100, 0);
    auto result = write_edid(mock, 0, bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::invalid_argument);
  }

  TEST(EdidIoctlWrite, WriteRejectsTooManyBlocks) {
    mock_ioctl_backend_t mock;
    std::vector<std::uint8_t> too_large(k_edid_block_size * (k_max_edid_blocks + 1), 0);
    auto result = write_edid(mock, 0, too_large);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::too_large);
  }

  TEST(EdidIoctlWrite, PartialWriteReportsError) {
    mock_ioctl_backend_t mock;
    mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return std::unexpected(edid_error_t{
        error_category_e::io_error, EIO, "partial write"});
    };
    auto edid = make_1080p_edid();
    auto result = write_edid(mock, 0, edid);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::io_error);
  }

  TEST(EdidIoctlWrite, ShortSuccessfulWriteIsRejected) {
    mock_ioctl_backend_t mock;
    mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return 0U;
    };
    auto result = write_edid(mock, 0, make_1080p_edid());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::io_error);
  }

  // =========================================================================
  // Capability classification
  // =========================================================================

  TEST(EdidCapability, NotSupportedDevice) {
    mock_ioctl_backend_t mock;
    // Default returns ENOTTY for both get and set.
    auto cap = probe_capabilities(mock);
    EXPECT_FALSE(cap.readable);
    EXPECT_FALSE(cap.writable);
    EXPECT_FALSE(cap.restorable);
    EXPECT_FALSE(cap.allows_negotiation());
  }

  TEST(EdidCapability, ReadOnlyDevice) {
    mock_ioctl_backend_t mock;
    auto edid = make_1080p_edid();
    mock.on_get_edid = [&edid](auto, auto, auto, std::span<std::uint8_t> buf)
        -> edid_result_t<std::uint32_t> {
      std::memcpy(buf.data(), edid.data(), k_edid_block_size);
      return 1U;
    };
    // set_edid stays default (ENOTTY).
    auto cap = probe_capabilities(mock);
    EXPECT_TRUE(cap.readable);
    EXPECT_FALSE(cap.writable);
    EXPECT_FALSE(cap.restorable);
    EXPECT_FALSE(cap.allows_negotiation());
  }

  TEST(EdidCapability, WriteOnlyDevice) {
    mock_ioctl_backend_t mock;
    // get_edid returns ENOTTY.
    mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return 1U;
    };
    auto cap = probe_capabilities(mock);
    EXPECT_FALSE(cap.readable);
    EXPECT_FALSE(cap.writable);
    EXPECT_FALSE(cap.restorable);
    EXPECT_FALSE(cap.allows_negotiation());
  }

  TEST(EdidCapability, FullCapabilityDevice) {
    auto edid = make_1080p_edid();
    auto mock = make_read_write_mock(edid);
    auto cap = probe_capabilities(mock);
    EXPECT_TRUE(cap.readable);
    EXPECT_FALSE(cap.writable);
    EXPECT_FALSE(cap.restorable);
    EXPECT_FALSE(cap.allows_negotiation());
  }

  TEST(EdidCapability, PermissionErrorDevice) {
    mock_ioctl_backend_t mock;
    auto edid = make_1080p_edid();
    mock.on_get_edid = [&edid](auto, auto, auto, std::span<std::uint8_t> buf)
        -> edid_result_t<std::uint32_t> {
      std::memcpy(buf.data(), edid.data(), k_edid_block_size);
      return 1U;
    };
    mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return std::unexpected(edid_error_t{
        error_category_e::permission_denied, EACCES, "permission denied"});
    };
    auto cap = probe_capabilities(mock);
    EXPECT_TRUE(cap.readable);
    EXPECT_FALSE(cap.writable);
    EXPECT_FALSE(cap.restorable);
    EXPECT_FALSE(cap.allows_negotiation());
  }

  TEST(EdidCapability, NoDataDoesNotPermitWrites) {
    mock_ioctl_backend_t mock;
    mock.on_get_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return std::unexpected(edid_error_t{
        error_category_e::no_data, ENODATA, "no data present"});
    };
    mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return 1U;
    };
    auto cap = probe_capabilities(mock);
    EXPECT_FALSE(cap.readable);
    EXPECT_FALSE(cap.writable);
    EXPECT_FALSE(cap.restorable);
    EXPECT_FALSE(cap.allows_negotiation());
  }

  TEST(EdidCapability, ProbeNeverWritesFixtureOrOriginal) {
    auto edid = make_1080p_edid();
    auto mock = make_read_write_mock(edid);
    std::uint32_t writes = 0;
    mock.on_set_edid = [&writes](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      ++writes;
      return 1U;
    };
    const auto cap = probe_capabilities(mock);
    EXPECT_TRUE(cap.readable);
    EXPECT_EQ(writes, 0U);
  }

  TEST(EdidIoctlRead, RejectsIncompleteMultiBlockBackup) {
    mock_ioctl_backend_t mock;
    auto base = make_1080p_edid();
    set_extension_count(base, 1);
    mock.on_get_edid = [&base](auto, auto, std::uint32_t blocks, std::span<std::uint8_t> buffer)
        -> edid_result_t<std::uint32_t> {
      std::memcpy(buffer.data(), base.data(), k_edid_block_size);
      return blocks == 1 ? 1U : 1U;
    };
    const auto result = read_edid(mock);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::io_error);
  }

  // =========================================================================
  // Restore guard tests
  // =========================================================================

  TEST(EdidRestoreGuard, NormalEndRestoresEdid) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);

    std::vector<std::string> log_msgs;
    auto logger = [&log_msgs](const std::string &msg) { log_msgs.push_back(msg); };

    {
      edid_restore_guard_t guard(mock, 0, logger);
      ASSERT_TRUE(guard.is_armed());
      EXPECT_EQ(guard.saved_edid(), original);

      // Write a different EDID.
      auto new_edid = make_720p_edid();
      auto result = write_edid(mock, 0, new_edid);
      ASSERT_TRUE(result.has_value());
    }
    // Guard destroyed; should have restored the original.
    auto result = read_edid(mock);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, original);
    // Check that at least one log message mentions restore.
    bool found_restore_log = false;
    for (const auto &msg : log_msgs) {
      if (msg.find("restore") != std::string::npos ||
          msg.find("Restore") != std::string::npos) {
        found_restore_log = true;
      }
    }
    EXPECT_TRUE(found_restore_log);
  }

  TEST(EdidRestoreGuard, MidInitFailureDisarms) {
    mock_ioctl_backend_t mock;
    // get_edid fails -> guard should be disarmed.
    std::vector<std::string> log_msgs;
    auto logger = [&log_msgs](const std::string &msg) { log_msgs.push_back(msg); };

    edid_restore_guard_t guard(mock, 0, logger);
    EXPECT_FALSE(guard.is_armed());
    EXPECT_TRUE(guard.saved_edid().empty());

    // Destroy should be a no-op.
    EXPECT_TRUE(guard.restore());
  }

  TEST(EdidRestoreGuard, ExplicitRestoreDisarms) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);

    edid_restore_guard_t guard(mock, 0);
    ASSERT_TRUE(guard.is_armed());

    EXPECT_TRUE(guard.restore());
    EXPECT_FALSE(guard.is_armed());

    // Second restore is a no-op.
    EXPECT_TRUE(guard.restore());
  }

  TEST(EdidRestoreGuard, RepeatedRestoreIsIdempotent) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);

    edid_restore_guard_t guard(mock, 0);
    ASSERT_TRUE(guard.is_armed());

    EXPECT_TRUE(guard.restore());
    EXPECT_TRUE(guard.restore());
    EXPECT_TRUE(guard.restore());
    EXPECT_FALSE(guard.is_armed());
  }

  TEST(EdidRestoreGuard, RestoreFailureLogsButDoesNotThrow) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);

    std::vector<std::string> log_msgs;
    auto logger = [&log_msgs](const std::string &msg) { log_msgs.push_back(msg); };

    edid_restore_guard_t guard(mock, 0, logger);
    ASSERT_TRUE(guard.is_armed());

    // Break the write path.
    mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
      return std::unexpected(edid_error_t{
        error_category_e::device_gone, ENODEV, "device disappeared"});
    };

    // restore() should return false but not throw.
    EXPECT_FALSE(guard.restore());
    EXPECT_TRUE(guard.is_armed());  // Still armed because restore failed.

    // Check error was logged.
    bool found_error_log = false;
    for (const auto &msg : log_msgs) {
      if (msg.find("failed") != std::string::npos ||
          msg.find("device") != std::string::npos) {
        found_error_log = true;
      }
    }
    EXPECT_TRUE(found_error_log);
  }

  TEST(EdidRestoreGuard, DestructorDoesNotThrowOnFailure) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);

    {
      edid_restore_guard_t guard(mock, 0);
      ASSERT_TRUE(guard.is_armed());

      // Break the write path.
      mock.on_set_edid = [](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
        return std::unexpected(edid_error_t{
          error_category_e::device_gone, ENODEV, "device gone"});
      };
      // Destructor runs here; must not throw.
    }
    SUCCEED();  // If we get here, destructor did not throw.
  }

  TEST(EdidRestoreGuard, MoveTransfersOwnership) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);

    auto guard1 = std::make_unique<edid_restore_guard_t>(mock, 0);
    ASSERT_TRUE(guard1->is_armed());

    // Move to a new guard.
    edid_restore_guard_t guard2(std::move(*guard1));
    EXPECT_FALSE(guard1->is_armed());
    EXPECT_TRUE(guard2.is_armed());
    EXPECT_EQ(guard2.saved_edid(), original);

    // Now destroy guard1; should be no-op.
    guard1.reset();

    // Write something different.
    auto new_edid = make_720p_edid();
    write_edid(mock, 0, new_edid);

    // Guard2 should restore on destruction.
    guard2.restore();
    auto result = read_edid(mock);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, original);
  }

  TEST(EdidRestoreGuard, ExceptionUnwindRestores) {
    auto original = make_1080p_edid();
    auto mock = make_read_write_mock(original);
    bool restored = false;

    try {
      edid_restore_guard_t guard(mock, 0, [&](const std::string &msg) {
        if (msg.find("restore succeeded") != std::string::npos ||
            msg.find("Restore") != std::string::npos) {
          restored = true;
        }
      });
      ASSERT_TRUE(guard.is_armed());

      // Write different EDID, then throw.
      auto new_edid = make_720p_edid();
      write_edid(mock, 0, new_edid);

      throw std::runtime_error("simulated exception");
    } catch (const std::runtime_error &) {
      // Guard should have restored in its destructor.
    }

    // Verify the original was restored.
    auto result = read_edid(mock);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, original);
  }

  // =========================================================================
  // Partial read/write and wrong block count tests
  // =========================================================================

  TEST(EdidPartialOps, PartialReadReturnsAvailableBlocks) {
    auto edid = make_1080p_edid();
    auto mock = make_read_write_mock(edid);

    // Request 4 blocks from a 1-block device: should get 1.
    auto result = read_edid(mock);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), k_edid_block_size);
  }

  TEST(EdidPartialOps, WrongBlockCountOnRead) {
    mock_ioctl_backend_t mock;
    mock.on_get_edid = [](auto, auto start, auto, auto)
        -> edid_result_t<std::uint32_t> {
      if (start > 0) {
        return std::unexpected(edid_error_t{
          error_category_e::invalid_argument, EINVAL, "bad start block"});
      }
      return std::unexpected(edid_error_t{
        error_category_e::too_large, E2BIG, "too many blocks"});
    };
    auto result = read_edid(mock);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().category, error_category_e::too_large);
  }

  // =========================================================================
  // Constants validation
  // =========================================================================

  TEST(EdidConstants, BlockSizeIs128) {
    EXPECT_EQ(k_edid_block_size, 128U);
  }

  TEST(EdidConstants, MaxBlocksReasonable) {
    EXPECT_GE(k_max_edid_blocks, 2U);
    EXPECT_LE(k_max_edid_blocks, 256U);
  }

  TEST(EdidConstants, MaxSizeConsistent) {
    EXPECT_EQ(k_max_edid_size, k_edid_block_size * k_max_edid_blocks);
  }

  // =========================================================================
  // Fixture size validation
  // =========================================================================

  TEST(EdidFixtures, AllFixturesAre128Bytes) {
    EXPECT_EQ(make_720p_edid().size(), k_edid_block_size);
    EXPECT_EQ(make_1080p_edid().size(), k_edid_block_size);
    EXPECT_EQ(make_1440p_edid().size(), k_edid_block_size);
    EXPECT_EQ(make_2160p_edid().size(), k_edid_block_size);
  }

  TEST(EdidFixtures, BadChecksumEdidIs128Bytes) {
    EXPECT_EQ(make_bad_checksum_edid().size(), k_edid_block_size);
  }

  TEST(EdidFixtures, TruncatedEdidIsShort) {
    auto edid = make_truncated_edid();
    EXPECT_LT(edid.size(), k_edid_block_size);
    EXPECT_EQ(edid.size(), 64U);
  }

  TEST(EdidFixtures, EdidHeaderCorrect) {
    auto edid = make_1080p_edid();
    EXPECT_EQ(edid[0], 0x00);
    EXPECT_EQ(edid[1], 0xFF);
    EXPECT_EQ(edid[2], 0xFF);
    EXPECT_EQ(edid[3], 0xFF);
    EXPECT_EQ(edid[4], 0xFF);
    EXPECT_EQ(edid[5], 0xFF);
    EXPECT_EQ(edid[6], 0xFF);
    EXPECT_EQ(edid[7], 0x00);
  }

  TEST(EdidFixtures, EdidVersionIs14) {
    auto edid = make_1080p_edid();
    EXPECT_EQ(edid[18], 0x01);  // Version 1
    EXPECT_EQ(edid[19], 0x04);  // Revision 4
  }

}  // namespace
