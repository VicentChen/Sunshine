/**
 * @file src/xbox_remote/protocol.cpp
 * @brief Serialization and validation for Xbox Home Remote Play protocol contracts.
 */

#include "src/xbox_remote/protocol.h"

// standard includes
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

// library includes
#include <nlohmann/json.hpp>

namespace xbox_remote::protocol {
  namespace {
    using json = nlohmann::json;

    /**
     * @brief Construct a structured parsing error.
     *
     * @param code Error category.
     * @param field Failing JSON path or binary section.
     * @param message Non-sensitive diagnostic message.
     * @return Populated protocol error.
     */
    error_t make_error(error_code_e code, std::string field, std::string message) {
      return {code, std::move(field), std::move(message)};
    }

    /**
     * @brief Parse one JSON document without retaining its source text in errors.
     *
     * @param source JSON source text.
     * @return Parsed JSON or a sanitized error.
     */
    result_t<json> parse_json(std::string_view source) {
      result_t<json> result;
      try {
        result.value = json::parse(source);
      } catch (const json::exception &) {
        result.error = make_error(error_code_e::invalid_json, "$", "invalid JSON document");
      }
      return result;
    }

    /**
     * @brief Read a required string property.
     *
     * @param object Parent JSON object.
     * @param key Property name.
     * @param path Parent JSON path.
     * @param error Output error.
     * @return Property value when present and correctly typed.
     */
    std::optional<std::string> required_string(const json &object, std::string_view key, std::string_view path, error_t &error) {
      const auto iterator = object.find(key);
      const std::string field = std::string(path) + "." + std::string(key);
      if (iterator == object.end()) {
        error = make_error(error_code_e::missing_field, field, "required field is missing");
        return std::nullopt;
      }
      if (!iterator->is_string()) {
        error = make_error(error_code_e::wrong_type, field, "required field must be a string");
        return std::nullopt;
      }
      return iterator->get<std::string>();
    }

    /**
     * @brief Read an optional string property.
     *
     * @param object Parent JSON object.
     * @param key Property name.
     * @param path Parent JSON path.
     * @param error Output error.
     * @return Property value, an empty string when absent, or no value on a type error.
     */
    std::optional<std::string> optional_string(const json &object, std::string_view key, std::string_view path, error_t &error) {
      const auto iterator = object.find(key);
      if (iterator == object.end()) {
        return std::string {};
      }
      if (!iterator->is_string()) {
        error = make_error(error_code_e::wrong_type, std::string(path) + "." + std::string(key), "optional field must be a string");
        return std::nullopt;
      }
      return iterator->get<std::string>();
    }

    /**
     * @brief Extract a bounded service failure code without retaining free-form text.
     *
     * @param document Session-state response object.
     * @return Safe diagnostic code, or an empty string when absent or unsafe.
     */
    std::string service_failure_code(const json &document) {
      constexpr std::size_t maximum_code_size = 96;
      const json *source = &document;
      const auto details = document.find("errorDetails");
      if (details != document.end() && details->is_object()) {
        source = &*details;
      }
      const auto code = source->find("code");
      if (code == source->end() || !code->is_string()) {
        return {};
      }
      const auto value = code->get<std::string>();
      if (value.empty() || value.size() > maximum_code_size) {
        return {};
      }
      const auto safe_character = [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.' || character == ':';
      };
      return std::ranges::all_of(value, safe_character) ? value : std::string {};
    }

