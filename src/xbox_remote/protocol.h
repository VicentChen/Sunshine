/**
 * @file src/xbox_remote/protocol.h
 * @brief Xbox Home Remote Play protocol contracts used by the compatibility probe.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xbox_remote::protocol {
  /**
   * @brief Number of bytes in an Xbox input packet header.
   */
  constexpr std::size_t input_header_size = 14;

  /**
   * @brief Number of bytes in one Xbox gamepad frame.
   */
  constexpr std::size_t gamepad_frame_size = 23;

  /**
   * @brief Number of bytes in a one-gamepad input packet.
   */
  constexpr std::size_t single_gamepad_packet_size = input_header_size + 1 + gamepad_frame_size;

  /**
   * @brief Number of bytes in a client-metadata packet.
   */
  constexpr std::size_t client_metadata_packet_size = input_header_size + 1;

  /**
   * @brief Errors produced while validating a protocol payload.
   */
  enum class error_code_e {
    none,  ///< The payload is valid.
    invalid_json,  ///< The payload is not valid JSON.
    missing_field,  ///< A required JSON field is absent.
    wrong_type,  ///< A JSON field has the wrong type.
    invalid_value,  ///< A field contains a value outside the protocol contract.
    invalid_length,  ///< A binary payload has the wrong length.
  };

  /**
   * @brief Structured protocol parsing error.
   */
  struct error_t {
    error_code_e code = error_code_e::none;  ///< Machine-readable error category.
    std::string field;  ///< JSON path or binary section that failed validation.
    std::string message;  ///< Human-readable error text without sensitive payload data.
  };

  /**
   * @brief Result returned by protocol parsers.
   *
   * @tparam T Parsed value type.
   */
  template<typename T>
  struct result_t {
    T value {};  ///< Parsed value when @c error.code is @c error_code_e::none.
    error_t error {};  ///< Structured parsing failure.

    /**
     * @brief Check whether parsing succeeded.
     *
     * @return @c true when the result contains a valid value.
     */
    explicit operator bool() const {
      return error.code == error_code_e::none;
    }
  };

  /**
   * @brief One Xbox console returned by Home console discovery.
   */
  struct console_t {
    std::string server_id;  ///< Stable server identifier used to start a Home session.
    std::string device_name;  ///< User-visible console name.
    std::string console_type;  ///< Xbox hardware family reported by the service.
    std::string power_state;  ///< Current service-reported power state.
  };

  /**
   * @brief Settings included in a Home session play request.
   */
  struct home_play_settings_t {
    std::string nano_version = "V3;WebrtcTransport.dll";  ///< Xbox streaming transport version marker.
    bool enable_text_to_speech = false;  ///< Whether text-to-speech is requested.
    std::uint32_t high_contrast = 0;  ///< High-contrast accessibility mode.
    std::string locale = "en-US";  ///< Session locale sent to Xbox services.
    bool use_ice_connection = false;  ///< GSSV SDP/ICE exchange selector.
    std::int32_t timezone_offset_minutes = 120;  ///< Client timezone offset in minutes.
    std::string sdk_type = "web";  ///< Xbox SDK client category.
    std::string os_name = "windows";  ///< Client operating-system name.
  };

  /**
   * @brief Minimal request body for @c POST /v5/sessions/home/play.
   */
  struct home_play_request_t {
    std::string client_session_id;  ///< Optional client-generated session identifier.
    std::string server_id;  ///< Selected Home console identifier.
    std::string system_update_group;  ///< Optional Xbox system update group.
    home_play_settings_t settings {};  ///< Remote Play client settings.
    std::vector<std::string> fallback_region_names;  ///< Ordered fallback GSSV regions.
  };

  /**
   * @brief Successful Home session creation response.
   */
  struct session_created_t {
    std::string session_id;  ///< Server-assigned Home session identifier.
  };

  /**
   * @brief Home session state returned by the GSSV state endpoint.
   */
  struct session_state_t {
    std::string state;  ///< Raw state name returned by Xbox services.
    std::string failure_code;  ///< Optional allowlisted service code without free-form response text.
  };

  /**
   * @brief Home session configuration with best-effort legacy transport details.
   */
  struct session_configuration_t {
    std::string ipv4_address;  ///< Optional legacy server IPv4 address or reserved test address.
    std::uint16_t ipv4_port = 0;  ///< Optional legacy server transport port.
    std::string ice_exchange_path;  ///< Optional legacy service-provided ICE exchange path.
    std::string srtp_key;  ///< Optional legacy SRTP key returned by the service.
    std::uint32_t keepalive_seconds = 20;  ///< Service interval or current-client 20-second compatibility default.
  };

  /**
   * @brief Outer response used by the SDP and ICE exchange endpoints.
   */
  struct exchange_response_t {
    std::string exchange_response;  ///< JSON-encoded inner SDP or ICE response.
  };

  /**
   * @brief Minimal SDP offer sent to the Home session SDP endpoint.
   */
  struct sdp_offer_t {
    std::string sdp;  ///< Complete WebRTC SDP offer.
  };

  /**
   * @brief One ICE candidate exchanged through the Home session REST API.
   */
  struct ice_candidate_t {
    std::string candidate;  ///< SDP candidate attribute without sensitive logging guarantees.
    std::string sdp_mid;  ///< SDP media-section identifier.
    std::uint16_t sdp_mline_index = 0;  ///< Zero-based SDP media-section index.
    std::string username_fragment;  ///< Optional ICE username fragment.
  };

  /**
   * @brief Xbox input gamepad-button bit mask.
   */
  enum class gamepad_button_e : std::uint16_t {
    nexus = 0x0002,  ///< Xbox Nexus or Guide button.
    menu = 0x0004,  ///< Menu button.
    view = 0x0008,  ///< View button.
    a = 0x0010,  ///< A face button.
    b = 0x0020,  ///< B face button.
    x = 0x0040,  ///< X face button.
    y = 0x0080,  ///< Y face button.
    dpad_up = 0x0100,  ///< Direction-pad up.
    dpad_down = 0x0200,  ///< Direction-pad down.
    dpad_left = 0x0400,  ///< Direction-pad left.
    dpad_right = 0x0800,  ///< Direction-pad right.
    left_shoulder = 0x1000,  ///< Left shoulder button.
    right_shoulder = 0x2000,  ///< Right shoulder button.
    left_thumb = 0x4000,  ///< Left thumbstick button.
    right_thumb = 0x8000,  ///< Right thumbstick button.
  };

  /**
   * @brief Xbox input physical-control activity bit mask.
   */
  enum class gamepad_physicality_e : std::uint32_t {
    none = 0x00000000,  ///< No physical control is active.
    dpad_up = 0x00000001,  ///< Direction-pad up is active.
    dpad_down = 0x00000002,  ///< Direction-pad down is active.
    dpad_left = 0x00000004,  ///< Direction-pad left is active.
    dpad_right = 0x00000008,  ///< Direction-pad right is active.
    menu = 0x00000010,  ///< Menu is active.
    view = 0x00000020,  ///< View is active.
    left_thumb = 0x00000040,  ///< Left thumbstick button is active.
    right_thumb = 0x00000080,  ///< Right thumbstick button is active.
    left_shoulder = 0x00000100,  ///< Left shoulder is active.
    right_shoulder = 0x00000200,  ///< Right shoulder is active.
    nexus = 0x00000400,  ///< Nexus or Guide is active.
    misc = 0x00000800,  ///< Miscellaneous controller input is active.
    a = 0x00001000,  ///< A is active.
    b = 0x00002000,  ///< B is active.
    x = 0x00004000,  ///< X is active.
    y = 0x00008000,  ///< Y is active.
    left_trigger = 0x00010000,  ///< Left trigger is active.
    right_trigger = 0x00020000,  ///< Right trigger is active.
    left_thumb_x = 0x00040000,  ///< Left thumbstick X axis is active.
    left_thumb_y = 0x00080000,  ///< Left thumbstick Y axis is active.
    right_thumb_x = 0x00100000,  ///< Right thumbstick X axis is active.
    right_thumb_y = 0x00200000,  ///< Right thumbstick Y axis is active.
  };

  /**
   * @brief Xbox-compatible WebRTC data-channel contract.
   */
  struct data_channel_t {
    std::string_view label;  ///< WebRTC data-channel label.
    std::string_view protocol;  ///< WebRTC data-channel subprotocol.
    std::uint16_t sid = 0;  ///< Negotiated SCTP stream identifier.
    bool reliable = true;  ///< Whether retransmission is required.
    bool ordered = true;  ///< Whether ordered delivery is required.
  };

  /**
   * @brief Four data channels required by Xbox Remote Play.
   */
  inline constexpr std::array<data_channel_t, 4> data_channels {{
    {"control", "controlV1", 0, true, true},
    {"input", "1.0", 2, true, true},
    {"message", "messageV1", 4, true, true},
    {"chat", "chatV1", 6, true, true},
  }};

  /**
   * @brief Parameters used to create the six startup capability messages.
   */
  struct startup_message_parameters_t {
    std::uint32_t width = 1920;  ///< Requested video width used in capability messages.
    std::uint32_t height = 1080;  ///< Requested video height used in capability messages.
    std::uint32_t frames_per_second = 60;  ///< Requested maximum frame rate.
    std::uint32_t bitrate_kbps = 20000;  ///< Requested maximum video bitrate.
    std::string install_id = "00000000-0000-0000-0000-000000000000";  ///< Non-secret client installation identifier.
  };

  /**
   * @brief Xbox input report-type flags.
   */
  enum class report_type_e : std::uint16_t {
    gamepad = 0x0002,  ///< Complete gamepad snapshots.
    client_metadata = 0x0008,  ///< Client input capabilities.
    server_metadata = 0x0010,  ///< Server dimensions preceding feedback data.
    vibration = 0x0080,  ///< Four-motor vibration command.
  };

  /**
   * @brief Common header of a client-to-Xbox input packet.
   */
  struct input_header_t {
    report_type_e report_type = report_type_e::gamepad;  ///< Packet payload selector.
    std::uint32_t sequence = 0;  ///< Sequence assigned immediately before transport send.
    double timestamp_ms = 0.0;  ///< Monotonic client timestamp in milliseconds.
  };

  /**
   * @brief One complete Xbox gamepad wire snapshot.
   */
  struct gamepad_frame_t {
    std::uint8_t gamepad_index = 0;  ///< Xbox session gamepad slot.
    std::uint16_t button_mask = 0;  ///< Xbox GameStream button bit mask.
    std::int16_t left_stick_x = 0;  ///< Left-stick horizontal axis.
    std::int16_t left_stick_y = 0;  ///< Left-stick vertical axis; final Sunshine sign awaits hardware validation.
    std::int16_t right_stick_x = 0;  ///< Right-stick horizontal axis.
    std::int16_t right_stick_y = 0;  ///< Right-stick vertical axis; final Sunshine sign awaits hardware validation.
    std::uint16_t left_trigger = 0;  ///< Left trigger in the Xbox unsigned 16-bit range.
    std::uint16_t right_trigger = 0;  ///< Right trigger in the Xbox unsigned 16-bit range.
    std::uint32_t physical_physicality = 0;  ///< Physical-control activity mask.
    std::uint32_t virtual_physicality = 0;  ///< Virtual-control activity mask.
  };

  /**
   * @brief Parsed Xbox four-motor vibration command.
   */
  struct vibration_t {
    std::uint8_t gamepad_index = 0;  ///< Xbox session gamepad slot.
    std::uint8_t left_motor_percent = 0;  ///< Strong-motor percentage.
    std::uint8_t right_motor_percent = 0;  ///< Weak-motor percentage.
    std::uint8_t left_trigger_percent = 0;  ///< Left-trigger motor percentage.
    std::uint8_t right_trigger_percent = 0;  ///< Right-trigger motor percentage.
    std::uint16_t duration_ms = 0;  ///< Vibration duration.
    std::uint16_t delay_ms = 0;  ///< Delay between repeated vibration commands.
    std::uint8_t repeat = 0;  ///< Repeat count.
  };

  /**
   * @brief Serialize a Home session play request.
   *
   * @param request Request model to serialize.
   * @return Compact JSON request body.
   */
  std::string serialize_home_play_request(const home_play_request_t &request);

  /**
   * @brief Parse a Home console discovery response.
   *
   * @param json JSON response body.
   * @return Parsed consoles or a structured validation error.
   */
  result_t<std::vector<console_t>> parse_console_list(std::string_view json);

  /**
   * @brief Parse a Home session creation response.
   *
   * @param json JSON response body.
   * @return Parsed session identifier or a structured validation error.
   */
  result_t<session_created_t> parse_session_created(std::string_view json);

  /**
   * @brief Parse a Home session state response.
   *
   * @param json JSON response body.
   * @return Parsed raw state or a structured validation error.
   */
  result_t<session_state_t> parse_session_state(std::string_view json);

  /**
   * @brief Determine whether a Home session has reached the only first-version ready state.
   *
   * @param state Parsed Home session state.
   * @return @c true only for the exact @c Provisioned state.
   */
  bool is_home_session_provisioned(const session_state_t &state);

  /**
   * @brief Parse a Home session configuration response.
   *
   * @param json JSON response body.
   * @return Parsed transport configuration or a structured validation error.
   */
  result_t<session_configuration_t> parse_session_configuration(std::string_view json);

  /**
   * @brief Parse the outer response used by SDP and ICE polling.
   *
   * @param json JSON response body.
   * @return Parsed inner exchange payload or a structured validation error.
   */
  result_t<exchange_response_t> parse_exchange_response(std::string_view json);

  /**
   * @brief Parse the JSON-encoded inner SDP answer from an exchange response.
   *
   * @param json Inner exchange JSON.
   * @return Complete SDP answer or a structured validation error.
   */
  result_t<std::string> parse_sdp_exchange(std::string_view json);

  /**
   * @brief Parse the JSON-encoded inner ICE candidate array.
   *
   * Xbox may encode each array item as either an object or a JSON string.
   *
   * @param json Inner exchange JSON.
   * @return Validated ICE candidates or a structured validation error.
   */
  result_t<std::vector<ice_candidate_t>> parse_ice_exchange(std::string_view json);

  /**
   * @brief Serialize an Xbox-compatible SDP offer request.
   *
   * @param offer SDP offer model.
   * @return Compact JSON request body with fixed channel version ranges.
   */
  std::string serialize_sdp_offer(const sdp_offer_t &offer);

  /**
   * @brief Serialize local ICE candidates for the Home session ICE endpoint.
   *
   * @param candidates Ordered local candidates, including any explicit end marker.
   * @return Compact Xbox-compatible ICE exchange request.
   */
  std::string serialize_ice_candidates(const std::vector<ice_candidate_t> &candidates);

  /**
   * @brief Create the message-channel handshake.
   *
   * @param id Non-secret message identifier.
   * @return Compact JSON handshake.
   */
  std::string make_message_handshake(std::string_view id);

  /**
   * @brief Validate a message-channel handshake acknowledgement.
   *
   * @param json JSON message received on the message channel.
   * @return Parsed acknowledgement version or a structured validation error.
   */
  result_t<std::string> parse_message_handshake_ack(std::string_view json);

  /**
   * @brief Create the fixed control-channel authorization request.
   *
   * @return Compact JSON authorization message.
   */
  std::string make_authorization_request();

  /**
   * @brief Create a gamepad add or remove control message.
   *
   * @param gamepad_index Xbox session gamepad slot.
   * @param was_added Whether the gamepad is being added.
   * @return Compact JSON control message.
   */
  std::string make_gamepad_changed(std::uint32_t gamepad_index, bool was_added);

  /**
   * @brief Create the six message-channel startup capability messages.
   *
   * @param parameters Deterministic display and installation values.
   * @return Messages ordered according to the Xbox startup contract.
   */
  std::vector<std::string> make_startup_messages(const startup_message_parameters_t &parameters);

  /**
   * @brief Encode an Xbox client-metadata input packet.
   *
   * @param header Sequence and timestamp. The report type is forced to client metadata.
   * @param max_touch_points Maximum supported touch contacts; zero disables touch.
   * @return Fifteen-byte little-endian packet.
   */
  std::vector<std::uint8_t> encode_client_metadata(input_header_t header, std::uint8_t max_touch_points);

  /**
   * @brief Encode one complete Xbox gamepad packet.
   *
   * @param header Sequence and timestamp. The report type is forced to gamepad.
   * @param frame Complete gamepad state.
   * @return Thirty-eight-byte little-endian packet.
   */
  std::vector<std::uint8_t> encode_gamepad_packet(input_header_t header, const gamepad_frame_t &frame);

  /**
   * @brief Decode one complete Xbox gamepad packet.
   *
   * @param bytes Packet bytes.
   * @return Decoded header and frame or a structured validation error.
   */
  result_t<std::pair<input_header_t, gamepad_frame_t>> decode_gamepad_packet(const std::vector<std::uint8_t> &bytes);

  /**
   * @brief Parse a server-to-client Xbox vibration packet.
   *
   * @param bytes Packet bytes, optionally prefixed by server metadata.
   * @return Parsed four-motor command or a structured validation error.
   */
  result_t<vibration_t> parse_vibration_packet(const std::vector<std::uint8_t> &bytes);
}  // namespace xbox_remote::protocol
