/**
 * @file tests/unit/rkmpp/test_edid.cpp
 * @brief Tests native EDID parsing, projection, and device I/O boundaries.
 */

#include "tests/unit/rkmpp/edid_test_fixtures.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <functional>
#include <gtest/gtest.h>

namespace {
  using namespace platf::edid;
  using platf::hdmirx::resolution_t;

  /** @brief Mockable in-memory EDID backend. */
  class memory_backend_t final: public ioctl_backend_t {
  public:
    std::vector<std::uint8_t> bytes;  ///< Device EDID storage.
    std::uint32_t reads {};  ///< Number of get calls.
    std::uint32_t writes {};  ///< Number of set calls.
    bool partial_read {};  ///< Return one fewer block from multi-block reads.
    bool partial_write {};  ///< Return one fewer block from writes.

    edid_result_t<std::uint32_t> get_edid(
      std::uint32_t,
      std::uint32_t,
      std::uint32_t blocks,
      std::span<std::uint8_t> output
    ) override {
      ++reads;
      const auto available = static_cast<std::uint32_t>(bytes.size() / k_edid_block_size);
      if (available < blocks) {
        return std::unexpected(edid_error_t {error_category_e::no_data, ENODATA, "not enough blocks"});
      }
      std::copy_n(bytes.begin(), static_cast<std::size_t>(blocks) * k_edid_block_size, output.begin());
      return partial_read && blocks > 1U ? blocks - 1U : blocks;
    }

    edid_result_t<std::uint32_t> set_edid(
      std::uint32_t,
      std::uint32_t,
      std::uint32_t blocks,
      std::span<const std::uint8_t> input
    ) override {
      ++writes;
      if (!partial_write) {
        bytes.assign(input.begin(), input.end());
      }
      return partial_write ? blocks - 1U : blocks;
    }
  };

  /** @brief Return whether a catalog contains a resolution from an origin. */
  bool contains(
    const std::vector<mode_record_t> &catalog,
    resolution_t resolution,
    mode_origin_e origin
  ) {
    return std::any_of(catalog.begin(), catalog.end(), [&](const auto &record) {
      return record.mode.resolution == resolution && record.origin == origin;
    });
  }

  TEST(EdidErrors, ClassifiesErrnoValues) {
    EXPECT_EQ(classify_errno(ENOTTY), error_category_e::not_supported);
    EXPECT_EQ(classify_errno(EINVAL), error_category_e::invalid_argument);
    EXPECT_EQ(classify_errno(ENODATA), error_category_e::no_data);
    EXPECT_EQ(classify_errno(E2BIG), error_category_e::too_large);
    EXPECT_EQ(classify_errno(EACCES), error_category_e::permission_denied);
    EXPECT_EQ(classify_errno(EPERM), error_category_e::permission_denied);
    EXPECT_EQ(classify_errno(ENODEV), error_category_e::device_gone);
    EXPECT_EQ(classify_errno(ENXIO), error_category_e::device_gone);
    EXPECT_EQ(classify_errno(EIO), error_category_e::io_error);
    EXPECT_STREQ(error_category_name(error_category_e::io_error), "io_error");
  }

  TEST(EdidValidation, RequiresExactDeclaredBlocksAndEveryChecksum) {
    const auto native = edid_test::make_native_edid();
    EXPECT_TRUE(validate_edid_checksums(native));
    EXPECT_FALSE(validate_edid_checksums(edid_test::make_bad_checksum_edid()));
    auto invalid_header = native;
    invalid_header.front() = 0xffU;
    auto invalid_header_base = std::span<std::uint8_t, k_edid_block_size> {invalid_header.data(), k_edid_block_size};
    edid_test::fix_checksum(invalid_header_base);
    EXPECT_FALSE(validate_edid_checksums(invalid_header));
    EXPECT_FALSE(validate_edid_checksums(std::span<const std::uint8_t> {native.data(), 64}));
    auto extra = native;
    extra.resize(native.size() + k_edid_block_size, 0);
    EXPECT_FALSE(validate_edid_checksums(extra));
  }