    /**
     * @brief Append an integer in explicit little-endian order.
     *
     * @tparam T Integral type.
     * @param bytes Destination byte vector.
     * @param value Value to append.
     */
    template<typename T>
    void append_le(std::vector<std::uint8_t> &bytes, T value) {
      using unsigned_t = std::make_unsigned_t<T>;
      const auto unsigned_value = static_cast<unsigned_t>(value);
      for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(unsigned_value >> (index * 8U)));
      }
    }

    /**
     * @brief Read an integer in explicit little-endian order.
     *
     * @tparam T Integral type.
     * @param bytes Source byte vector.
     * @param offset Byte offset advanced past the decoded field.
     * @return Decoded value. The caller must validate the source length first.
     */
    template<typename T>
    T read_le(const std::vector<std::uint8_t> &bytes, std::size_t &offset) {
      using unsigned_t = std::make_unsigned_t<T>;
      unsigned_t value = 0;
      for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<unsigned_t>(bytes[offset++]) << (index * 8U);
      }
      return static_cast<T>(value);
    }

    /**
     * @brief Append a floating-point timestamp in IEEE-754 little-endian order.
     *
     * @param bytes Destination byte vector.
     * @param value Timestamp value.
     */
    void append_double_le(std::vector<std::uint8_t> &bytes, double value) {
      append_le(bytes, std::bit_cast<std::uint64_t>(value));
    }

    /**
     * @brief Read an IEEE-754 little-endian timestamp.
     *
     * @param bytes Source byte vector.
     * @param offset Byte offset advanced past the timestamp.
     * @return Decoded timestamp.
     */
    double read_double_le(const std::vector<std::uint8_t> &bytes, std::size_t &offset) {
      return std::bit_cast<double>(read_le<std::uint64_t>(bytes, offset));
    }

    /**
     * @brief Wrap message content in an Xbox message-channel envelope.
     *
     * @param target Xbox message target path.
     * @param content JSON content stored as a string in the envelope.
     * @param id Deterministic non-secret message identifier.
     * @return Compact JSON envelope.
     */
    std::string make_message_envelope(std::string_view target, const json &content, std::string_view id) {
      return json {
        {"type", "Message"},
        {"content", content.dump()},
        {"id", std::string(id)},
        {"target", std::string(target)},
        {"cv", ""},
      }
        .dump();
    }

    /**
     * @brief Encode the common input header.
     *
     * @param bytes Destination packet.
     * @param header Header fields.
     */
    void append_input_header(std::vector<std::uint8_t> &bytes, const input_header_t &header) {
      append_le(bytes, static_cast<std::uint16_t>(header.report_type));
      append_le(bytes, header.sequence);
      append_double_le(bytes, header.timestamp_ms);
    }
  }  // namespace

  std::string serialize_home_play_request(const home_play_request_t &request) {
    return json {
      {"clientSessionId", request.client_session_id},
      {"titleId", ""},
      {"serverId", request.server_id},
      {"systemUpdateGroup", request.system_update_group},
      {"settings",
       {
         {"nanoVersion", request.settings.nano_version},
         {"enableTextToSpeech", request.settings.enable_text_to_speech},
         {"highContrast", request.settings.high_contrast},
         {"locale", request.settings.locale},
         {"useIceConnection", request.settings.use_ice_connection},
         {"timezoneOffsetMinutes", request.settings.timezone_offset_minutes},
         {"sdkType", request.settings.sdk_type},
         {"osName", request.settings.os_name},
       }},
      {"fallbackRegionNames", request.fallback_region_names},
    }
      .dump();
  }

  result_t<std::vector<console_t>> parse_console_list(std::string_view source) {
    result_t<std::vector<console_t>> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "console response must be an object");
      return result;
    }
    const auto results = document.value.find("results");
    if (results == document.value.end()) {
      result.error = make_error(error_code_e::missing_field, "$.results", "required field is missing");
      return result;
    }
    if (!results->is_array()) {
      result.error = make_error(error_code_e::wrong_type, "$.results", "console results must be an array");
      return result;
    }

    result.value.reserve(results->size());
    for (std::size_t index = 0; index < results->size(); ++index) {
      const auto &item = (*results)[index];
      const std::string path = "$.results[" + std::to_string(index) + "]";
      if (!item.is_object()) {
        result.error = make_error(error_code_e::wrong_type, path, "console entry must be an object");
        result.value.clear();
        return result;
      }

      console_t console;
      const auto server_id = required_string(item, "serverId", path, result.error);
      const auto device_name = optional_string(item, "deviceName", path, result.error);
      const auto console_type = optional_string(item, "consoleType", path, result.error);
      const auto power_state = optional_string(item, "powerState", path, result.error);
      if (!server_id || !device_name || !console_type || !power_state) {
        result.value.clear();
        return result;
      }
      if (server_id->empty()) {
        result.error = make_error(error_code_e::invalid_value, path + ".serverId", "server identifier must not be empty");
        result.value.clear();
        return result;
      }
      console.server_id = *server_id;
      console.device_name = *device_name;
      console.console_type = *console_type;
      console.power_state = *power_state;
      result.value.push_back(std::move(console));
    }
    return result;
  }

  result_t<session_created_t> parse_session_created(std::string_view source) {
    result_t<session_created_t> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "session creation response must be an object");
      return result;
    }

    const auto session_id = document.value.find("sessionId");
    if (session_id != document.value.end()) {
      if (!session_id->is_string()) {
        result.error = make_error(error_code_e::wrong_type, "$.sessionId", "session identifier must be a string");
        return result;
      }
      result.value.session_id = session_id->get<std::string>();
    } else {
      const auto session_path = required_string(document.value, "sessionPath", "$", result.error);
      if (!session_path) {
        return result;
      }
      const auto slash = session_path->find_last_of('/');
      if (slash != std::string::npos) {
        result.value.session_id = session_path->substr(slash + 1);
      }
    }
    if (result.value.session_id.empty()) {
      result.error = make_error(error_code_e::invalid_value, "$.sessionId", "session identifier must not be empty");
    }
    return result;
  }

  result_t<session_state_t> parse_session_state(std::string_view source) {
    result_t<session_state_t> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "session state response must be an object");
      return result;
    }
    const auto state = required_string(document.value, "state", "$", result.error);
    if (!state) {
      return result;
    }
    if (state->empty()) {
      result.error = make_error(error_code_e::invalid_value, "$.state", "session state must not be empty");
      return result;
    }
    result.value.state = *state;
    result.value.failure_code = service_failure_code(document.value);
    return result;
  }

  bool is_home_session_provisioned(const session_state_t &state) {
    return state.state == "Provisioned";
  }

  result_t<session_configuration_t> parse_session_configuration(std::string_view source) {
    result_t<session_configuration_t> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "session configuration must be an object");
      return result;
    }
    const auto server_details = document.value.find("serverDetails");
    if (server_details != document.value.end()) {
      if (!server_details->is_object()) {
        result.error = make_error(error_code_e::wrong_type, "$.serverDetails", "server details must be an object");
        return result;
      }

      const std::string ip_key = server_details->contains("ipV4Address") ? "ipV4Address" : "ipAddress";
      if (const auto ip_address = server_details->find(ip_key); ip_address != server_details->end() && ip_address->is_string()) {
        result.value.ipv4_address = ip_address->get<std::string>();
      }

      const std::string port_key = server_details->contains("ipV4Port") ? "ipV4Port" : "port";
      if (const auto port = server_details->find(port_key); port != server_details->end() && port->is_number_integer()) {
        const auto port_value = port->get<std::int64_t>();
        if (port_value > 0 && port_value <= std::numeric_limits<std::uint16_t>::max()) {
          result.value.ipv4_port = static_cast<std::uint16_t>(port_value);
        }
      }

      if (const auto ice_path = server_details->find("iceExchangePath"); ice_path != server_details->end() && ice_path->is_string()) {
        result.value.ice_exchange_path = ice_path->get<std::string>();
      }

      if (const auto srtp = server_details->find("srtp"); srtp != server_details->end() && srtp->is_object()) {
        if (const auto key = srtp->find("key"); key != srtp->end() && key->is_string()) {
          result.value.srtp_key = key->get<std::string>();
        }
      }
    }

    const auto keepalive = document.value.find("keepAlivePulseInSeconds");
    if (keepalive == document.value.end()) {
      return result;
    }
    if (!keepalive->is_number_integer()) {
      result.error = make_error(error_code_e::wrong_type, "$.keepAlivePulseInSeconds", "keepalive interval must be an integer");
      return result;
    }
    const auto keepalive_value = keepalive->get<std::int64_t>();
    if (keepalive_value <= 0 || keepalive_value > std::numeric_limits<std::uint32_t>::max()) {
      result.error = make_error(error_code_e::invalid_value, "$.keepAlivePulseInSeconds", "keepalive interval is outside the valid range");
      return result;
    }
    result.value.keepalive_seconds = static_cast<std::uint32_t>(keepalive_value);
    return result;
  }

  result_t<exchange_response_t> parse_exchange_response(std::string_view source) {
    result_t<exchange_response_t> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "exchange response must be an object");
      return result;
    }
    const auto exchange = required_string(document.value, "exchangeResponse", "$", result.error);
    if (!exchange) {
      return result;
    }
    if (exchange->empty()) {
      result.error = make_error(error_code_e::invalid_value, "$.exchangeResponse", "exchange response must not be empty");
      return result;
    }
    result.value.exchange_response = *exchange;
    return result;
  }

  result_t<std::string> parse_sdp_exchange(std::string_view source) {
    result_t<std::string> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "SDP exchange must be an object");
      return result;
    }
    const auto sdp = required_string(document.value, "sdp", "$", result.error);
    if (!sdp) {
      return result;
    }
    if (sdp->empty()) {
      result.error = make_error(error_code_e::invalid_value, "$.sdp", "SDP answer must not be empty");
      return result;
    }
    const auto type = optional_string(document.value, "sdpType", "$", result.error);
    if (!type) {
      return result;
    }
    if (!type->empty() && *type != "answer") {
      result.error = make_error(error_code_e::invalid_value, "$.sdpType", "SDP exchange type must be answer");
      return result;
    }
    result.value = *sdp;
    return result;
  }

  result_t<std::vector<ice_candidate_t>> parse_ice_exchange(std::string_view source) {
    result_t<std::vector<ice_candidate_t>> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_array()) {
      result.error = make_error(error_code_e::wrong_type, "$", "ICE exchange must be an array");
      return result;
    }
    for (std::size_t index = 0; index < document.value.size(); ++index) {
      json candidate_document = document.value[index];
      if (candidate_document.is_string()) {
        auto nested = parse_json(candidate_document.get<std::string>());
        if (!nested) {
          result.error = make_error(error_code_e::invalid_json, "$[" + std::to_string(index) + "]", "ICE candidate string is not valid JSON");
          return result;
        }
        candidate_document = std::move(nested.value);
      }
      const auto path = "$[" + std::to_string(index) + "]";
      if (!candidate_document.is_object()) {
        result.error = make_error(error_code_e::wrong_type, path, "ICE candidate must be an object");
        return result;
      }
      ice_candidate_t candidate;
      const auto candidate_value = required_string(candidate_document, "candidate", path, result.error);
      if (!candidate_value || candidate_value->empty()) {
        if (result.error.code == error_code_e::none) {
          result.error = make_error(error_code_e::invalid_value, path + ".candidate", "ICE candidate must not be empty");
        }
        return result;
      }
      candidate.candidate = *candidate_value;
      const auto mid = optional_string(candidate_document, "sdpMid", path, result.error);
      if (!mid) {
        return result;
      }
      candidate.sdp_mid = mid->empty() ? "0" : *mid;
      const auto mline = candidate_document.find("sdpMLineIndex");
      if (mline != candidate_document.end()) {
        std::uint64_t parsed_index = 0;
        if (mline->is_number_unsigned()) {
          parsed_index = mline->get<std::uint64_t>();
        } else if (mline->is_string()) {
          try {
            const auto text = mline->get<std::string>();
            std::size_t consumed = 0;
            parsed_index = std::stoull(text, &consumed);
            if (consumed != text.size()) {
              throw std::invalid_argument("trailing data");
            }
          } catch (const std::exception &) {
            result.error = make_error(error_code_e::wrong_type, path + ".sdpMLineIndex", "SDP m-line index must be an unsigned integer");
            return result;
          }
        } else {
          result.error = make_error(error_code_e::wrong_type, path + ".sdpMLineIndex", "SDP m-line index must be an unsigned integer");
          return result;
        }
        if (parsed_index > std::numeric_limits<std::uint16_t>::max()) {
          result.error = make_error(error_code_e::invalid_value, path + ".sdpMLineIndex", "SDP m-line index is out of range");
          return result;
        }
        candidate.sdp_mline_index = static_cast<std::uint16_t>(parsed_index);
      }
      const auto username_fragment = optional_string(candidate_document, "usernameFragment", path, result.error);
      if (!username_fragment) {
        return result;
      }
      candidate.username_fragment = *username_fragment;
      result.value.push_back(std::move(candidate));
    }
    return result;
  }

  std::string serialize_sdp_offer(const sdp_offer_t &offer) {
    return json {
      {"messageType", "offer"},
      {"sdp", offer.sdp},
      {"configuration",
       {
         {"chatConfiguration",
          {
            {"bytesPerSample", 2},
            {"expectedClipDurationMs", 20},
            {"format", {{"codec", "opus"}, {"container", "webm"}}},
            {"numChannels", 1},
            {"sampleFrequencyHz", 24000},
          }},
         {"chat", {{"minVersion", 1}, {"maxVersion", 1}}},
         {"control", {{"minVersion", 1}, {"maxVersion", 3}}},
         {"input", {{"minVersion", 1}, {"maxVersion", 8}}},
         {"message", {{"minVersion", 1}, {"maxVersion", 1}}},
       }},
    }
      .dump();
  }

  std::string serialize_ice_candidates(const std::vector<ice_candidate_t> &candidates) {
    json encoded_candidates = json::array();
    for (const auto &candidate : candidates) {
      json encoded {
        {"candidate", candidate.candidate},
        {"sdpMid", candidate.sdp_mid},
        {"sdpMLineIndex", candidate.sdp_mline_index},
      };
      if (!candidate.username_fragment.empty()) {
        encoded["usernameFragment"] = candidate.username_fragment;
      }
      encoded_candidates.push_back(encoded.dump());
    }
    return json {
      {"messageType", "iceCandidate"},
      {"candidate", std::move(encoded_candidates)},
    }
      .dump();
  }

  std::string make_message_handshake(std::string_view id) {
    return json {
      {"type", "Handshake"},
      {"version", "messageV1"},
      {"id", std::string(id)},
      {"cv", "0"},
    }
      .dump();
  }

  result_t<std::string> parse_message_handshake_ack(std::string_view source) {
    result_t<std::string> result;
    auto document = parse_json(source);
    if (!document) {
      result.error = std::move(document.error);
      return result;
    }
    if (!document.value.is_object()) {
      result.error = make_error(error_code_e::wrong_type, "$", "handshake acknowledgement must be an object");
      return result;
    }
    const auto type = required_string(document.value, "type", "$", result.error);
    if (!type) {
      return result;
    }
    if (*type != "HandshakeAck") {
      result.error = make_error(error_code_e::invalid_value, "$.type", "message is not a handshake acknowledgement");
      return result;
    }
    const auto version = required_string(document.value, "version", "$", result.error);
    if (!version) {
      return result;
    }
    if (*version != "messageV1") {
      result.error = make_error(error_code_e::invalid_value, "$.version", "unsupported handshake version");
      return result;
    }
    result.value = *version;
    return result;
  }

  std::string make_authorization_request() {
    return json {
      {"message", "authorizationRequest"},
      {"accessKey", "4BDB3609-C1F1-4195-9B37-FEFF45DA8B8E"},
    }
      .dump();
  }

  std::string make_gamepad_changed(std::uint32_t gamepad_index, bool was_added) {
    return json {
      {"message", "gamepadChanged"},
      {"gamepadIndex", gamepad_index},
      {"wasAdded", was_added},
    }
      .dump();
  }

  std::vector<std::string> make_startup_messages(const startup_message_parameters_t &parameters) {
    std::vector<std::string> messages;
    messages.reserve(6);
    messages.push_back(make_message_envelope(
      "/streaming/systemUi/configuration",
      {{"version", {0, 2, 0}}, {"systemUis", json::array()}},
      "startup-1"
    ));
    messages.push_back(make_message_envelope(
      "/streaming/properties/clientappinstallidchanged",
      {{"clientAppInstallId", parameters.install_id}},
      "startup-2"
    ));
    messages.push_back(make_message_envelope(
      "/streaming/characteristics/orientationchanged",
      {{"orientation", 0}},
      "startup-3"
    ));
    messages.push_back(make_message_envelope(
      "/streaming/characteristics/touchinputenabledchanged",
      {{"touchInputEnabled", false}},
      "startup-4"
    ));
    messages.push_back(make_message_envelope(
      "/streaming/characteristics/clientdevicecapabilities",
      {
        {"supportsCustomResolution", true},
        {"supportsHevc", false},
        {"supportsHdr", false},
        {"supportsFps", parameters.frames_per_second},
        {"maxWidth", parameters.width},
        {"maxHeight", parameters.height},
        {"maxBitrateKbps", parameters.bitrate_kbps},
        {"video",
         {
           {"width", parameters.width},
           {"height", parameters.height},
           {"maxWidth", parameters.width},
           {"maxHeight", parameters.height},
           {"maxBitrateKbps", parameters.bitrate_kbps},
         }},
      },
      "startup-5"
    ));
    messages.push_back(make_message_envelope(
      "/streaming/characteristics/dimensionschanged",
      {
        {"horizontal", parameters.width},
        {"vertical", parameters.height},
        {"preferredWidth", parameters.width},
        {"preferredHeight", parameters.height},
        {"safeAreaLeft", 0},
        {"safeAreaTop", 0},
        {"safeAreaRight", parameters.width},
        {"safeAreaBottom", parameters.height},
        {"supportsCustomResolution", true},
      },
      "startup-6"
    ));
    return messages;
  }

  std::vector<std::uint8_t> encode_client_metadata(input_header_t header, std::uint8_t max_touch_points) {
    header.report_type = report_type_e::client_metadata;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(client_metadata_packet_size);
    append_input_header(bytes, header);
    bytes.push_back(max_touch_points);
    return bytes;
  }

  std::vector<std::uint8_t> encode_gamepad_packet(input_header_t header, const gamepad_frame_t &frame) {
    header.report_type = report_type_e::gamepad;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(single_gamepad_packet_size);
    append_input_header(bytes, header);
    bytes.push_back(1);
    bytes.push_back(frame.gamepad_index);
    append_le(bytes, frame.button_mask);
    append_le(bytes, frame.left_stick_x);
    append_le(bytes, frame.left_stick_y);
    append_le(bytes, frame.right_stick_x);
    append_le(bytes, frame.right_stick_y);
    append_le(bytes, frame.left_trigger);
    append_le(bytes, frame.right_trigger);
    append_le(bytes, frame.physical_physicality);
    append_le(bytes, frame.virtual_physicality);
    return bytes;
  }

  result_t<std::pair<input_header_t, gamepad_frame_t>> decode_gamepad_packet(const std::vector<std::uint8_t> &bytes) {
    result_t<std::pair<input_header_t, gamepad_frame_t>> result;
    if (bytes.size() != single_gamepad_packet_size) {
      result.error = make_error(error_code_e::invalid_length, "gamepadPacket", "single-gamepad packet must contain exactly 38 bytes");
      return result;
    }

    std::size_t offset = 0;
    const auto raw_report_type = read_le<std::uint16_t>(bytes, offset);
    if (raw_report_type != static_cast<std::uint16_t>(report_type_e::gamepad)) {
      result.error = make_error(error_code_e::invalid_value, "gamepadPacket.reportType", "packet is not a gamepad report");
      return result;
    }
    auto &[header, frame] = result.value;
    header.report_type = report_type_e::gamepad;
    header.sequence = read_le<std::uint32_t>(bytes, offset);
    header.timestamp_ms = read_double_le(bytes, offset);
    if (bytes[offset++] != 1) {
      result.error = make_error(error_code_e::invalid_value, "gamepadPacket.frameCount", "single-gamepad packet must contain one frame");
      return result;
    }
    frame.gamepad_index = bytes[offset++];
    frame.button_mask = read_le<std::uint16_t>(bytes, offset);
    frame.left_stick_x = read_le<std::int16_t>(bytes, offset);
    frame.left_stick_y = read_le<std::int16_t>(bytes, offset);
    frame.right_stick_x = read_le<std::int16_t>(bytes, offset);
    frame.right_stick_y = read_le<std::int16_t>(bytes, offset);
    frame.left_trigger = read_le<std::uint16_t>(bytes, offset);
    frame.right_trigger = read_le<std::uint16_t>(bytes, offset);
    frame.physical_physicality = read_le<std::uint32_t>(bytes, offset);
    frame.virtual_physicality = read_le<std::uint32_t>(bytes, offset);
    return result;
  }

  result_t<vibration_t> parse_vibration_packet(const std::vector<std::uint8_t> &bytes) {
    result_t<vibration_t> result;
    if (bytes.size() < 2) {
      result.error = make_error(error_code_e::invalid_length, "vibrationPacket", "packet is shorter than its report type");
      return result;
    }

    std::size_t offset = 0;
    const auto report_type = read_le<std::uint16_t>(bytes, offset);
    constexpr auto server_metadata = static_cast<std::uint16_t>(report_type_e::server_metadata);
    constexpr auto vibration = static_cast<std::uint16_t>(report_type_e::vibration);
    if ((report_type & vibration) == 0 || (report_type & ~(server_metadata | vibration)) != 0) {
      result.error = make_error(error_code_e::invalid_value, "vibrationPacket.reportType", "packet contains unsupported report-type flags");
      return result;
    }
    if ((report_type & server_metadata) != 0) {
      offset += 8;
    }
    constexpr std::size_t vibration_payload_size = 11;
    if (bytes.size() != offset + vibration_payload_size) {
      result.error = make_error(error_code_e::invalid_length, "vibrationPacket", "packet length does not match its report-type flags");
      return result;
    }
    if (bytes[offset++] != 0) {
      result.error = make_error(error_code_e::invalid_value, "vibrationPacket.rumbleType", "unsupported vibration command type");
      return result;
    }
    result.value.gamepad_index = bytes[offset++];
    if (result.value.gamepad_index != 0) {
      result.error = make_error(error_code_e::invalid_value, "vibrationPacket.gamepadIndex", "first-version sessions only accept gamepad index zero");
      return result;
    }
    result.value.left_motor_percent = bytes[offset++];
    result.value.right_motor_percent = bytes[offset++];
    result.value.left_trigger_percent = bytes[offset++];
    result.value.right_trigger_percent = bytes[offset++];
    if (result.value.left_motor_percent > 100 || result.value.right_motor_percent > 100 || result.value.left_trigger_percent > 100 || result.value.right_trigger_percent > 100) {
      result.error = make_error(error_code_e::invalid_value, "vibrationPacket.motorPercent", "vibration percentages must not exceed 100");
      return result;
    }
    result.value.duration_ms = read_le<std::uint16_t>(bytes, offset);
    result.value.delay_ms = read_le<std::uint16_t>(bytes, offset);
    result.value.repeat = bytes[offset];
    return result;
  }
}  // namespace xbox_remote::protocol
