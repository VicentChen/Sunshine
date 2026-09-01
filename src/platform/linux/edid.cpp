/**
 * @file src/platform/linux/edid.cpp
 * @brief EDID data model, parser, ioctl abstraction, and RAII restore guard.
 */
#include "edid.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <numeric>

namespace platf::edid {

  // -------------------------------------------------------------------------
  // Error classification
  // -------------------------------------------------------------------------

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

  const char *error_category_name(error_category_e cat) noexcept {
    switch (cat) {
      case error_category_e::not_supported:     return "not_supported";
      case error_category_e::invalid_argument:  return "invalid_argument";
      case error_category_e::no_data:           return "no_data";
      case error_category_e::too_large:         return "too_large";
      case error_category_e::permission_denied: return "permission_denied";
      case error_category_e::device_gone:       return "device_gone";
      case error_category_e::io_error:          return "io_error";
    }
    return "unknown";
  }

  // -------------------------------------------------------------------------
  // EDID checksum and parsing
  // -------------------------------------------------------------------------

  std::uint8_t compute_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept {
    return std::accumulate(block.begin(), block.end(), std::uint8_t{0});
  }

  bool validate_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept {
    return compute_block_checksum(block) == 0;
  }

  bool validate_edid_checksums(std::span<const std::uint8_t> edid_data) noexcept {
    if (edid_data.size() < k_edid_block_size || edid_data.size() % k_edid_block_size != 0) {
      return false;
    }
    const auto block_count = edid_data.size() / k_edid_block_size;
    for (std::size_t i = 0; i < block_count; ++i) {
      auto block = edid_data.subspan(i * k_edid_block_size, k_edid_block_size);
      std::span<const std::uint8_t, k_edid_block_size> fixed{block.data(), k_edid_block_size};
      if (!validate_block_checksum(fixed)) {
        return false;
      }
    }
    return true;
  }

  std::optional<timing_descriptor_t> parse_timing_descriptor(
    std::span<const std::uint8_t, 18> desc) noexcept {
    // Pixel clock in 10 kHz units (little-endian at bytes 0-1)
    const auto pixel_clock_10khz = static_cast<std::uint16_t>(
      desc[0] | (desc[1] << 8));
    if (pixel_clock_10khz == 0) {
      // Zero pixel clock means this is a monitor descriptor, not timing.
      return std::nullopt;
    }

    timing_descriptor_t t;
    t.pixel_clock_khz = static_cast<std::uint32_t>(pixel_clock_10khz) * 10;

    // Horizontal active: lower 8 bits in byte 2, upper 4 bits in byte 4[7:4]
    t.h_active = static_cast<std::uint16_t>(
      desc[2] | ((desc[4] >> 4) << 8));
    // Horizontal blanking: lower 8 bits in byte 3, upper 4 bits in byte 4[3:0]
    t.h_blanking = static_cast<std::uint16_t>(
      desc[3] | ((desc[4] & 0x0F) << 8));
    // Vertical active: lower 8 bits in byte 5, upper 4 bits in byte 7[7:4]
    t.v_active = static_cast<std::uint16_t>(
      desc[5] | ((desc[7] >> 4) << 8));
    // Vertical blanking: lower 8 bits in byte 6, upper 4 bits in byte 7[3:0]
    t.v_blanking = static_cast<std::uint16_t>(
      desc[6] | ((desc[7] & 0x0F) << 8));

    return t;
  }

  std::vector<timing_descriptor_t> parse_base_block_timings(
    std::span<const std::uint8_t, k_edid_block_size> base_block) noexcept {
    std::vector<timing_descriptor_t> timings;
    // Four 18-byte descriptor slots start at offset 54 in the base block.
    for (int i = 0; i < 4; ++i) {
      const auto offset = 54 + i * 18;
      if (static_cast<std::uint32_t>(offset + 18) > k_edid_block_size) break;
      std::span<const std::uint8_t, 18> desc{base_block.data() + offset, 18};
      if (auto t = parse_timing_descriptor(desc)) {
        timings.push_back(*t);
      }
    }
    return timings;
  }

