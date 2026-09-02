/**
 * @file src/platform/linux/edid.cpp
 * @brief EDID parsing, native-mode projection, and V4L2 ioctl implementation.
 */

#include "src/platform/linux/edid.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <numeric>

namespace platf::edid {
  namespace {
    /** @brief One established-timing bit and its decoded video mode. */
    struct established_timing_t {
      std::uint8_t byte;  ///< Base-block byte containing the bit.
      std::uint8_t mask;  ///< Bit mask inside byte.
      platf::hdmirx::resolution_t resolution;  ///< Active dimensions.
      platf::hdmirx::refresh_rate_t refresh;  ///< Nominal refresh rate.
      bool interlaced;  ///< Whether the mode is interlaced.
    };

    constexpr std::array k_established_timings {
      established_timing_t {35, 0x80, {720, 400}, {70, 1}, false},
      established_timing_t {35, 0x40, {720, 400}, {88, 1}, false},
      established_timing_t {35, 0x20, {640, 480}, {60, 1}, false},
      established_timing_t {35, 0x10, {640, 480}, {67, 1}, false},
      established_timing_t {35, 0x08, {640, 480}, {72, 1}, false},
      established_timing_t {35, 0x04, {640, 480}, {75, 1}, false},
      established_timing_t {35, 0x02, {800, 600}, {56, 1}, false},
      established_timing_t {35, 0x01, {800, 600}, {60, 1}, false},
      established_timing_t {36, 0x80, {800, 600}, {72, 1}, false},
      established_timing_t {36, 0x40, {800, 600}, {75, 1}, false},
      established_timing_t {36, 0x20, {832, 624}, {75, 1}, false},
      established_timing_t {36, 0x10, {1024, 768}, {87, 1}, true},
      established_timing_t {36, 0x08, {1024, 768}, {60, 1}, false},
      established_timing_t {36, 0x04, {1024, 768}, {70, 1}, false},
      established_timing_t {36, 0x02, {1024, 768}, {75, 1}, false},
      established_timing_t {36, 0x01, {1280, 1024}, {75, 1}, false},
      established_timing_t {37, 0x80, {1152, 870}, {75, 1}, false},
    };

    /** @brief Update one EDID block checksum after rewriting bytes. */
    void fix_checksum(std::span<std::uint8_t, k_edid_block_size> block) noexcept {
      const auto sum = std::accumulate(block.begin(), block.begin() + 127, std::uint8_t {0});
      block[127] = static_cast<std::uint8_t>(0U - sum);
    }

    /** @brief Compare refresh rates exactly. */
    bool same_refresh(const platf::hdmirx::refresh_rate_t &left, const platf::hdmirx::refresh_rate_t &right) noexcept {
      return static_cast<std::uint64_t>(left.numerator) * right.denominator ==
             static_cast<std::uint64_t>(right.numerator) * left.denominator;
    }

    /** @brief Compare hardware-independent modes while ignoring provenance. */
    bool same_mode(const platf::hdmirx::hdmi_mode_t &left, const platf::hdmirx::hdmi_mode_t &right) noexcept {
      return left.resolution == right.resolution && same_refresh(left.refresh_rate, right.refresh_rate);
    }

    /** @brief Decode one base-block standard timing pair. */
    std::optional<platf::hdmirx::hdmi_mode_t> standard_timing_to_mode(
      std::uint8_t horizontal,
      std::uint8_t aspect_and_refresh,
      bool legacy_aspect
    ) noexcept {
      if ((horizontal == 0x01U && aspect_and_refresh == 0x01U) || horizontal == 0U) {
        return std::nullopt;
      }
      const auto width = static_cast<std::uint32_t>(horizontal + 31U) * 8U;
      std::uint32_t height;
      switch (aspect_and_refresh >> 6U) {
        case 0:
          height = legacy_aspect ? width : width * 10U / 16U;
          break;
        case 1:
          height = width * 3U / 4U;
          break;
        case 2:
          height = width * 4U / 5U;
          break;
        default:
          height = width * 9U / 16U;
          break;
      }
      if (height == 0) {
        return std::nullopt;
      }
      return platf::hdmirx::hdmi_mode_t {{width, height}, {static_cast<std::uint32_t>(aspect_and_refresh & 0x3fU) + 60U, 1}, true};
    }