  TEST(EdidDetailedTiming, ParsesNativeTotalsAndInterlaceFlag) {
    auto edid = edid_test::make_base_edid();
    std::span<std::uint8_t, k_edid_block_size> block {edid.data(), edid.size()};
    block[54 + 17] |= 0x80U;
    edid_test::fix_checksum(block);
    const auto timing = parse_timing_descriptor(std::span<const std::uint8_t, 18> {edid.data() + 54, 18});
    ASSERT_TRUE(timing);
    EXPECT_EQ(timing->h_active, 1920);
    EXPECT_EQ(timing->h_blanking, 280);
    EXPECT_EQ(timing->v_active, 1080);
    EXPECT_EQ(timing->v_blanking, 45);
    EXPECT_TRUE(timing->interlaced);
    const auto mode = timing_to_hdmi_mode(*timing);
    ASSERT_TRUE(mode);
    EXPECT_EQ(mode->resolution, (resolution_t {1920, 1080}));
  }

  TEST(EdidCatalog, ParsesEverySupportedNativeEncoding) {
    const auto native = edid_test::make_native_edid();
    const auto catalog = parse_mode_catalog(native);
    EXPECT_TRUE(contains(catalog, {640, 480}, mode_origin_e::established_timing));
    EXPECT_TRUE(contains(catalog, {800, 600}, mode_origin_e::established_timing));
    EXPECT_TRUE(contains(catalog, {1024, 768}, mode_origin_e::established_timing));
    EXPECT_TRUE(contains(catalog, {1600, 900}, mode_origin_e::standard_timing));
    EXPECT_TRUE(contains(catalog, {1920, 1080}, mode_origin_e::base_dtd));
    EXPECT_TRUE(contains(catalog, {1920, 1080}, mode_origin_e::cta_vdb));
    EXPECT_TRUE(contains(catalog, {1280, 720}, mode_origin_e::cta_vdb));
    EXPECT_TRUE(contains(catalog, {1280, 720}, mode_origin_e::cta_dtd));
    EXPECT_TRUE(contains(catalog, {3840, 2160}, mode_origin_e::cta_y420_vdb));
  }

  TEST(EdidCatalog, DeduplicatesPolicyModesButKeepsProvenance) {
    const auto native = edid_test::make_native_edid();
    const auto catalog = parse_mode_catalog(native);
    const auto modes = parse_edid_modes(native);
    EXPECT_GT(catalog.size(), modes.size());
    EXPECT_EQ(std::count_if(modes.begin(), modes.end(), [](const auto &mode) {
                return mode.resolution == resolution_t {1920, 1080};
              }),
              1);
  }

  TEST(EdidProjection, PreservesNative1080DetailedTimingBytes) {
    const auto native = edid_test::make_native_edid();
    const auto projected = project_edid_to_resolution(native, {1920, 1080});
    ASSERT_FALSE(projected.empty());
    EXPECT_TRUE(validate_edid_checksums(projected));
    EXPECT_TRUE(std::equal(native.begin() + 54, native.begin() + 72, projected.begin() + 54));
    const auto catalog = parse_mode_catalog(projected);
    ASSERT_FALSE(catalog.empty());
    EXPECT_TRUE(std::all_of(catalog.begin(), catalog.end(), [](const auto &record) {
      return record.mode.resolution == resolution_t {1920, 1080};
    }));
  }

  TEST(EdidProjection, PreservesNonVideoCtaCapabilities) {
    const auto native = edid_test::make_native_edid();
    const auto projected = project_edid_to_resolution(native, {1920, 1080});
    ASSERT_FALSE(projected.empty());
    const std::array<std::uint8_t, 4> audio {0x23, 0x09, 0x07, 0x07};
    const std::array<std::uint8_t, 6> hdmi_vsdb {0x65, 0x03, 0x0c, 0x00, 0x10, 0x00};
    EXPECT_NE(std::search(projected.begin(), projected.end(), audio.begin(), audio.end()), projected.end());
    EXPECT_NE(std::search(projected.begin(), projected.end(), hdmi_vsdb.begin(), hdmi_vsdb.end()), projected.end());
  }