  std::optional<platf::hdmirx::hdmi_mode_t> timing_to_hdmi_mode(
    const timing_descriptor_t &timing) noexcept {
    if (timing.h_active == 0 || timing.v_active == 0 || timing.pixel_clock_khz == 0) {
      return std::nullopt;
    }

    const std::uint32_t h_total = static_cast<std::uint32_t>(timing.h_active) + timing.h_blanking;
    const std::uint32_t v_total = static_cast<std::uint32_t>(timing.v_active) + timing.v_blanking;

    if (h_total == 0 || v_total == 0) {
      return std::nullopt;
    }

    // Refresh rate: pixel_clock_khz * 1000 / (h_total * v_total) Hz
    // Express as rational to avoid floating point:
    // numerator = pixel_clock_khz * 1000, denominator = h_total * v_total
    const auto num = timing.pixel_clock_khz * 1000U;
    const auto den = h_total * v_total;

    return platf::hdmirx::hdmi_mode_t{
      .resolution = {timing.h_active, timing.v_active},
      .refresh_rate = {num, den},
      .verified = true,
    };
  }

  namespace {
    /** Map the progressive CTA VICs used by common HDMI consoles and displays. */
    std::optional<platf::hdmirx::hdmi_mode_t> cta_vic_to_hdmi_mode(std::uint8_t vic) noexcept {
      using platf::hdmirx::hdmi_mode_t;
      using platf::hdmirx::refresh_rate_t;
      using platf::hdmirx::resolution_t;
      const auto mode = [](resolution_t resolution, refresh_rate_t refresh_rate) {
        return hdmi_mode_t {resolution, refresh_rate, true};
      };
      switch (vic) {
        case 1: return mode({640, 480}, {60, 1});
        case 2:
        case 3: return mode({720, 480}, {60, 1});
        case 4: return mode({1280, 720}, {60, 1});
        case 16: return mode({1920, 1080}, {60, 1});
        case 17:
        case 18: return mode({720, 576}, {50, 1});
        case 19: return mode({1280, 720}, {50, 1});
        case 31: return mode({1920, 1080}, {50, 1});
        case 32: return mode({1920, 1080}, {24, 1});
        case 33: return mode({1920, 1080}, {25, 1});
        case 34: return mode({1920, 1080}, {30, 1});
        case 93: return mode({3840, 2160}, {24, 1});
        case 94: return mode({3840, 2160}, {25, 1});
        case 95: return mode({3840, 2160}, {30, 1});
        case 96: return mode({3840, 2160}, {50, 1});
        case 97: return mode({3840, 2160}, {60, 1});
        case 98: return mode({4096, 2160}, {24, 1});
        case 99: return mode({4096, 2160}, {25, 1});
        case 100: return mode({4096, 2160}, {30, 1});
        case 101: return mode({4096, 2160}, {50, 1});
        case 102: return mode({4096, 2160}, {60, 1});
        default: return std::nullopt;
      }
    }

    bool same_hdmi_mode(const platf::hdmirx::hdmi_mode_t &left, const platf::hdmirx::hdmi_mode_t &right) noexcept {
      if (left.resolution != right.resolution) {
        return false;
      }
      return static_cast<std::uint64_t>(left.refresh_rate.numerator) * right.refresh_rate.denominator ==
             static_cast<std::uint64_t>(right.refresh_rate.numerator) * left.refresh_rate.denominator;
    }

    void append_unique_mode(std::vector<platf::hdmirx::hdmi_mode_t> &modes, const platf::hdmirx::hdmi_mode_t &mode) {
      if (std::none_of(modes.begin(), modes.end(), [&mode](const auto &candidate) { return same_hdmi_mode(candidate, mode); })) {
        modes.push_back(mode);
      }
    }