    /** @brief Decode CTA VICs used by HDMI sources supported by this receiver. */
    std::optional<std::pair<platf::hdmirx::hdmi_mode_t, bool>> cta_vic_to_mode(std::uint8_t vic) noexcept {
      using platf::hdmirx::hdmi_mode_t;
      const auto progressive = [](platf::hdmirx::resolution_t resolution, platf::hdmirx::refresh_rate_t refresh) {
        return std::pair {hdmi_mode_t {resolution, refresh, true}, false};
      };
      const auto interlaced = [](platf::hdmirx::resolution_t resolution, platf::hdmirx::refresh_rate_t refresh) {
        return std::pair {hdmi_mode_t {resolution, refresh, true}, true};
      };
      switch (vic) {
        case 1:
          return progressive({640, 480}, {60, 1});
        case 2:
        case 3:
          return progressive({720, 480}, {60, 1});
        case 4:
          return progressive({1280, 720}, {60, 1});
        case 5:
          return interlaced({1920, 1080}, {60, 1});
        case 16:
          return progressive({1920, 1080}, {60, 1});
        case 17:
        case 18:
          return progressive({720, 576}, {50, 1});
        case 19:
          return progressive({1280, 720}, {50, 1});
        case 20:
          return interlaced({1920, 1080}, {50, 1});
        case 31:
          return progressive({1920, 1080}, {50, 1});
        case 32:
          return progressive({1920, 1080}, {24, 1});
        case 33:
          return progressive({1920, 1080}, {25, 1});
        case 34:
          return progressive({1920, 1080}, {30, 1});
        case 60:
          return progressive({1280, 720}, {24, 1});
        case 61:
          return progressive({1280, 720}, {25, 1});
        case 62:
          return progressive({1280, 720}, {30, 1});
        case 63:
          return progressive({1920, 1080}, {120, 1});
        case 64:
          return progressive({1920, 1080}, {100, 1});
        case 93:
          return progressive({3840, 2160}, {24, 1});
        case 94:
          return progressive({3840, 2160}, {25, 1});
        case 95:
          return progressive({3840, 2160}, {30, 1});
        case 96:
          return progressive({3840, 2160}, {50, 1});
        case 97:
          return progressive({3840, 2160}, {60, 1});
        case 98:
          return progressive({4096, 2160}, {24, 1});
        case 99:
          return progressive({4096, 2160}, {25, 1});
        case 100:
          return progressive({4096, 2160}, {30, 1});
        case 101:
          return progressive({4096, 2160}, {50, 1});
        case 102:
          return progressive({4096, 2160}, {60, 1});
        default:
          return std::nullopt;
      }
    }

    /** @brief Append a DTD record from a known block and byte offset. */
    void append_dtd_record(
      std::vector<mode_record_t> &catalog,
      std::span<const std::uint8_t, 18> bytes,
      mode_origin_e origin,
      std::uint32_t block_index,
      std::uint32_t byte_offset,
      bool native
    ) {
      const auto timing = parse_timing_descriptor(bytes);
      if (!timing) {
        return;
      }
      const auto mode = timing_to_hdmi_mode(*timing);
      if (!mode) {
        return;
      }
      catalog.push_back(mode_record_t {
        .mode = *mode,
        .origin = origin,
        .block_index = block_index,
        .byte_offset = byte_offset,
        .native = native,
        .y420_only = false,
        .interlaced = timing->interlaced,
        .raw_encoding = {bytes.begin(), bytes.end()},
      });
    }

