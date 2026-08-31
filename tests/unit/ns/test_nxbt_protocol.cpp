/**
 * @file tests/unit/ns/test_nxbt_protocol.cpp
 * @brief Unit tests for the versioned NXBT Bridge wire protocol.
 */

// standard includes
#include <cstdint>
#include <string_view>
#include <vector>

// local includes
#include "tests/tests_common.h"
#include "src/input/nxbt_protocol.h"

namespace {
  /**
   * @brief Convert an even-length lower-case hexadecimal string into bytes.
   *
   * @param hex Hexadecimal source text.
   * @return Decoded bytes.
   */
  std::vector<std::uint8_t> from_hex(std::string_view hex) {
    const auto nibble = [](char value) -> std::uint8_t {
      return static_cast<std::uint8_t>(value >= 'a' ? value - 'a' + 10 : value - '0');
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
      bytes.push_back(static_cast<std::uint8_t>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
    }
    return bytes;
  }

  /**
   * @brief Compare all controller-state fields used by the wire protocol.
   *
   * @param expected Expected state.
   * @param actual Decoded state.
   */
  void expect_state_eq(const input::nxbt::controller_state_t &expected, const input::nxbt::controller_state_t &actual) {
    EXPECT_EQ(actual.controller_id, expected.controller_id);
    EXPECT_EQ(actual.button_flags, expected.button_flags);
    EXPECT_EQ(actual.left_trigger, expected.left_trigger);
    EXPECT_EQ(actual.right_trigger, expected.right_trigger);
    EXPECT_EQ(actual.left_stick_x, expected.left_stick_x);
    EXPECT_EQ(actual.left_stick_y, expected.left_stick_y);
    EXPECT_EQ(actual.right_stick_x, expected.right_stick_x);
    EXPECT_EQ(actual.right_stick_y, expected.right_stick_y);
    EXPECT_EQ(actual.sequence, expected.sequence);
    EXPECT_EQ(actual.monotonic_timestamp_us, expected.monotonic_timestamp_us);
  }
}  // namespace

TEST(NxbtProtocolTest, EncodesAndDecodesGoldenVectors) {
  using namespace input::nxbt;
  const controller_state_t state {7, 0x0003ffffU, 255, 128, -32768, 32767, -1, 1, 0xfffffffeU, 0x0102030405060708ULL};
  const std::vector<std::pair<message_t, std::string_view>> vectors {
    {{message_type_e::hello, 0x11223344U}, "4e584254010001000400000044332211"},
    {{message_type_e::hello_ack, 0}, "4e584254010002000400000000000000"},
    {{message_type_e::error, 0, 0, 0, {}, 0, controller_status_e::unavailable, protocol_error_e::invalid_length}, "4e584254010003000400000003000000"},
    {{message_type_e::attach, 0, 7, 2}, "4e584254010004000400000007020000"},
    {{message_type_e::rebind, 0, 7, 3}, "4e584254010005000400000007030000"},
    {{message_type_e::state, 0, 0, 0, state}, "4e584254010006001c00000007ff8000ffff03000080ff7fffff0100feffffff0807060504030201"},
    {{message_type_e::neutralize, 0, 7}, "4e584254010007000400000007000000"},
    {{message_type_e::detach, 0, 7}, "4e584254010008000400000007000000"},
    {{message_type_e::ping, 0, 0, 0, {}, 0x0102030405060708ULL}, "4e58425401000900080000000807060504030201"},
    {{message_type_e::pong, 0, 0, 0, {}, 0x0102030405060708ULL}, "4e58425401000a00080000000807060504030201"},
    {{message_type_e::status, 0, 7, 0, {}, 0, controller_status_e::connected}, "4e58425401000b000400000007030000"},
  };

  for (const auto &[message, expected_hex] : vectors) {
    SCOPED_TRACE(static_cast<int>(message.type));
    const auto encoded = encode_message(message);
    EXPECT_EQ(encoded, from_hex(expected_hex));
    const auto decoded = decode_message(encoded);
    ASSERT_EQ(decoded.error, protocol_error_e::none);
    EXPECT_EQ(decoded.message.type, message.type);
    EXPECT_EQ(decoded.message.capabilities, message.capabilities);
    EXPECT_EQ(decoded.message.controller_id, message.controller_id);
    EXPECT_EQ(decoded.message.client_relative_id, message.client_relative_id);
    EXPECT_EQ(decoded.message.monotonic_timestamp_us, message.monotonic_timestamp_us);
    EXPECT_EQ(decoded.message.status, message.status);
    EXPECT_EQ(decoded.message.error, message.error);
    if (message.type == message_type_e::state) {
      expect_state_eq(message.state, decoded.message.state);
    }
  }
}

TEST(NxbtProtocolTest, RejectsMalformedPackets) {
  using namespace input::nxbt;
  const auto valid = encode_message({message_type_e::hello});
  auto bad_magic = valid;
  bad_magic[0] = 0;
  EXPECT_EQ(decode_message(bad_magic).error, protocol_error_e::bad_magic);

  auto bad_version = valid;
  bad_version[4] = 2;
  EXPECT_EQ(decode_message(bad_version).error, protocol_error_e::unsupported_version);

  auto bad_type = valid;
  bad_type[6] = 255;
  EXPECT_EQ(decode_message(bad_type).error, protocol_error_e::unknown_message_type);

  auto bad_length = valid;
  bad_length[8] = 5;
  EXPECT_EQ(decode_message(bad_length).error, protocol_error_e::invalid_length);

  auto truncated_payload = valid;
  truncated_payload.pop_back();
  EXPECT_EQ(decode_message(truncated_payload).error, protocol_error_e::truncated);

  auto trailing_byte = valid;
  trailing_byte.push_back(0);
  EXPECT_EQ(decode_message(trailing_byte).error, protocol_error_e::invalid_length);

  EXPECT_EQ(decode_message({}).error, protocol_error_e::truncated);
}

TEST(NxbtProtocolTest, RejectsPacketEncodedWithAnUnknownMessageType) {
  using namespace input::nxbt;
  const auto unknown_type = static_cast<message_type_e>(255);
  const auto packet = encode_message({unknown_type});
  EXPECT_EQ(packet.size(), message_header_size);
  EXPECT_EQ(decode_message(packet).error, protocol_error_e::unknown_message_type);
}

TEST(NxbtProtocolTest, ComparesSequenceNumbersAcrossWraparound) {
  using input::nxbt::sequence_is_newer;
  EXPECT_TRUE(sequence_is_newer(0, 0xffffffffU));
  EXPECT_TRUE(sequence_is_newer(1, 0xfffffffeU));
  EXPECT_FALSE(sequence_is_newer(0xffffffffU, 0));
  EXPECT_FALSE(sequence_is_newer(5, 5));
  EXPECT_FALSE(sequence_is_newer(0x80000000U, 0));
}