    void append_cta_video_modes(std::span<const std::uint8_t, k_edid_block_size> extension,
                                std::vector<platf::hdmirx::hdmi_mode_t> &modes) {
      if (extension[0] != 0x02U) {
        return;
      }
      // A zero DTD offset means this CTA block has no data-block collection.
      // Treating the zero padding as headers would be harmless but obscures
      // malformed-extension diagnostics and needlessly scans the whole block.
      if (extension[2] == 0) {
        return;
      }
      const auto data_end = std::min<std::size_t>(extension[2], 127);
      for (std::size_t offset = 4; offset < data_end;) {
        const auto header = extension[offset++];
        const auto tag = header >> 5U;
        const auto length = static_cast<std::size_t>(header & 0x1fU);
        if (length > data_end - offset) {
          return;
        }
        if (tag == 0x02U) {
          for (std::size_t index = 0; index < length; ++index) {
            if (auto mode = cta_vic_to_hdmi_mode(extension[offset + index] & 0x7fU)) {
              append_unique_mode(modes, *mode);
            }
          }
        } else if (tag == 0x07U && length > 1U && extension[offset] == 0x0eU) {
          // CTA extended tag 0x0e is a YCbCr 4:2:0 Video Data Block. Its
          // remaining bytes are VICs just like a normal Video Data Block and
          // can advertise 4K50/60 even when the normal VDB contains only 2K.
          for (std::size_t index = 1; index < length; ++index) {
            if (auto mode = cta_vic_to_hdmi_mode(extension[offset + index] & 0x7fU)) {
              append_unique_mode(modes, *mode);
            }
          }
        }
        offset += length;
      }
    }
  }  // namespace

  std::vector<platf::hdmirx::hdmi_mode_t> parse_edid_modes(
    std::span<const std::uint8_t> edid_data) noexcept {
    if (edid_data.size() < k_edid_block_size) {
      return {};
    }

    // A mode set is trusted only when every block declared by the base block is
    // present and valid. This also prevents selecting from a truncated CTA list.
    std::span<const std::uint8_t, k_edid_block_size> base{edid_data.data(), k_edid_block_size};
    if (!validate_block_checksum(base)) {
      return {};
    }
    const auto block_count = static_cast<std::size_t>(base[126]) + 1U;
    if (block_count > k_max_edid_blocks || edid_data.size() < block_count * k_edid_block_size) {
      return {};
    }
    for (std::size_t index = 1; index < block_count; ++index) {
      std::span<const std::uint8_t, k_edid_block_size> block {edid_data.data() + index * k_edid_block_size, k_edid_block_size};
      if (!validate_block_checksum(block)) {
        return {};
      }
    }

    auto timings = parse_base_block_timings(base);
    std::vector<platf::hdmirx::hdmi_mode_t> modes;
    modes.reserve(timings.size());
    for (const auto &t : timings) {
      if (auto mode = timing_to_hdmi_mode(t)) {
        append_unique_mode(modes, *mode);
      }
    }
    for (std::size_t index = 1; index < block_count; ++index) {
      std::span<const std::uint8_t, k_edid_block_size> extension {edid_data.data() + index * k_edid_block_size, k_edid_block_size};
      append_cta_video_modes(extension, modes);
    }
    return modes;
  }

  // -------------------------------------------------------------------------
  // EDID fixture generation
  // -------------------------------------------------------------------------

  /**
   * @brief Write an EDID base-block header (bytes 0-7).
   *
   * Standard EDID 1.4 header: 00 FF FF FF FF FF FF 00.
   */
  static void write_edid_header(std::vector<std::uint8_t> &block) {
    block[0] = 0x00;
    block[1] = 0xFF;
    block[2] = 0xFF;
    block[3] = 0xFF;
    block[4] = 0xFF;
    block[5] = 0xFF;
    block[6] = 0xFF;
    block[7] = 0x00;
  }