  TEST(EdidProjection, EmitsEachSelectedCtaVicExactlyOnce) {
    const auto projected = project_edid_to_resolution(edid_test::make_native_edid(), {1920, 1080});
    ASSERT_FALSE(projected.empty());
    const auto extension_offset = k_edid_block_size;
    ASSERT_EQ(projected[extension_offset + 4], 0x41U);
    EXPECT_EQ(projected[extension_offset + 5], 0x90U);
    EXPECT_EQ(projected[extension_offset + 6] >> 5U, 1U);
  }

  TEST(EdidProjection, PreservesNativeCtaDetailedTimingBytes) {
    const auto native = edid_test::make_native_edid();
    const auto source_catalog = parse_mode_catalog(native);
    const auto projected = project_edid_to_resolution(native, {1280, 720});
    ASSERT_FALSE(projected.empty());
    const auto projected_catalog = parse_mode_catalog(projected);
    const auto source_dtd = std::find_if(source_catalog.begin(), source_catalog.end(), [](const auto &record) {
      return record.origin == mode_origin_e::cta_dtd && record.mode.resolution == resolution_t {1280, 720};
    });
    const auto projected_dtd = std::find_if(projected_catalog.begin(), projected_catalog.end(), [](const auto &record) {
      return record.origin == mode_origin_e::cta_dtd;
    });
    ASSERT_NE(source_dtd, source_catalog.end());
    ASSERT_NE(projected_dtd, projected_catalog.end());
    EXPECT_EQ(projected_dtd->raw_encoding, source_dtd->raw_encoding);
    EXPECT_TRUE(std::all_of(projected_catalog.begin(), projected_catalog.end(), [](const auto &record) {
      return record.mode.resolution == resolution_t {1280, 720};
    }));
  }

  TEST(EdidProjection, SupportsStandardAndEstablishedModesWithoutTemplates) {
    const auto native = edid_test::make_native_edid();
    for (const auto resolution : {resolution_t {1600, 900}, resolution_t {640, 480}}) {
      const auto projected = project_edid_to_resolution(native, resolution);
      ASSERT_FALSE(projected.empty());
      const auto catalog = parse_mode_catalog(projected);
      ASSERT_FALSE(catalog.empty());
      EXPECT_TRUE(std::all_of(catalog.begin(), catalog.end(), [&](const auto &record) {
        return record.mode.resolution == resolution;
      }));
    }
  }

  TEST(EdidProjection, RejectsInvalidUnadvertisedAndUnknownExtensionData) {
    const auto native = edid_test::make_native_edid();
    EXPECT_TRUE(project_edid_to_resolution(native, {1366, 768}).empty());
    EXPECT_TRUE(project_edid_to_resolution(edid_test::make_bad_checksum_edid(), {1920, 1080}).empty());
    auto unknown = native;
    unknown[k_edid_block_size] = 0x70;
    auto extension = std::span<std::uint8_t, k_edid_block_size> {unknown.data() + k_edid_block_size, k_edid_block_size};
    edid_test::fix_checksum(extension);
    EXPECT_TRUE(project_edid_to_resolution(unknown, {1920, 1080}).empty());
  }

  TEST(EdidIo, ReadsCompleteDeclaredEdid) {
    memory_backend_t backend;
    backend.bytes = edid_test::make_native_edid();
    const auto result = read_edid(backend);
    ASSERT_TRUE(result);
    EXPECT_EQ(*result, backend.bytes);
    EXPECT_EQ(backend.reads, 2U);
  }

  TEST(EdidIo, RejectsIncompleteMultiBlockRead) {
    memory_backend_t backend;
    backend.bytes = edid_test::make_native_edid();
    backend.partial_read = true;
    const auto result = read_edid(backend);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, error_category_e::io_error);
  }

  TEST(EdidIo, ValidatesWriteShapeAndPartialWrites) {
    memory_backend_t backend;
    EXPECT_FALSE(write_edid(backend, 0, {}).has_value());
    std::vector<std::uint8_t> oversized(k_max_edid_size + k_edid_block_size, 0);
    EXPECT_EQ(write_edid(backend, 0, oversized).error().category, error_category_e::too_large);
    backend.bytes = edid_test::make_base_edid();
    backend.partial_write = true;
    const auto result = write_edid(backend, 0, backend.bytes);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, error_category_e::io_error);
  }
}  // namespace
