/**
 * @file tests/unit/xbox/test_xbox_remote_protocol.cpp
 * @brief Contract tests for the Xbox Home Remote Play protocol layer.
 */

// standard includes
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// library includes
#include <nlohmann/json.hpp>

// local includes
#include "tests/tests_common.h"
#include "src/xbox_remote/protocol.h"

namespace {
  using json = nlohmann::json;

  /**
   * @brief Resolve a fixture relative to the Sunshine source directory.
   *
   * @param name Fixture file name.
   * @return Absolute fixture path.
   */
  std::filesystem::path fixture_path(std::string_view name) {
    return std::filesystem::path(SUNSHINE_SOURCE_DIR) / "tests" / "fixtures" / "xbox_remote" / name;
  }

  /**
   * @brief Read a complete text fixture.
   *
   * @param name Fixture file name.
   * @return Fixture contents.
   */
  std::string read_fixture(std::string_view name) {
    std::ifstream stream(fixture_path(name), std::ios::binary);
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
  }

  /**
   * @brief Decode a hexadecimal fixture into wire bytes.
   *
   * @param name Fixture file name.
   * @return Decoded bytes.
   */
  std::vector<std::uint8_t> read_hex_fixture(std::string_view name) {
    const auto source = read_fixture(name);
    const auto nibble = [](char value) -> std::uint8_t {
      if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
      }
      return static_cast<std::uint8_t>(value - 'a' + 10);
    };