  /**
   * @brief Write a detailed timing descriptor into an EDID block.
   *
   * @param block The EDID block buffer.
   * @param offset Byte offset within the block (typically 54, 72, 90, or 108).
   * @param h_active Horizontal active pixels.
   * @param v_active Vertical active lines.
   * @param h_blanking Horizontal blanking pixels.
   * @param v_blanking Vertical blanking lines.
   * @param pixel_clock_10khz Pixel clock in 10 kHz units.
   */
  static void write_timing_descriptor(
    std::vector<std::uint8_t> &block,
    std::size_t offset,
    std::uint16_t h_active,
    std::uint16_t v_active,
    std::uint16_t h_blanking,
    std::uint16_t v_blanking,
    std::uint16_t pixel_clock_10khz) {
    // Pixel clock (little-endian, 10kHz units)
    block[offset + 0] = static_cast<std::uint8_t>(pixel_clock_10khz & 0xFF);
    block[offset + 1] = static_cast<std::uint8_t>((pixel_clock_10khz >> 8) & 0xFF);
    // Horizontal active lower 8 bits
    block[offset + 2] = static_cast<std::uint8_t>(h_active & 0xFF);
    // Horizontal blanking lower 8 bits
    block[offset + 3] = static_cast<std::uint8_t>(h_blanking & 0xFF);
    // Upper nibbles: h_active[11:8] in upper, h_blanking[11:8] in lower
    block[offset + 4] = static_cast<std::uint8_t>(
      ((h_active >> 8) << 4) | ((h_blanking >> 8) & 0x0F));
    // Vertical active lower 8 bits
    block[offset + 5] = static_cast<std::uint8_t>(v_active & 0xFF);
    // Vertical blanking lower 8 bits
    block[offset + 6] = static_cast<std::uint8_t>(v_blanking & 0xFF);
    // Upper nibbles: v_active[11:8] in upper, v_blanking[11:8] in lower
    block[offset + 7] = static_cast<std::uint8_t>(
      ((v_active >> 8) << 4) | ((v_blanking >> 8) & 0x0F));
    // Remaining bytes (8-17) are sync offsets/widths, set to reasonable defaults.
    // Byte 8: h_sync_offset lower 8
    block[offset + 8] = 0x30;
    // Byte 9: h_sync_pulse_width lower 8
    block[offset + 9] = 0x20;
    // Byte 10: v_sync_offset[3:0] | v_sync_pulse_width[3:0]
    block[offset + 10] = 0x35;
    // Byte 11: upper bits of sync values
    block[offset + 11] = 0x00;
    // Bytes 12-13: horizontal/vertical image size (mm)
    block[offset + 12] = 0x00;
    block[offset + 13] = 0x00;
    // Byte 14: upper nibbles of image size
    block[offset + 14] = 0x00;
    // Byte 15: horizontal border
    block[offset + 15] = 0x00;
    // Byte 16: vertical border
    block[offset + 16] = 0x00;
    // Byte 17: flags (non-interlaced, normal display, digital separate sync)
    block[offset + 17] = 0x1E;
  }