    /** @brief Check that a CTA data-block collection has valid boundaries. */
    bool valid_cta_data_blocks(std::span<const std::uint8_t, k_edid_block_size> extension) noexcept {
      if (extension[0] != 0x02U) {
        return false;
      }
      if (extension[2] == 0) {
        return true;
      }
      const auto end = static_cast<std::size_t>(extension[2]);
      if (end < 4 || end > 127) {
        return false;
      }
      for (std::size_t offset = 4; offset < end;) {
        const auto length = static_cast<std::size_t>(extension[offset++] & 0x1fU);
        if (length > end - offset) {
          return false;
        }
        offset += length;
      }
      return true;
    }

    /** @brief Filter one CTA video data block into a projected collection. */
    bool append_projected_cta_block(
      std::span<const std::uint8_t> block,
      const platf::hdmirx::resolution_t &target,
      std::vector<std::uint8_t> &output
    ) {
      const auto header = block.front();
      const auto tag = header >> 5U;
      const auto length = static_cast<std::size_t>(header & 0x1fU);
      const bool y420_vdb = tag == 0x07U && length > 1U && block[1] == 0x0eU;
      const bool y420_map = tag == 0x07U && length > 0U && block[1] == 0x0fU;
      if (tag != 0x02U && !y420_vdb && !y420_map) {
        output.insert(output.end(), block.begin(), block.end());
        return true;
      }
      if (y420_map) {
        return true;
      }

      const auto first = y420_vdb ? std::size_t {2} : std::size_t {1};
      std::vector<std::uint8_t> selected;
      for (std::size_t index = first; index < block.size(); ++index) {
        const auto vic = static_cast<std::uint8_t>(block[index] & 0x7fU);
        const auto decoded = cta_vic_to_mode(vic);
        if (!decoded) {
          return false;
        }
        if (decoded->first.resolution == target) {
          selected.push_back(block[index]);
        }
      }
      if (selected.empty()) {
        return true;
      }
      if (y420_vdb) {
        output.push_back(static_cast<std::uint8_t>(0xe0U | (selected.size() + 1U)));
        output.push_back(0x0eU);
        output.insert(output.end(), selected.begin(), selected.end());
      } else {
        if (std::none_of(selected.begin(), selected.end(), [](std::uint8_t value) {
              return (value & 0x80U) != 0;
            })) {
          selected.front() |= 0x80U;
        }
        output.push_back(static_cast<std::uint8_t>(0x40U | selected.size()));
        output.insert(output.end(), selected.begin(), selected.end());
      }
      return true;
    }
  }  // namespace

  error_category_e classify_errno(int err_no) noexcept {
    switch (err_no) {
      case ENOTTY:
        return error_category_e::not_supported;
      case EINVAL:
        return error_category_e::invalid_argument;
      case ENODATA:
        return error_category_e::no_data;
      case E2BIG:
        return error_category_e::too_large;
      case EACCES:
      case EPERM:
        return error_category_e::permission_denied;
      case ENODEV:
      case ENXIO:
        return error_category_e::device_gone;
      default:
        return error_category_e::io_error;
    }
  }

  const char *error_category_name(error_category_e category) noexcept {
    switch (category) {
      case error_category_e::not_supported:
        return "not_supported";
      case error_category_e::invalid_argument:
        return "invalid_argument";
      case error_category_e::no_data:
        return "no_data";
      case error_category_e::too_large:
        return "too_large";
      case error_category_e::permission_denied:
        return "permission_denied";
      case error_category_e::device_gone:
        return "device_gone";
      case error_category_e::io_error:
        return "io_error";
    }
    return "unknown";
  }

  std::uint8_t compute_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept {
    return std::accumulate(block.begin(), block.end(), std::uint8_t {0});
  }

  bool validate_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept {
    return compute_block_checksum(block) == 0;
  }

