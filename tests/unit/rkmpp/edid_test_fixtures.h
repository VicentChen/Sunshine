/**
 * @file tests/unit/rkmpp/edid_test_fixtures.h
 * @brief Test-only native EDID builders for RKMPP unit tests.
 */
#pragma once

#include "src/platform/linux/edid.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace edid_test {

  /**
   * @brief Repair the checksum of one mutable EDID block.
   *
   * @param block Block to update.
   */
  inline void fix_checksum(std::span<std::uint8_t, platf::edid::k_edid_block_size> block) {
    std::uint8_t sum = 0;
    for (std::size_t index = 0; index < 127; ++index) {
      sum += block[index];
    }
    block[127] = static_cast<std::uint8_t>(0U - sum);
  }

  /**
   * @brief Write a complete detailed timing descriptor.
   *
   * @param block Destination block.
   * @param offset Descriptor offset.
   * @param width Active width.
   * @param height Active height.
   * @param h_blanking Horizontal blanking.
   * @param v_blanking Vertical blanking.
   * @param pixel_clock_10khz Pixel clock in 10 kHz units.
   * @param interlaced Whether to set the interlaced flag.
   */
  inline void write_dtd(
    std::span<std::uint8_t, platf::edid::k_edid_block_size> block,
    std::size_t offset,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t h_blanking,
    std::uint16_t v_blanking,
    std::uint16_t pixel_clock_10khz,
    bool interlaced = false
  ) {
    block[offset] = static_cast<std::uint8_t>(pixel_clock_10khz & 0xffU);
    block[offset + 1] = static_cast<std::uint8_t>(pixel_clock_10khz >> 8U);
    block[offset + 2] = static_cast<std::uint8_t>(width & 0xffU);
    block[offset + 3] = static_cast<std::uint8_t>(h_blanking & 0xffU);
    block[offset + 4] = static_cast<std::uint8_t>(((width >> 8U) << 4U) | (h_blanking >> 8U));
    block[offset + 5] = static_cast<std::uint8_t>(height & 0xffU);
    block[offset + 6] = static_cast<std::uint8_t>(v_blanking & 0xffU);
    block[offset + 7] = static_cast<std::uint8_t>(((height >> 8U) << 4U) | (v_blanking >> 8U));
    block[offset + 8] = 88;
    block[offset + 9] = 44;
    block[offset + 10] = 0x45;
    block[offset + 17] = static_cast<std::uint8_t>(0x1eU | (interlaced ? 0x80U : 0U));
  }

  /**
   * @brief Build a valid base EDID with one preferred native DTD.
   *
   * @param width Active width.
   * @param height Active height.
   * @param h_blanking Horizontal blanking.
   * @param v_blanking Vertical blanking.
   * @param pixel_clock_10khz Pixel clock in 10 kHz units.
   * @return One valid EDID base block.
   */
  inline std::vector<std::uint8_t> make_base_edid(
    std::uint16_t width = 1920,
    std::uint16_t height = 1080,
    std::uint16_t h_blanking = 280,
    std::uint16_t v_blanking = 45,
    std::uint16_t pixel_clock_10khz = 14850
  ) {
    std::vector<std::uint8_t> edid(platf::edid::k_edid_block_size, 0);
    constexpr std::array<std::uint8_t, 8> header {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::copy(header.begin(), header.end(), edid.begin());
    edid[8] = 0x4e;
    edid[9] = 0xae;
    edid[18] = 1;
    edid[19] = 4;
    edid[20] = 0xa5;
    edid[23] = 0x78;
    edid[24] = 0x06;
    for (std::size_t offset = 38; offset < 54; offset += 2) {
      edid[offset] = 0x01;
      edid[offset + 1] = 0x01;
    }
    auto block = std::span<std::uint8_t, platf::edid::k_edid_block_size> {edid.data(), edid.size()};
    write_dtd(block, 54, width, height, h_blanking, v_blanking, pixel_clock_10khz);
    fix_checksum(block);
    return edid;
  }

  /**
   * @brief Build a realistic two-block native receiver EDID with mixed modes.
   *
   * The fixture includes established, standard, base DTD, CTA VDB, Y420 VDB,
   * CTA DTD, audio, speaker allocation, and HDMI vendor capabilities.
   *
   * @return Complete valid native EDID.
   */
  inline std::vector<std::uint8_t> make_native_edid() {
    auto edid = make_base_edid();
    edid.resize(platf::edid::k_edid_block_size * 2U, 0);
    auto base = std::span<std::uint8_t, platf::edid::k_edid_block_size> {edid.data(), platf::edid::k_edid_block_size};
    base[35] = 0x21U;  // 640x480@60 and 800x600@60.
    base[36] = 0x08U;  // 1024x768@60.
    base[38] = 0xa9U;  // 1600 pixels.
    base[39] = 0xc0U;  // 16:9 at 60 Hz => 1600x900.
    base[72] = 0;
    base[73] = 0;
    base[75] = 0xfc;
    base[77] = 'R';
    base[78] = 'K';
    base[79] = 'M';
    base[80] = 'P';
    base[81] = 'P';
    base[126] = 1;
    fix_checksum(base);

    auto cta = std::span<std::uint8_t, platf::edid::k_edid_block_size> {
      edid.data() + platf::edid::k_edid_block_size,
      platf::edid::k_edid_block_size
    };
    cta[0] = 0x02;
    cta[1] = 0x03;
    std::size_t offset = 4;
    cta[offset++] = 0x43;
    cta[offset++] = 0x90;  // Native 1920x1080p60.
    cta[offset++] = 4;  // 1280x720p60.
    cta[offset++] = 97;  // 3840x2160p60.
    cta[offset++] = 0xe2;
    cta[offset++] = 0x0e;
    cta[offset++] = 97;
    cta[offset++] = 0x23;
    cta[offset++] = 0x09;
    cta[offset++] = 0x07;
    cta[offset++] = 0x07;
    cta[offset++] = 0x83;
    cta[offset++] = 0x01;
    cta[offset++] = 0;
    cta[offset++] = 0;
    cta[offset++] = 0x65;
    cta[offset++] = 0x03;
    cta[offset++] = 0x0c;
    cta[offset++] = 0x00;
    cta[offset++] = 0x10;
    cta[offset++] = 0;
    cta[2] = static_cast<std::uint8_t>(offset);
    cta[3] = 0x40;
    write_dtd(cta, offset, 1280, 720, 370, 30, 7425);
    fix_checksum(cta);
    return edid;
  }

  /**
   * @brief Return the Rockchip 340 MHz receiver EDID used by the target board.
   *
   * The real profile retains 1080p and other safe modes while advertising
   * 2160p50/60 through CTA and YCbCr 4:2:0 video blocks. It also contains CTA
   * VIC 7 and 22 interlaced compatibility modes.
   *
   * @return Complete valid two-block receiver EDID.
   */
  inline std::vector<std::uint8_t> make_rockchip_340mhz_edid() {
    return {
      0x00,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0xff,
      0x00,
      0x49,
      0x70,
      0x88,
      0x35,
      0x01,
      0x00,
      0x00,
      0x00,
      0x2d,
      0x1f,
      0x01,
      0x03,
      0x80,
      0x78,
      0x44,
      0x78,
      0x0a,
      0xcf,
      0x74,
      0xa3,
      0x57,
      0x4c,
      0xb0,
      0x23,
      0x09,
      0x48,
      0x4c,
      0x21,
      0x08,
      0x00,
      0x61,
      0x40,
      0x01,
      0x01,
      0x81,
      0x00,
      0x95,
      0x00,
      0xa9,
      0xc0,
      0x01,
      0x01,
      0x01,
      0x01,
      0x01,
      0x01,
      0x02,
      0x3a,
      0x80,
      0x18,
      0x71,
      0x38,
      0x2d,
      0x40,
      0x58,
      0x2c,
      0x45,
      0x00,
      0x20,
      0xc2,
      0x31,
      0x00,
      0x00,
      0x1e,
      0x01,
      0x1d,
      0x00,
      0x72,
      0x51,
      0xd0,
      0x1e,
      0x20,
      0x6e,
      0x28,
      0x55,
      0x00,
      0x20,
      0xc2,
      0x31,
      0x00,
      0x00,
      0x1e,
      0x00,
      0x00,
      0x00,
      0xfc,
      0x00,
      0x52,
      0x4b,
      0x2d,
      0x55,
      0x48,
      0x44,
      0x0a,
      0x20,
      0x20,
      0x20,
      0x20,
      0x20,
      0x20,
      0x00,
      0x00,
      0x00,
      0xfd,
      0x00,
      0x3b,
      0x46,
      0x1f,
      0x8c,
      0x3c,
      0x00,
      0x0a,
      0x20,
      0x20,
      0x20,
      0x20,
      0x20,
      0x20,
      0x01,
      0xa7,
      0x02,
      0x03,
      0x2e,
      0xf1,
      0x51,
      0x07,
      0x16,
      0x14,
      0x05,
      0x01,
      0x03,
      0x12,
      0x13,
      0x84,
      0x22,
      0x1f,
      0x90,
      0x5d,
      0x5e,
      0x5f,
      0x60,
      0x61,
      0x23,
      0x09,
      0x07,
      0x07,
      0x83,
      0x01,
      0x00,
      0x00,
      0x67,
      0x03,
      0x0c,
      0x00,
      0x30,
      0x00,
      0x00,
      0x44,
      0xe3,
      0x05,
      0x03,
      0x01,
      0xe3,
      0x0e,
      0x60,
      0x61,
      0x02,
      0x3a,
      0x80,
      0x18,
      0x71,
      0x38,
      0x2d,
      0x40,
      0x58,
      0x2c,
      0x45,
      0x00,
      0x20,
      0xc2,
      0x31,
      0x00,
      0x00,
      0x1e,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0xd2,
    };
  }

  /**
   * @brief Return a fixture with a deliberately invalid checksum.
   *
   * @return Corrupted EDID.
   */
  inline std::vector<std::uint8_t> make_bad_checksum_edid() {
    auto edid = make_base_edid();
    edid.back() ^= 0xffU;
    return edid;
  }

}  // namespace edid_test