  /**
   * @brief Fix the checksum byte of an EDID block.
   *
   * Sets byte 127 so that the sum of all 128 bytes is 0 mod 256.
   */
  static void fix_checksum(std::span<std::uint8_t, k_edid_block_size> block) {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < 127; ++i) {
      sum += block[i];
    }
    block[127] = static_cast<std::uint8_t>(256 - sum);
  }

  std::vector<std::uint8_t> generate_edid_base_block(
    std::uint16_t h_active,
    std::uint16_t v_active,
    std::uint16_t h_blanking,
    std::uint16_t v_blanking,
    std::uint16_t pixel_clock_10khz) noexcept {
    std::vector<std::uint8_t> block(k_edid_block_size, 0);

    write_edid_header(block);

    // Manufacturer ID (bytes 8-9): "SUN" encoded
    block[8] = 0x4E;   // S=19, U=21 -> (19<<2)|(21>>3) = 0x4E
    block[9] = 0xAE;   // U=21, N=14 -> ((21&7)<<5)|14 = 0xAE
    // Product code (bytes 10-11)
    block[10] = 0x01;
    block[11] = 0x00;
    // Serial (bytes 12-15)
    block[12] = 0x01;
    block[13] = 0x00;
    block[14] = 0x00;
    block[15] = 0x00;
    // Week, Year (bytes 16-17)
    block[16] = 0x01;
    block[17] = 0x1F;  // 2021
    // EDID version 1.4 (bytes 18-19)
    block[18] = 0x01;
    block[19] = 0x04;
    // Basic display parameters (byte 20): digital input, 8 bpc
    block[20] = 0xA5;
    // Max H/V image size in cm (bytes 21-22)
    block[21] = 0x00;
    block[22] = 0x00;
    // Gamma (byte 23)
    block[23] = 0x78;  // 2.2
    // Feature support (byte 24)
    block[24] = 0x06;
    // Chromaticity (bytes 25-34): all zeros for minimal fixture
    // Established timings (bytes 35-37): none
    // Standard timings (bytes 38-53): unused (0101 pattern)
    for (int i = 38; i < 54; i += 2) {
      block[i] = 0x01;
      block[i + 1] = 0x01;
    }

    // First detailed timing descriptor at offset 54
    write_timing_descriptor(block, 54, h_active, v_active, h_blanking, v_blanking, pixel_clock_10khz);

    // Number of extensions (byte 126)
    block[126] = 0;

    // Fix checksum at byte 127
    fix_checksum(std::span<std::uint8_t, k_edid_block_size>{block.data(), k_edid_block_size});

    return block;
  }

  std::vector<std::uint8_t> with_cta_lpcm_audio_extension(
    std::span<const std::uint8_t> base_edid,
    std::uint8_t native_cta_vic) {
    if (base_edid.size() != k_edid_block_size) {
      return {};
    }

    std::span<const std::uint8_t, k_edid_block_size> base{base_edid.data(), k_edid_block_size};
    if (!validate_block_checksum(base)) {
      return {};
    }

    std::vector<std::uint8_t> edid;
    edid.reserve(k_edid_block_size * 2U);
    edid.insert(edid.end(), base_edid.begin(), base_edid.end());
    edid.resize(k_edid_block_size * 2U, 0);

    auto base_block = std::span<std::uint8_t, k_edid_block_size>{edid.data(), k_edid_block_size};
    base_block[126] = 1;  // One CTA-861 extension follows the base block.
    fix_checksum(base_block);

    auto extension = std::span<std::uint8_t, k_edid_block_size>{edid.data() + k_edid_block_size, k_edid_block_size};
    extension[0] = 0x02;  // CTA-861 extension tag.
    extension[1] = 0x03;  // CTA-861 revision 3.
    extension[2] = native_cta_vic == 0 ? 18 : 20;  // Data-block collection end.
    extension[3] = 0x40;  // Basic audio is supported.
    auto offset = std::size_t{4};
    if (native_cta_vic != 0) {
      extension[offset++] = 0x41;  // Video data block with one descriptor.
      extension[offset++] = native_cta_vic | 0x80U;  // Mark the mode native.
    }
    extension[offset++] = 0x23;  // Audio data block with three payload bytes.
    extension[offset++] = 0x09;  // LPCM, two channels.
    extension[offset++] = 0x07;  // 32, 44.1, and 48 kHz.
    extension[offset++] = 0x07;  // 16, 20, and 24-bit samples.
    extension[offset++] = 0x83;  // Speaker allocation block with three payload bytes.
    extension[offset++] = 0x01;  // Front left and front right speakers.
    offset += 2;  // The remaining speaker-allocation payload bytes are zero.
    extension[offset++] = 0x65;  // HDMI vendor-specific data block, five bytes.
    extension[offset++] = 0x03;  // HDMI licensing OUI, least significant byte.
    extension[offset++] = 0x0c;
    extension[offset++] = 0x00;
    extension[offset++] = 0x10;  // Physical address 1.0.0.0.
    extension[offset++] = 0x00;

    fix_checksum(extension);
    return edid;
  }

  std::vector<std::uint8_t> make_720p_edid() noexcept {
    // 1280x720@60Hz: pixel clock 74.25 MHz = 7425 * 10kHz
    // H: 1280 active + 370 blanking = 1650 total
    // V: 720 active + 30 blanking = 750 total
    return generate_edid_base_block(1280, 720, 370, 30, 7425);
  }

  std::vector<std::uint8_t> make_1080p_edid() noexcept {
    // 1920x1080@60Hz: pixel clock 148.5 MHz = 14850 * 10kHz
    // H: 1920 active + 280 blanking = 2200 total
    // V: 1080 active + 45 blanking = 1125 total
    return generate_edid_base_block(1920, 1080, 280, 45, 14850);
  }

  std::vector<std::uint8_t> make_1440p_edid() noexcept {
    // 2560x1440@60Hz: pixel clock 241.5 MHz = 24150 * 10kHz
    // H: 2560 active + 160 blanking = 2720 total
    // V: 1440 active + 49 blanking = 1489 total
    return generate_edid_base_block(2560, 1440, 160, 49, 24150);
  }

  std::vector<std::uint8_t> make_2160p_edid() noexcept {
    // 3840x2160@30Hz: pixel clock 297 MHz = 29700 * 10kHz
    // H: 3840 active + 560 blanking = 4400 total
    // V: 2160 active + 90 blanking = 2250 total
    return generate_edid_base_block(3840, 2160, 560, 90, 29700);
  }

  std::vector<std::uint8_t> restrict_edid_to_resolution(
    std::span<const std::uint8_t> source_edid,
    const platf::hdmirx::resolution_t &target) noexcept {
    if (!validate_edid_checksums(source_edid)) {
      return {};
    }
    const auto source_modes = parse_edid_modes(source_edid);
    if (std::none_of(source_modes.begin(), source_modes.end(), [&target](const auto &mode) {
          return mode.resolution == target;
        })) {
      return {};
    }

    std::vector<std::uint8_t> target_base;
    if (target == platf::hdmirx::resolution_t {1280, 720}) {
      target_base = make_720p_edid();
    } else if (target == platf::hdmirx::resolution_t {1920, 1080}) {
      target_base = make_1080p_edid();
    } else if (target == platf::hdmirx::resolution_t {2560, 1440}) {
      target_base = make_1440p_edid();
    } else if (target == platf::hdmirx::resolution_t {3840, 2160}) {
      target_base = make_2160p_edid();
    } else {
      return {};
    }

    const auto block_count = static_cast<std::size_t>(source_edid[126]) + 1U;
    if (block_count > k_max_edid_blocks || source_edid.size() != block_count * k_edid_block_size) {
      return {};
    }
    std::vector<std::uint8_t> restricted(source_edid.begin(), source_edid.end());
    auto base = std::span<std::uint8_t, k_edid_block_size> {restricted.data(), k_edid_block_size};

    // Keep the real receiver identity and monitor descriptors, but make the
    // selected timing the only base-block video mode and mark it preferred.
    base[24] |= 0x02U;
    base[35] = 0;
    base[36] = 0;
    base[37] = 0;
    for (std::size_t offset = 38; offset < 54; offset += 2) {
      base[offset] = 0x01;
      base[offset + 1] = 0x01;
    }
    std::copy_n(target_base.begin() + 54, 18, base.begin() + 54);
    for (std::size_t offset = 72; offset <= 108; offset += 18) {
      if (base[offset] != 0 || base[offset + 1] != 0) {
        std::fill_n(base.begin() + offset, 18, std::uint8_t {0});
      }
    }
    fix_checksum(base);

    const auto target_dtd = std::span<const std::uint8_t, 18> {target_base.data() + 54, 18};
    for (std::size_t block_index = 1; block_index < block_count; ++block_index) {
      auto extension = std::span<std::uint8_t, k_edid_block_size> {
        restricted.data() + block_index * k_edid_block_size,
        k_edid_block_size
      };
      if (extension[0] != 0x02U) {
        continue;
      }

      const auto data_end = extension[2] == 0 ? std::size_t {4} : std::min<std::size_t>(extension[2], 127);
      if (data_end < 4) {
        return {};
      }
      std::vector<std::uint8_t> data_blocks;
      for (std::size_t offset = 4; offset < data_end;) {
        const auto header = extension[offset++];
        const auto tag = header >> 5U;
        const auto length = static_cast<std::size_t>(header & 0x1fU);
        if (length > data_end - offset) {
          return {};
        }
        const bool y420_video_block = tag == 0x07U && length > 1U && extension[offset] == 0x0eU;
        const bool y420_capability_map = tag == 0x07U && length > 0U && extension[offset] == 0x0fU;
        if (tag != 0x02U && !y420_video_block && !y420_capability_map) {
          data_blocks.push_back(header);
          data_blocks.insert(data_blocks.end(), extension.begin() + offset, extension.begin() + offset + length);
          offset += length;
          continue;
        }

        // A YCbCr 4:2:0 Capability Map indexes the original normal VDB. Once
        // that VDB is filtered and reordered, retaining the map would attach
        // 4:2:0 capability to unrelated modes. Dedicated Y420 video blocks
        // below retain any target-specific 4:2:0 VICs without stale indexes.
        if (y420_capability_map) {
          offset += length;
          continue;
        }

        std::vector<std::uint8_t> target_svds;
        const auto first_vic = y420_video_block ? std::size_t {1} : std::size_t {0};
        for (std::size_t index = first_vic; index < length; ++index) {
          const auto vic = static_cast<std::uint8_t>(extension[offset + index] & 0x7fU);
          if (const auto mode = cta_vic_to_hdmi_mode(vic); mode && mode->resolution == target) {
            target_svds.push_back(vic);
          }
        }
        if (!target_svds.empty()) {
          if (y420_video_block) {
            data_blocks.push_back(static_cast<std::uint8_t>(0xe0U | (target_svds.size() + 1U)));
            data_blocks.push_back(0x0eU);
            data_blocks.insert(data_blocks.end(), target_svds.begin(), target_svds.end());
          } else {
            const auto preferred = std::max_element(target_svds.begin(), target_svds.end(), [](std::uint8_t left, std::uint8_t right) {
              const auto left_mode = cta_vic_to_hdmi_mode(left);
              const auto right_mode = cta_vic_to_hdmi_mode(right);
              return static_cast<std::uint64_t>(left_mode->refresh_rate.numerator) * right_mode->refresh_rate.denominator <
                     static_cast<std::uint64_t>(right_mode->refresh_rate.numerator) * left_mode->refresh_rate.denominator;
            });
            data_blocks.push_back(static_cast<std::uint8_t>(0x40U | target_svds.size()));
            for (const auto vic : target_svds) {
              data_blocks.push_back(static_cast<std::uint8_t>(vic | (vic == *preferred ? 0x80U : 0U)));
            }
          }
        }
        offset += length;
      }

      if (4U + data_blocks.size() >= 127U) {
        return {};
      }
      std::array<std::uint8_t, k_edid_block_size> rewritten {};
      rewritten[0] = extension[0];
      rewritten[1] = extension[1];
      rewritten[3] = extension[3] & 0xf0U;
      std::copy(data_blocks.begin(), data_blocks.end(), rewritten.begin() + 4);
      const auto dtd_offset = 4U + data_blocks.size();
      rewritten[2] = static_cast<std::uint8_t>(dtd_offset);
      if (dtd_offset + target_dtd.size() <= 127U) {
        std::copy(target_dtd.begin(), target_dtd.end(), rewritten.begin() + dtd_offset);
        rewritten[3] |= 0x01U;
      }
      fix_checksum(std::span<std::uint8_t, k_edid_block_size> {rewritten.data(), rewritten.size()});
      std::copy(rewritten.begin(), rewritten.end(), extension.begin());
    }

    return validate_edid_checksums(restricted) ? restricted : std::vector<std::uint8_t> {};
  }

  std::vector<std::uint8_t> make_bad_checksum_edid() noexcept {
    auto edid = make_1080p_edid();
    // Corrupt the checksum byte.
    edid[127] ^= 0xFF;
    return edid;
  }

  std::vector<std::uint8_t> make_truncated_edid() noexcept {
    auto edid = make_1080p_edid();
    edid.resize(64);
    return edid;
  }

  // -------------------------------------------------------------------------
  // High-level EDID device operations
  // -------------------------------------------------------------------------

  edid_result_t<std::vector<std::uint8_t>> read_edid(
    ioctl_backend_t &backend,
    std::uint32_t pad) {
    // The base block declares the number of extension blocks at byte 126.
    // Read it first, then request exactly the complete EDID.  In particular,
    // never treat a short read of a multi-block EDID as a valid backup: doing
    // so would make a later restore truncate the device's original EDID.
    std::vector<std::uint8_t> base(k_edid_block_size, 0);
    auto base_result = backend.get_edid(pad, 0, 1,
                                        std::span<std::uint8_t>{base});
    if (!base_result.has_value()) {
      return std::unexpected(base_result.error());
    }
    if (base_result.value() == 0) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::no_data,
        .raw_errno = ENODATA,
        .message = "read returned zero blocks",
      });
    }
    if (base_result.value() != 1) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::io_error,
        .raw_errno = EIO,
        .message = "base EDID read returned an unexpected block count",
      });
    }

    const auto block_count = static_cast<std::uint32_t>(base[126]) + 1U;
    if (block_count > k_max_edid_blocks) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::too_large,
        .raw_errno = E2BIG,
        .message = "EDID extension count exceeds supported maximum",
      });
    }
    if (block_count == 1) {
      return base;
    }

    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(block_count) * k_edid_block_size, 0);
    auto result = backend.get_edid(pad, 0, block_count,
                                   std::span<std::uint8_t>{buffer});
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    if (result.value() != block_count) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::io_error,
        .raw_errno = EIO,
        .message = "incomplete multi-block EDID read; original was not saved",
      });
    }

    return buffer;
  }

  edid_result_t<std::uint32_t> write_edid(
    ioctl_backend_t &backend,
    std::uint32_t pad,
    std::span<const std::uint8_t> data) {
    if (data.empty() || data.size() % k_edid_block_size != 0) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::invalid_argument,
        .raw_errno = EINVAL,
        .message = "EDID data size is not a multiple of 128 bytes",
      });
    }

    const auto block_count = static_cast<std::uint32_t>(data.size() / k_edid_block_size);
    if (block_count > k_max_edid_blocks) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::too_large,
        .raw_errno = E2BIG,
        .message = "EDID exceeds maximum block count",
      });
    }

    auto result = backend.set_edid(pad, 0, block_count, data);
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    if (result.value() != block_count) {
      return std::unexpected(edid_error_t{
        .category = error_category_e::io_error,
        .raw_errno = EIO,
        .message = "partial EDID write; restoration is required",
      });
    }
    return result;
  }

  edid_capability_t probe_capabilities(
    ioctl_backend_t &backend,
    std::uint32_t pad) {
    edid_capability_t cap;

    // This must remain a read-only probe.  A write "probe" is unsafe because
    // it can overwrite an EDID before a complete original is saved.
    auto read_result = read_edid(backend, pad);
    if (read_result.has_value()) {
      cap.readable = true;
    }

    return cap;
  }

  // -------------------------------------------------------------------------
  // RAII session restore guard
  // -------------------------------------------------------------------------

  edid_restore_guard_t::edid_restore_guard_t(
    ioctl_backend_t &backend,
    std::uint32_t pad,
    log_callback_t logger)
      : backend_(&backend), pad_(pad), logger_(std::move(logger)) {
    auto result = read_edid(backend, pad);
    if (result.has_value()) {
      saved_edid_ = std::move(result.value());
      armed_ = true;
      log("EDID restore guard armed: saved " + std::to_string(saved_edid_.size()) + " bytes");
    } else {
      log("EDID restore guard disarmed: read failed (" +
          std::string(error_category_name(result.error().category)) +
          "): " + result.error().message);
    }
  }

  edid_restore_guard_t::edid_restore_guard_t(edid_restore_guard_t &&other) noexcept
      : backend_(other.backend_),
        pad_(other.pad_),
        saved_edid_(std::move(other.saved_edid_)),
        armed_(other.armed_),
        logger_(std::move(other.logger_)) {
    other.armed_ = false;
    other.backend_ = nullptr;
  }

  edid_restore_guard_t &edid_restore_guard_t::operator=(edid_restore_guard_t &&other) noexcept {
    if (this != &other) {
      restore();
      backend_ = other.backend_;
      pad_ = other.pad_;
      saved_edid_ = std::move(other.saved_edid_);
      armed_ = other.armed_;
      logger_ = std::move(other.logger_);
      other.armed_ = false;
      other.backend_ = nullptr;
    }
    return *this;
  }

  edid_restore_guard_t::~edid_restore_guard_t() {
    restore();
  }

  bool edid_restore_guard_t::restore() noexcept {
    if (!armed_) {
      return true;
    }

    try {
      auto result = write_edid(*backend_, pad_,
                               std::span<const std::uint8_t>{saved_edid_});
      if (result.has_value()) {
        armed_ = false;
        log("EDID restore succeeded");
        return true;
      } else {
        log("EDID restore failed (" +
            std::string(error_category_name(result.error().category)) +
            "): " + result.error().message);
        // Keep armed_ true so destructor can try again.
        return false;
      }
    } catch (...) {
      log("EDID restore threw an unexpected exception");
      return false;
    }
  }

  bool edid_restore_guard_t::is_armed() const noexcept {
    return armed_;
  }

  const std::vector<std::uint8_t> &edid_restore_guard_t::saved_edid() const noexcept {
    return saved_edid_;
  }

  void edid_restore_guard_t::log(const std::string &msg) const {
    if (logger_) {
      logger_(msg);
    }
  }

}  // namespace platf::edid