  bool validate_edid_checksums(std::span<const std::uint8_t> edid_data) noexcept {
    if (edid_data.size() < k_edid_block_size || edid_data.size() % k_edid_block_size != 0) {
      return false;
    }
    constexpr std::array<std::uint8_t, 8> header {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (!std::equal(header.begin(), header.end(), edid_data.begin())) {
      return false;
    }
    const auto declared_blocks = static_cast<std::size_t>(edid_data[126]) + 1U;
    if (declared_blocks > k_max_edid_blocks || edid_data.size() != declared_blocks * k_edid_block_size) {
      return false;
    }
    for (std::size_t index = 0; index < declared_blocks; ++index) {
      std::span<const std::uint8_t, k_edid_block_size> block {edid_data.data() + index * k_edid_block_size, k_edid_block_size};
      if (!validate_block_checksum(block)) {
        return false;
      }
    }
    return true;
  }

  std::optional<timing_descriptor_t> parse_timing_descriptor(std::span<const std::uint8_t, 18> descriptor) noexcept {
    const auto pixel_clock_10khz = static_cast<std::uint16_t>(descriptor[0] | (descriptor[1] << 8U));
    if (pixel_clock_10khz == 0) {
      return std::nullopt;
    }
    return timing_descriptor_t {
      .pixel_clock_khz = static_cast<std::uint32_t>(pixel_clock_10khz) * 10U,
      .h_active = static_cast<std::uint16_t>(descriptor[2] | ((descriptor[4] >> 4U) << 8U)),
      .h_blanking = static_cast<std::uint16_t>(descriptor[3] | ((descriptor[4] & 0x0fU) << 8U)),
      .v_active = static_cast<std::uint16_t>(descriptor[5] | ((descriptor[7] >> 4U) << 8U)),
      .v_blanking = static_cast<std::uint16_t>(descriptor[6] | ((descriptor[7] & 0x0fU) << 8U)),
      .interlaced = (descriptor[17] & 0x80U) != 0,
    };
  }

  std::vector<timing_descriptor_t> parse_base_block_timings(std::span<const std::uint8_t, k_edid_block_size> base_block) noexcept {
    std::vector<timing_descriptor_t> timings;
    for (std::size_t offset = 54; offset <= 108; offset += 18) {
      std::span<const std::uint8_t, 18> descriptor {base_block.data() + offset, 18};
      if (const auto timing = parse_timing_descriptor(descriptor)) {
        timings.push_back(*timing);
      }
    }
    return timings;
  }

  std::optional<platf::hdmirx::hdmi_mode_t> timing_to_hdmi_mode(const timing_descriptor_t &timing) noexcept {
    const auto h_total = static_cast<std::uint32_t>(timing.h_active) + timing.h_blanking;
    const auto v_total = static_cast<std::uint32_t>(timing.v_active) + timing.v_blanking;
    if (timing.pixel_clock_khz == 0 || timing.h_active == 0 || timing.v_active == 0 || h_total == 0 || v_total == 0) {
      return std::nullopt;
    }
    return platf::hdmirx::hdmi_mode_t {
      .resolution = {timing.h_active, timing.v_active},
      .refresh_rate = {timing.pixel_clock_khz * 1000U, h_total * v_total},
      .verified = true,
    };
  }

  std::vector<mode_record_t> parse_mode_catalog(std::span<const std::uint8_t> edid_data) noexcept {
    if (!validate_edid_checksums(edid_data)) {
      return {};
    }
    std::vector<mode_record_t> catalog;
    const std::span<const std::uint8_t, k_edid_block_size> base {edid_data.data(), k_edid_block_size};
    for (const auto &timing : k_established_timings) {
      if ((base[timing.byte] & timing.mask) == 0) {
        continue;
      }
      catalog.push_back(mode_record_t {
        .mode = {timing.resolution, timing.refresh, true},
        .origin = mode_origin_e::established_timing,
        .block_index = 0,
        .byte_offset = timing.byte,
        .native = false,
        .y420_only = false,
        .interlaced = timing.interlaced,
        .raw_encoding = {timing.mask},
      });
    }

    const bool legacy_aspect = base[18] == 1U && base[19] < 3U;
    for (std::size_t offset = 38; offset < 54; offset += 2) {
      const auto mode = standard_timing_to_mode(base[offset], base[offset + 1], legacy_aspect);
      if (!mode) {
        continue;
      }
      catalog.push_back(mode_record_t {
        .mode = *mode,
        .origin = mode_origin_e::standard_timing,
        .block_index = 0,
        .byte_offset = static_cast<std::uint32_t>(offset),
        .native = false,
        .y420_only = false,
        .interlaced = false,
        .raw_encoding = {base[offset], base[offset + 1]},
      });
    }

    for (std::size_t offset = 54, index = 0; offset <= 108; offset += 18, ++index) {
      append_dtd_record(catalog, std::span<const std::uint8_t, 18> {base.data() + offset, 18}, mode_origin_e::base_dtd, 0, static_cast<std::uint32_t>(offset), index == 0 && (base[24] & 0x02U) != 0);
    }

    const auto block_count = static_cast<std::size_t>(base[126]) + 1U;
    for (std::size_t block_index = 1; block_index < block_count; ++block_index) {
      const std::span<const std::uint8_t, k_edid_block_size> extension {edid_data.data() + block_index * k_edid_block_size, k_edid_block_size};
      if (!valid_cta_data_blocks(extension)) {
        return {};
      }
      const auto data_end = extension[2] == 0 ? std::size_t {4} : static_cast<std::size_t>(extension[2]);
      for (std::size_t offset = 4; offset < data_end;) {
        const auto header_offset = offset;
        const auto header = extension[offset++];
        const auto tag = header >> 5U;
        const auto length = static_cast<std::size_t>(header & 0x1fU);
        const bool y420_vdb = tag == 0x07U && length > 1U && extension[offset] == 0x0eU;
        if (tag == 0x02U || y420_vdb) {
          const auto first = y420_vdb ? std::size_t {1} : std::size_t {0};
          for (std::size_t index = first; index < length; ++index) {
            const auto encoded = extension[offset + index];
            const auto decoded = cta_vic_to_mode(encoded & 0x7fU);
            if (decoded) {
              catalog.push_back(mode_record_t {
                .mode = decoded->first,
                .origin = y420_vdb ? mode_origin_e::cta_y420_vdb : mode_origin_e::cta_vdb,
                .block_index = static_cast<std::uint32_t>(block_index),
                .byte_offset = static_cast<std::uint32_t>(offset + index),
                .native = !y420_vdb && (encoded & 0x80U) != 0,
                .y420_only = y420_vdb,
                .interlaced = decoded->second,
                .raw_encoding = {encoded},
              });
            }
          }
        }
        offset = header_offset + 1U + length;
      }
      if (extension[2] != 0) {
        const auto native_dtd_count = static_cast<std::size_t>(extension[3] & 0x0fU);
        for (std::size_t offset = data_end, index = 0; offset + 18 <= 127; offset += 18, ++index) {
          append_dtd_record(catalog, std::span<const std::uint8_t, 18> {extension.data() + offset, 18}, mode_origin_e::cta_dtd, static_cast<std::uint32_t>(block_index), static_cast<std::uint32_t>(offset), index < native_dtd_count);
        }
      }
    }
    return catalog;
  }

  std::vector<platf::hdmirx::hdmi_mode_t> parse_edid_modes(std::span<const std::uint8_t> edid_data) noexcept {
    std::vector<platf::hdmirx::hdmi_mode_t> modes;
    for (const auto &record : parse_mode_catalog(edid_data)) {
      if (std::none_of(modes.begin(), modes.end(), [&record](const auto &candidate) {
            return same_mode(candidate, record.mode);
          })) {
        modes.push_back(record.mode);
      }
    }
    return modes;
  }

  std::vector<std::uint8_t> project_edid_to_resolution(
    std::span<const std::uint8_t> source_edid,
    const platf::hdmirx::resolution_t &target
  ) noexcept {
    const auto catalog = parse_mode_catalog(source_edid);
    if (catalog.empty() || std::none_of(catalog.begin(), catalog.end(), [&target](const auto &record) {
          return record.mode.resolution == target;
        })) {
      return {};
    }
    const auto block_count = static_cast<std::size_t>(source_edid[126]) + 1U;
    for (std::size_t index = 1; index < block_count; ++index) {
      if (source_edid[index * k_edid_block_size] != 0x02U) {
        return {};
      }
    }

    std::vector<std::uint8_t> projected(source_edid.begin(), source_edid.end());
    auto base = std::span<std::uint8_t, k_edid_block_size> {projected.data(), k_edid_block_size};
    for (const auto &timing : k_established_timings) {
      if (timing.resolution != target) {
        base[timing.byte] &= static_cast<std::uint8_t>(~timing.mask);
      }
    }
    for (std::size_t offset = 38; offset < 54; offset += 2) {
      const auto mode = standard_timing_to_mode(base[offset], base[offset + 1], base[18] == 1U && base[19] < 3U);
      if (mode && mode->resolution != target) {
        base[offset] = 0x01U;
        base[offset + 1] = 0x01U;
      }
    }

    std::vector<std::array<std::uint8_t, 18>> base_target_dtds;
    std::vector<std::array<std::uint8_t, 18>> monitor_descriptors;
    for (std::size_t offset = 54; offset <= 108; offset += 18) {
      std::array<std::uint8_t, 18> raw {};
      std::copy_n(base.begin() + offset, raw.size(), raw.begin());
      const auto timing = parse_timing_descriptor(std::span<const std::uint8_t, 18> {raw.data(), raw.size()});
      if (!timing) {
        if (std::any_of(raw.begin(), raw.end(), [](std::uint8_t value) {
              return value != 0;
            })) {
          monitor_descriptors.push_back(raw);
        }
      } else if (timing->h_active == target.width && timing->v_active == target.height) {
        base_target_dtds.push_back(raw);
      }
    }
    std::fill(base.begin() + 54, base.begin() + 126, std::uint8_t {0});
    auto descriptor_offset = std::size_t {54};
    for (const auto &raw : base_target_dtds) {
      if (descriptor_offset > 108) {
        break;
      }
      std::copy(raw.begin(), raw.end(), base.begin() + descriptor_offset);
      descriptor_offset += raw.size();
    }
    for (const auto &raw : monitor_descriptors) {
      if (descriptor_offset > 108) {
        break;
      }
      std::copy(raw.begin(), raw.end(), base.begin() + descriptor_offset);
      descriptor_offset += raw.size();
    }
    if (base_target_dtds.empty()) {
      base[24] &= static_cast<std::uint8_t>(~0x02U);
    } else {
      base[24] |= 0x02U;
    }
    fix_checksum(base);

    for (std::size_t block_index = 1; block_index < block_count; ++block_index) {
      const std::span<const std::uint8_t, k_edid_block_size> source {source_edid.data() + block_index * k_edid_block_size, k_edid_block_size};
      if (!valid_cta_data_blocks(source)) {
        return {};
      }
      const auto data_end = source[2] == 0 ? std::size_t {4} : static_cast<std::size_t>(source[2]);
      std::vector<std::uint8_t> data_blocks;
      for (std::size_t offset = 4; offset < data_end;) {
        const auto length = static_cast<std::size_t>(source[offset] & 0x1fU);
        const auto block_size = length + 1U;
        if (!append_projected_cta_block(source.subspan(offset, block_size), target, data_blocks)) {
          return {};
        }
        offset += block_size;
      }

      std::vector<std::array<std::uint8_t, 18>> target_dtds;
      if (source[2] != 0) {
        for (std::size_t offset = data_end; offset + 18 <= 127; offset += 18) {
          std::array<std::uint8_t, 18> raw {};
          std::copy_n(source.begin() + offset, raw.size(), raw.begin());
          const auto timing = parse_timing_descriptor(std::span<const std::uint8_t, 18> {raw.data(), raw.size()});
          if (timing && timing->h_active == target.width && timing->v_active == target.height) {
            target_dtds.push_back(raw);
          }
        }
      }

      const auto dtd_offset = 4U + data_blocks.size();
      if (dtd_offset + target_dtds.size() * 18U > 127U) {
        return {};
      }
      std::array<std::uint8_t, k_edid_block_size> rewritten {};
      rewritten[0] = 0x02U;
      rewritten[1] = source[1];
      rewritten[3] = static_cast<std::uint8_t>((source[3] & 0xf0U) | std::min<std::size_t>(target_dtds.size(), 15U));
      std::copy(data_blocks.begin(), data_blocks.end(), rewritten.begin() + 4);
      auto output_offset = dtd_offset;
      for (const auto &raw : target_dtds) {
        std::copy(raw.begin(), raw.end(), rewritten.begin() + output_offset);
        output_offset += raw.size();
      }
      rewritten[2] = data_blocks.empty() && target_dtds.empty() ? 0U : static_cast<std::uint8_t>(dtd_offset);
      fix_checksum(std::span<std::uint8_t, k_edid_block_size> {rewritten.data(), rewritten.size()});
      std::copy(rewritten.begin(), rewritten.end(), projected.begin() + block_index * k_edid_block_size);
    }

    if (!validate_edid_checksums(projected)) {
      return {};
    }
    const auto projected_catalog = parse_mode_catalog(projected);
    if (projected_catalog.empty() || std::any_of(projected_catalog.begin(), projected_catalog.end(), [&target](const auto &record) {
          return record.mode.resolution != target;
        })) {
      return {};
    }
    return projected;
  }

  edid_result_t<std::vector<std::uint8_t>> read_edid(ioctl_backend_t &backend, std::uint32_t pad) {
    std::vector<std::uint8_t> base(k_edid_block_size, 0);
    auto base_result = backend.get_edid(pad, 0, 1, base);
    if (!base_result) {
      return std::unexpected(base_result.error());
    }
    if (*base_result != 1U) {
      return std::unexpected(edid_error_t {error_category_e::io_error, EIO, "base EDID read returned an unexpected block count"});
    }
    const auto block_count = static_cast<std::uint32_t>(base[126]) + 1U;
    if (block_count > k_max_edid_blocks) {
      return std::unexpected(edid_error_t {error_category_e::too_large, E2BIG, "EDID extension count exceeds supported maximum"});
    }
    if (block_count == 1U) {
      return base;
    }
    std::vector<std::uint8_t> complete(static_cast<std::size_t>(block_count) * k_edid_block_size, 0);
    auto result = backend.get_edid(pad, 0, block_count, complete);
    if (!result) {
      return std::unexpected(result.error());
    }
    if (*result != block_count) {
      return std::unexpected(edid_error_t {error_category_e::io_error, EIO, "incomplete multi-block EDID read"});
    }
    return complete;
  }

  edid_result_t<std::uint32_t> write_edid(ioctl_backend_t &backend, std::uint32_t pad, std::span<const std::uint8_t> data) {
    if (data.empty() || data.size() % k_edid_block_size != 0) {
      return std::unexpected(edid_error_t {error_category_e::invalid_argument, EINVAL, "EDID data size is not a multiple of 128 bytes"});
    }
    const auto block_count = static_cast<std::uint32_t>(data.size() / k_edid_block_size);
    if (block_count > k_max_edid_blocks) {
      return std::unexpected(edid_error_t {error_category_e::too_large, E2BIG, "EDID exceeds maximum block count"});
    }
    auto result = backend.set_edid(pad, 0, block_count, data);
    if (!result) {
      return std::unexpected(result.error());
    }
    if (*result != block_count) {
      return std::unexpected(edid_error_t {error_category_e::io_error, EIO, "partial EDID write"});
    }
    return result;
  }

}  // namespace platf::edid
