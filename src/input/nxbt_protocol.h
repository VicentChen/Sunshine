/**
 * @file src/input/nxbt_protocol.h
 * @brief Versioned wire protocol shared by Sunshine and the NXBT Bridge.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace input::nxbt {
  /**
   * @brief Magic value placed at the beginning of every NXBT Bridge packet.
   */
  constexpr std::uint32_t protocol_magic = 0x5442584eU;

  /**
   * @brief Current NXBT Bridge wire-protocol version.
   */
  constexpr std::uint16_t protocol_version = 1;

  /**
   * @brief Number of bytes in a serialized message header.
   */
  constexpr std::size_t message_header_size = 12;

  /**
   * @brief Kinds of packets carried by the NXBT Bridge protocol.
   */
  enum class message_type_e : std::uint8_t {
    hello = 1,  ///< Negotiate protocol version and capabilities.
    hello_ack = 2,  ///< Accept a hello message.
    error = 3,  ///< Reject a request with an error code.
    attach = 4,  ///< Bind a Sunshine controller to a Bridge controller slot.
    rebind = 5,  ///< Refresh the Moonlight client-relative controller id.
    state = 6,  ///< Submit one complete controller state.
    neutralize = 7,  ///< Release all inputs while keeping the controller attached.
    detach = 8,  ///< Remove a controller binding.
    ping = 9,  ///< Request a health-check response.
    pong = 10,  ///< Reply to a health-check request.
    status = 11,  ///< Report the Bridge controller connection state.
  };

  /**
   * @brief Connection state reported by the NXBT Bridge.
   */
  enum class controller_status_e : std::uint8_t {
    unavailable = 0,  ///< No usable Bluetooth adapter or controller exists.
    pairing = 1,  ///< The controller is waiting for initial Switch pairing.
    connecting = 2,  ///< The controller is attempting to connect.
    connected = 3,  ///< The controller is connected to the Switch.
    reconnecting = 4,  ///< The controller is reconnecting after a disconnect.
    failed = 5,  ///< Controller creation or Bluetooth operation failed.
  };

  /**
   * @brief Error codes returned in protocol error messages.
   */
  enum class protocol_error_e : std::uint16_t {
    none = 0,  ///< No error occurred.
    bad_magic = 1,  ///< The packet did not use the NXBT Bridge magic.
    unsupported_version = 2,  ///< The peer requested an unsupported version.
    invalid_length = 3,  ///< The packet length did not match its message type.
    unknown_message_type = 4,  ///< The packet used an unknown message type.
    truncated = 5,  ///< The packet ended before its declared payload.
  };

  /**
   * @brief Complete input snapshot submitted for one logical controller.
   */
  struct controller_state_t {
    std::uint8_t controller_id = 0;  ///< Bridge-owned controller slot.
    std::uint32_t button_flags = 0;  ///< NXBT button bit mask.
    std::uint8_t left_trigger = 0;  ///< Unmodified Sunshine left-trigger value.
    std::uint8_t right_trigger = 0;  ///< Unmodified Sunshine right-trigger value.
    std::int16_t left_stick_x = 0;  ///< Sunshine left-stick X value; positive is right.
    std::int16_t left_stick_y = 0;  ///< Sunshine left-stick Y value; positive is up.
    std::int16_t right_stick_x = 0;  ///< Sunshine right-stick X value; positive is right.
    std::int16_t right_stick_y = 0;  ///< Sunshine right-stick Y value; positive is up.
    std::uint32_t sequence = 0;  ///< Per-controller sequence number.
    std::uint64_t monotonic_timestamp_us = 0;  ///< Sender monotonic timestamp in microseconds.
  };

  /**
   * @brief One decoded protocol message.
   */
  struct message_t {
    message_type_e type = message_type_e::hello;  ///< Decoded packet type.
    std::uint32_t capabilities = 0;  ///< Hello or hello-ack capability bit mask.
    std::uint8_t controller_id = 0;  ///< Bridge controller slot for controller messages.
    std::uint8_t client_relative_id = 0;  ///< Moonlight client-relative controller id.
    controller_state_t state {};  ///< Complete state for a state packet.
    std::uint64_t monotonic_timestamp_us = 0;  ///< Ping or pong timestamp.
    controller_status_e status = controller_status_e::unavailable;  ///< Bridge controller status.
    protocol_error_e error = protocol_error_e::none;  ///< Error packet reason.
  };

  /**
   * @brief Outcome of decoding a wire-protocol packet.
   */
  struct decode_result_t {
    message_t message {};  ///< Valid decoded message when @c error is none.
    protocol_error_e error = protocol_error_e::none;  ///< Parsing failure, if any.
  };

  /**
   * @brief Serialize an NXBT Bridge message using explicit little-endian fields.
   *
   * @param message Message to serialize.
   * @return Complete packet including its 12-byte header.
   */
  std::vector<std::uint8_t> encode_message(const message_t &message);

  /**
   * @brief Decode a complete NXBT Bridge packet.
   *
   * @param bytes Packet bytes received from the peer.
   * @return A decoded message or an explicit protocol error.
   */
  decode_result_t decode_message(const std::vector<std::uint8_t> &bytes);

  /**
   * @brief Determine whether a sequence number is newer under uint32 wrap-around.
   *
   * @param candidate Candidate sequence number.
   * @param current Last accepted sequence number.
   * @return @c true when @p candidate is strictly newer than @p current.
   */
  bool sequence_is_newer(std::uint32_t candidate, std::uint32_t current);
}  // namespace input::nxbt