    std::string hex;
    for (const char value : source) {
      if (!std::isspace(static_cast<unsigned char>(value))) {
        hex.push_back(value);
      }
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
      bytes.push_back(static_cast<std::uint8_t>((nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
    }
    return bytes;
  }

  /**
   * @brief Normalize startup envelope content from its wire string into JSON.
   *
   * @param envelope Parsed startup message envelope.
   * @return Envelope with semantic JSON content.
   */
  json normalize_startup_envelope(json envelope) {
    envelope["content"] = json::parse(envelope.at("content").get<std::string>());
    return envelope;
  }
}  // namespace

TEST(XboxRemoteProtocolTest, FreezesRequiredDataChannels) {
  using xbox_remote::protocol::data_channels;
  ASSERT_EQ(data_channels.size(), 4);
  EXPECT_EQ(data_channels[0].label, "control");
  EXPECT_EQ(data_channels[0].protocol, "controlV1");
  EXPECT_EQ(data_channels[0].sid, 0);
  EXPECT_EQ(data_channels[1].label, "input");
  EXPECT_EQ(data_channels[1].protocol, "1.0");
  EXPECT_EQ(data_channels[1].sid, 2);
  EXPECT_EQ(data_channels[2].label, "message");
  EXPECT_EQ(data_channels[2].protocol, "messageV1");
  EXPECT_EQ(data_channels[2].sid, 4);
  EXPECT_EQ(data_channels[3].label, "chat");
  EXPECT_EQ(data_channels[3].protocol, "chatV1");
  EXPECT_EQ(data_channels[3].sid, 6);
  for (const auto &channel : data_channels) {
    EXPECT_TRUE(channel.reliable);
    EXPECT_TRUE(channel.ordered);
  }
}

TEST(XboxRemoteProtocolTest, FreezesButtonAndPhysicalityMasks) {
  using xbox_remote::protocol::gamepad_button_e;
  using xbox_remote::protocol::gamepad_physicality_e;
  EXPECT_EQ(static_cast<std::uint16_t>(gamepad_button_e::nexus), 0x0002);
  EXPECT_EQ(static_cast<std::uint16_t>(gamepad_button_e::right_thumb), 0x8000);
  EXPECT_EQ(static_cast<std::uint32_t>(gamepad_physicality_e::dpad_up), 0x00000001);
  EXPECT_EQ(static_cast<std::uint32_t>(gamepad_physicality_e::misc), 0x00000800);
  EXPECT_EQ(static_cast<std::uint32_t>(gamepad_physicality_e::a), 0x00001000);
  EXPECT_EQ(static_cast<std::uint32_t>(gamepad_physicality_e::right_thumb_y), 0x00200000);
}

TEST(XboxRemoteProtocolTest, SerializesHomePlayAndSdpOfferGoldenFixtures) {
  using namespace xbox_remote::protocol;
  home_play_request_t play;
  play.server_id = "redacted-home-console";
  EXPECT_EQ(json::parse(serialize_home_play_request(play)), json::parse(read_fixture("home_play_request.json")));

  const sdp_offer_t offer {"v=0\r\ns=redacted\r\n"};
  EXPECT_EQ(json::parse(serialize_sdp_offer(offer)), json::parse(read_fixture("sdp_offer_request.json")));

  const std::vector<ice_candidate_t> candidates {
    {"candidate:fixture 1 udp 2122260223 192.0.2.20 50000 typ host", "0", 0, "fixture-ufrag"},
    {"a=end-of-candidates", "", 0, "fixture-ufrag"},
  };
  EXPECT_EQ(json::parse(serialize_ice_candidates(candidates)), json::parse(read_fixture("ice_candidates_request.json")));
}

TEST(XboxRemoteProtocolTest, ParsesHomeRestGoldenFixturesAndIgnoresUnknownFields) {
  using namespace xbox_remote::protocol;
  const auto consoles = parse_console_list(read_fixture("console_list_response.json"));
  ASSERT_TRUE(consoles);
  ASSERT_EQ(consoles.value.size(), 1);
  EXPECT_EQ(consoles.value[0].server_id, "redacted-home-console");
  EXPECT_EQ(consoles.value[0].device_name, "Fixture Xbox");
  EXPECT_EQ(consoles.value[0].console_type, "XboxSeriesX");
  EXPECT_EQ(consoles.value[0].power_state, "On");

  const auto created = parse_session_created(read_fixture("session_created_response.json"));
  ASSERT_TRUE(created);
  EXPECT_EQ(created.value.session_id, "redacted-session");

  const auto state = parse_session_state(read_fixture("session_state_response.json"));
  ASSERT_TRUE(state);
  EXPECT_EQ(state.value.state, "Provisioned");
  EXPECT_TRUE(state.value.failure_code.empty());
  EXPECT_TRUE(is_home_session_provisioned(state.value));
  EXPECT_FALSE(is_home_session_provisioned({"ReadyToConnect"}));
  EXPECT_FALSE(is_home_session_provisioned({"provisioned"}));

  const auto configuration = parse_session_configuration(read_fixture("session_configuration_response.json"));
  ASSERT_TRUE(configuration);
  EXPECT_EQ(configuration.value.ipv4_address, "192.0.2.10");
  EXPECT_EQ(configuration.value.ipv4_port, 9002);
  EXPECT_EQ(configuration.value.ice_exchange_path, "/v5/sessions/home/redacted-session/ice");
  EXPECT_TRUE(configuration.value.srtp_key.empty());
  EXPECT_EQ(configuration.value.keepalive_seconds, 300);

  const auto exchange = parse_exchange_response(read_fixture("exchange_response.json"));
  ASSERT_TRUE(exchange);
  const auto inner = json::parse(exchange.value.exchange_response);
  EXPECT_EQ(inner.at("sdpType"), "answer");
}

TEST(XboxRemoteProtocolTest, AcceptsDocumentedHomeResponseAliases) {
  using namespace xbox_remote::protocol;
  const auto created = parse_session_created(R"({"sessionPath":"/v5/sessions/home/path-session"})");
  ASSERT_TRUE(created);
  EXPECT_EQ(created.value.session_id, "path-session");

  const auto configuration = parse_session_configuration(
    R"({"serverDetails":{"ipAddress":"192.0.2.20","port":9003,"iceExchangePath":"/fixture/ice","srtp":{"key":"fixture-key"}},"keepAlivePulseInSeconds":60})"
  );
  ASSERT_TRUE(configuration);
  EXPECT_EQ(configuration.value.ipv4_address, "192.0.2.20");
  EXPECT_EQ(configuration.value.ipv4_port, 9003);
  EXPECT_EQ(configuration.value.srtp_key, "fixture-key");

  const auto consoles = parse_console_list(R"({"results":[]})");
  ASSERT_TRUE(consoles);
  EXPECT_TRUE(consoles.value.empty());

  const auto failed = parse_session_state(R"({"state":"Failed","errorDetails":{"code":"ConsoleUnavailable","message":"response-secret-fixture"}})");
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed.value.failure_code, "ConsoleUnavailable");
  EXPECT_EQ(failed.value.failure_code.find("response-secret-fixture"), std::string::npos);

  const auto root_code = parse_session_state(R"({"state":"Error","code":"RemotePlay:NotReady"})");
  ASSERT_TRUE(root_code);
  EXPECT_EQ(root_code.value.failure_code, "RemotePlay:NotReady");

  const auto unsafe_code = parse_session_state(R"({"state":"Failed","errorDetails":{"code":"identifier with spaces"}})");
  ASSERT_TRUE(unsafe_code);
  EXPECT_TRUE(unsafe_code.value.failure_code.empty());
}

TEST(XboxRemoteProtocolTest, ReturnsStructuredErrorsForInvalidHomeResponses) {
  using namespace xbox_remote::protocol;
  EXPECT_EQ(parse_console_list("{").error.code, error_code_e::invalid_json);
  EXPECT_EQ(parse_console_list("[]").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_console_list("{}").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_console_list(R"({"results":{}})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_console_list(R"({"results":[1]})").error.field, "$.results[0]");
  EXPECT_EQ(parse_console_list(R"({"results":[{}]})").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_console_list(R"({"results":[{"serverId":1}]})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_console_list(R"({"results":[{"serverId":""}]})").error.code, error_code_e::invalid_value);
  EXPECT_EQ(parse_console_list(R"({"results":[{"serverId":"fixture","deviceName":1}]})").error.field, "$.results[0].deviceName");

  EXPECT_EQ(parse_session_created("[]").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_session_created(R"({"sessionId":1})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_session_created("{}").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_session_created(R"({"sessionPath":"invalid"})").error.code, error_code_e::invalid_value);
  EXPECT_EQ(parse_session_created(R"({"sessionId":""})").error.code, error_code_e::invalid_value);

  EXPECT_EQ(parse_session_state("null").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_session_state("{}").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_session_state(R"({"state":1})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_session_state(R"({"state":""})").error.code, error_code_e::invalid_value);

  EXPECT_EQ(parse_exchange_response("[]").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_exchange_response("{}").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_exchange_response(R"({"exchangeResponse":1})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_exchange_response(R"({"exchangeResponse":""})").error.code, error_code_e::invalid_value);
}

TEST(XboxRemoteProtocolTest, ValidatesOptionalKeepaliveAndLegacyServerDetails) {
  using namespace xbox_remote::protocol;
  const std::vector<std::pair<std::string, error_code_e>> invalid {
    {"[]", error_code_e::wrong_type},
    {R"({"serverDetails":1,"keepAlivePulseInSeconds":60})", error_code_e::wrong_type},
    {R"({"serverDetails":{"ipAddress":"192.0.2.1","port":9002,"iceExchangePath":"/ice"},"keepAlivePulseInSeconds":"60"})", error_code_e::wrong_type},
    {R"({"serverDetails":{"ipAddress":"192.0.2.1","port":9002,"iceExchangePath":"/ice"},"keepAlivePulseInSeconds":0})", error_code_e::invalid_value},
  };
  for (const auto &[source, expected] : invalid) {
    SCOPED_TRACE(source);
    EXPECT_EQ(parse_session_configuration(source).error.code, expected);
  }
  EXPECT_EQ(parse_session_configuration("{").error.code, error_code_e::invalid_json);

  const auto defaults = parse_session_configuration("{}");
  ASSERT_TRUE(defaults);
  EXPECT_EQ(defaults.value.keepalive_seconds, 20);

  const auto modern = parse_session_configuration(R"({"keepAlivePulseInSeconds":60})");
  ASSERT_TRUE(modern);
  EXPECT_EQ(modern.value.keepalive_seconds, 60);
  EXPECT_TRUE(modern.value.ipv4_address.empty());
  EXPECT_EQ(modern.value.ipv4_port, 0);

  const auto ignored_legacy = parse_session_configuration(
    R"({"serverDetails":{"ipV4Address":null,"ipV4Port":0,"iceExchangePath":null,"srtp":{"key":1}},"keepAlivePulseInSeconds":60})"
  );
  ASSERT_TRUE(ignored_legacy);
  EXPECT_TRUE(ignored_legacy.value.ipv4_address.empty());
  EXPECT_EQ(ignored_legacy.value.ipv4_port, 0);
  EXPECT_TRUE(ignored_legacy.value.ice_exchange_path.empty());
  EXPECT_TRUE(ignored_legacy.value.srtp_key.empty());
}

TEST(XboxRemoteProtocolTest, FreezesHandshakeControlAndStartupMessages) {
  using namespace xbox_remote::protocol;
  EXPECT_EQ(json::parse(make_message_handshake("fixture-handshake")), json::parse(read_fixture("message_handshake.json")));
  EXPECT_EQ(json::parse(make_authorization_request()), json::parse(read_fixture("authorization_request.json")));
  EXPECT_EQ(json::parse(make_gamepad_changed(0, false)), json::parse(read_fixture("gamepad_removed.json")));
  EXPECT_EQ(json::parse(make_gamepad_changed(0, true)), json::parse(read_fixture("gamepad_added.json")));

  const auto expected = json::parse(read_fixture("startup_messages.json"));
  const auto actual = make_startup_messages({});
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_EQ(normalize_startup_envelope(json::parse(actual[index])), expected[index]);
  }
}

TEST(XboxRemoteProtocolTest, ValidatesHandshakeAcknowledgement) {
  using namespace xbox_remote::protocol;
  const auto valid = parse_message_handshake_ack(
    R"({"type":"HandshakeAck","version":"messageV1","id":"fixture","cv":"0","future":true})"
  );
  ASSERT_TRUE(valid);
  EXPECT_EQ(valid.value, "messageV1");

  EXPECT_EQ(parse_message_handshake_ack("{").error.code, error_code_e::invalid_json);
  EXPECT_EQ(parse_message_handshake_ack("[]").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_message_handshake_ack("{}").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_message_handshake_ack(R"({"type":1})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_message_handshake_ack(R"({"type":"Message"})").error.code, error_code_e::invalid_value);
  EXPECT_EQ(parse_message_handshake_ack(R"({"type":"HandshakeAck"})").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_message_handshake_ack(R"({"type":"HandshakeAck","version":1})").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_message_handshake_ack(R"({"type":"HandshakeAck","version":"messageV2"})").error.code, error_code_e::invalid_value);
}

TEST(XboxRemoteProtocolTest, ParsesInnerSdpAndIceExchangeShapes) {
  using namespace xbox_remote::protocol;
  const auto sdp = parse_sdp_exchange(R"({"sdp":"v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\n","sdpType":"answer","future":true})");
  ASSERT_TRUE(sdp);
  EXPECT_NE(sdp.value.find("m=video"), std::string::npos);

  const auto ice = parse_ice_exchange(
    R"(["{\"candidate\":\"candidate:fixture 1 udp 1 192.0.2.1 5000 typ host\",\"sdpMid\":\"video\",\"sdpMLineIndex\":\"0\",\"usernameFragment\":\"fixture\"}",{"candidate":"a=end-of-candidates","sdpMid":"","sdpMLineIndex":0}])"
  );
  ASSERT_TRUE(ice);
  ASSERT_EQ(ice.value.size(), 2);
  EXPECT_EQ(ice.value[0].sdp_mid, "video");
  EXPECT_EQ(ice.value[0].sdp_mline_index, 0);
  EXPECT_EQ(ice.value[0].username_fragment, "fixture");
  EXPECT_EQ(ice.value[1].sdp_mid, "0");
}

TEST(XboxRemoteProtocolTest, RejectsInvalidInnerSdpAndIceExchangeShapes) {
  using namespace xbox_remote::protocol;
  EXPECT_EQ(parse_sdp_exchange("[]").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_sdp_exchange("{}").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_sdp_exchange(R"({"sdp":"","sdpType":"answer"})").error.code, error_code_e::invalid_value);
  EXPECT_EQ(parse_sdp_exchange(R"({"sdp":"v=0","sdpType":"offer"})").error.field, "$.sdpType");
  EXPECT_EQ(parse_ice_exchange("{}").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_ice_exchange("[1]").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_ice_exchange(R"(["not-json"])").error.code, error_code_e::invalid_json);
  EXPECT_EQ(parse_ice_exchange(R"([{}])").error.code, error_code_e::missing_field);
  EXPECT_EQ(parse_ice_exchange(R"([{"candidate":"candidate:x","sdpMLineIndex":-1}])").error.code, error_code_e::wrong_type);
  EXPECT_EQ(parse_ice_exchange(R"([{"candidate":"candidate:x","sdpMLineIndex":70000}])").error.code, error_code_e::invalid_value);
}

TEST(XboxRemoteProtocolTest, EncodesAndDecodesLittleEndianInputGoldenFixtures) {
  using namespace xbox_remote::protocol;
  const input_header_t header {report_type_e::gamepad, 0x01020304U, 1.0};
  const gamepad_frame_t frame {
    0,
    0xa55a,
    std::numeric_limits<std::int16_t>::min(),
    std::numeric_limits<std::int16_t>::max(),
    -1,
    1,
    0,
    0xffff,
    0x00123456,
    0x89abcdef,
  };
  const auto encoded = encode_gamepad_packet(header, frame);
  EXPECT_EQ(encoded, read_hex_fixture("gamepad_packet.bin.hex"));
  ASSERT_EQ(encoded.size(), single_gamepad_packet_size);

  const auto decoded = decode_gamepad_packet(encoded);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value.first.report_type, report_type_e::gamepad);
  EXPECT_EQ(decoded.value.first.sequence, header.sequence);
  EXPECT_DOUBLE_EQ(decoded.value.first.timestamp_ms, header.timestamp_ms);
  EXPECT_EQ(decoded.value.second.gamepad_index, frame.gamepad_index);
  EXPECT_EQ(decoded.value.second.button_mask, frame.button_mask);
  EXPECT_EQ(decoded.value.second.left_stick_x, frame.left_stick_x);
  EXPECT_EQ(decoded.value.second.left_stick_y, frame.left_stick_y);
  EXPECT_EQ(decoded.value.second.right_stick_x, frame.right_stick_x);
  EXPECT_EQ(decoded.value.second.right_stick_y, frame.right_stick_y);
  EXPECT_EQ(decoded.value.second.left_trigger, frame.left_trigger);
  EXPECT_EQ(decoded.value.second.right_trigger, frame.right_trigger);
  EXPECT_EQ(decoded.value.second.physical_physicality, frame.physical_physicality);
  EXPECT_EQ(decoded.value.second.virtual_physicality, frame.virtual_physicality);

  const input_header_t metadata_header {report_type_e::gamepad, 0xa1b2c3d4U, -2.0};
  EXPECT_EQ(encode_client_metadata(metadata_header, 0), read_hex_fixture("client_metadata.bin.hex"));
}

TEST(XboxRemoteProtocolTest, RejectsInvalidGamepadPackets) {
  using namespace xbox_remote::protocol;
  EXPECT_EQ(decode_gamepad_packet({}).error.code, error_code_e::invalid_length);
  auto packet = read_hex_fixture("gamepad_packet.bin.hex");
  packet[0] = static_cast<std::uint8_t>(report_type_e::client_metadata);
  EXPECT_EQ(decode_gamepad_packet(packet).error.field, "gamepadPacket.reportType");
  packet = read_hex_fixture("gamepad_packet.bin.hex");
  packet[input_header_size] = 2;
  EXPECT_EQ(decode_gamepad_packet(packet).error.field, "gamepadPacket.frameCount");
}

TEST(XboxRemoteProtocolTest, ParsesAndValidatesVibrationGoldenFixtures) {
  using namespace xbox_remote::protocol;
  const auto parsed = parse_vibration_packet(read_hex_fixture("vibration_packet.bin.hex"));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed.value.gamepad_index, 0);
  EXPECT_EQ(parsed.value.left_motor_percent, 100);
  EXPECT_EQ(parsed.value.right_motor_percent, 50);
  EXPECT_EQ(parsed.value.left_trigger_percent, 25);
  EXPECT_EQ(parsed.value.right_trigger_percent, 0);
  EXPECT_EQ(parsed.value.duration_ms, 1000);
  EXPECT_EQ(parsed.value.delay_ms, 250);
  EXPECT_EQ(parsed.value.repeat, 3);

  auto with_metadata = read_hex_fixture("vibration_packet.bin.hex");
  with_metadata[0] = 0x90;
  with_metadata.insert(with_metadata.begin() + 2, {0x38, 0x04, 0, 0, 0x80, 0x07, 0, 0});
  EXPECT_TRUE(parse_vibration_packet(with_metadata));

  EXPECT_EQ(parse_vibration_packet({}).error.code, error_code_e::invalid_length);
  EXPECT_EQ(parse_vibration_packet({0x02, 0x00}).error.code, error_code_e::invalid_value);
  EXPECT_EQ(parse_vibration_packet({0x81, 0x00}).error.code, error_code_e::invalid_value);
  EXPECT_EQ(parse_vibration_packet({0x80, 0x00}).error.code, error_code_e::invalid_length);

  auto invalid = read_hex_fixture("vibration_packet.bin.hex");
  invalid[2] = 1;
  EXPECT_EQ(parse_vibration_packet(invalid).error.field, "vibrationPacket.rumbleType");
  invalid = read_hex_fixture("vibration_packet.bin.hex");
  invalid[3] = 1;
  EXPECT_EQ(parse_vibration_packet(invalid).error.field, "vibrationPacket.gamepadIndex");
  invalid = read_hex_fixture("vibration_packet.bin.hex");
  invalid[4] = 101;
  EXPECT_EQ(parse_vibration_packet(invalid).error.field, "vibrationPacket.motorPercent");
  invalid = read_hex_fixture("vibration_packet.bin.hex");
  invalid.push_back(0);
  EXPECT_EQ(parse_vibration_packet(invalid).error.code, error_code_e::invalid_length);
}

TEST(XboxRemoteProtocolTest, GoldenFixturesContainNoCredentialOrPrivateAddressShapes) {
  const auto directory = fixture_path("");
  const std::regex jwt_shape(R"([A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})");
  const std::regex private_address(R"((10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.))");
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    const auto contents = read_fixture(entry.path().filename().string());
    SCOPED_TRACE(entry.path().filename().string());
    EXPECT_FALSE(std::regex_search(contents, jwt_shape));
    EXPECT_FALSE(std::regex_search(contents, private_address));
    EXPECT_EQ(contents.find("refresh_token"), std::string::npos);
    EXPECT_EQ(contents.find("access_token"), std::string::npos);
    EXPECT_EQ(contents.find("gsToken"), std::string::npos);
  }
}
