/**
 * @file src/input/nxbt_protocol.cpp
 * @brief Serialization implementation for the NXBT Bridge protocol.
 */

#include "src/input/nxbt_protocol.h"

namespace input::nxbt {
  namespace {
    /**
     * @brief Append an unsigned integer in little-endian byte order.
     *
     * @tparam T Unsigned integer type to append.
     * @param bytes Destination byte vector.
     * @param value Value to append.
     */
    template<typename T>
    void append_le(std::vector<std::uint8_t> &bytes, T value) {
      for (std::size_t index = 0; index < sizeof(T); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
      }
    }

    /**
     * @brief Read an unsigned integer in little-endian byte order.
     *
     * @tparam T Unsigned integer type to read.
     * @param bytes Source byte vector.
     * @param offset Byte offset in @p bytes.
     * @return Decoded value. Callers must first validate the packet length.
     */
    template<typename T>
    T read_le(const std::vector<std::uint8_t> &bytes, std::size_t &offset) {
      T value = 0;
      for (std::size_t index = 0; index < sizeof(T); ++index) {
        value |= static_cast<T>(bytes[offset++]) << (index * 8U);
      }
      return value;
    }

    /**
     * @brief Get the fixed payload size for a packet type.
     *
     * @param type Protocol packet type.
     * @return Payload byte count, or zero for unknown types.
     */
    std::size_t payload_size(message_type_e type) {
      switch (type) {
        case message_type_e::hello:
        case message_type_e::hello_ack:
          return 4;
        case message_type_e::error:
          return 4;
        case message_type_e::attach:
        case message_type_e::rebind:
        case message_type_e::neutralize:
        case message_type_e::detach:
        case message_type_e::status:
          return 4;
        case message_type_e::state:
          return 28;
        case message_type_e::ping:
        case message_type_e::pong:
          return 8;
      }
      return 0;
    }

    /**
     * @brief Check whether a raw byte is a defined message type.
     *
     * @param value Raw message type byte.
     * @return @c true when the type is recognized by protocol version 1.
     */
    bool is_message_type(std::uint8_t value) {
      return value >= static_cast<std::uint8_t>(message_type_e::hello) && value <= static_cast<std::uint8_t>(message_type_e::status);
    }

  }  // namespace

  std::vector<std::uint8_t> encode_message(const message_t &message) {
    std::vector<std::uint8_t> payload;
    payload.reserve(payload_size(message.type));

    switch (message.type) {
      case message_type_e::hello:
      case message_type_e::hello_ack:
        append_le(payload, message.capabilities);
        break;
      case message_type_e::error:
        append_le(payload, static_cast<std::uint16_t>(message.error));
        append_le(payload, std::uint16_t {0});
        break;
      case message_type_e::attach:
        payload.push_back(message.controller_id);
        payload.push_back(message.client_relative_id);
        payload.insert(payload.end(), 2, 0);
        break;
      case message_type_e::rebind:
        payload.push_back(message.controller_id);
        payload.push_back(message.client_relative_id);
        payload.insert(payload.end(), 2, 0);
        break;
      case message_type_e::state:
        payload.push_back(message.state.controller_id);
        payload.push_back(message.state.left_trigger);
        payload.push_back(message.state.right_trigger);
        payload.push_back(0);
        append_le(payload, message.state.button_flags);
        append_le(payload, static_cast<std::uint16_t>(message.state.left_stick_x));
        append_le(payload, static_cast<std::uint16_t>(message.state.left_stick_y));
        append_le(payload, static_cast<std::uint16_t>(message.state.right_stick_x));
        append_le(payload, static_cast<std::uint16_t>(message.state.right_stick_y));
        append_le(payload, message.state.sequence);
        append_le(payload, message.state.monotonic_timestamp_us);
        break;
      case message_type_e::neutralize:
        payload.push_back(message.controller_id);
        payload.insert(payload.end(), 3, 0);
        break;
      case message_type_e::detach:
        payload.push_back(message.controller_id);
        payload.insert(payload.end(), 3, 0);
        break;
      case message_type_e::ping:
        append_le(payload, message.monotonic_timestamp_us);
        break;
      case message_type_e::pong:
        append_le(payload, message.monotonic_timestamp_us);
        break;
      case message_type_e::status:
        payload.push_back(message.controller_id);
        payload.push_back(static_cast<std::uint8_t>(message.status));
        payload.insert(payload.end(), 2, 0);
        break;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(message_header_size + payload.size());
    append_le(bytes, protocol_magic);
    append_le(bytes, protocol_version);
    bytes.push_back(static_cast<std::uint8_t>(message.type));
    bytes.push_back(0);
    append_le(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
  }

  decode_result_t decode_message(const std::vector<std::uint8_t> &bytes) {
    decode_result_t result;
    if (bytes.size() < message_header_size) {
      result.error = protocol_error_e::truncated;
      return result;
    }

    std::size_t offset = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint8_t type = 0;
    std::uint32_t length = 0;
    magic = read_le<std::uint32_t>(bytes, offset);
    version = read_le<std::uint16_t>(bytes, offset);
    type = read_le<std::uint8_t>(bytes, offset);
    static_cast<void>(read_le<std::uint8_t>(bytes, offset));
    length = read_le<std::uint32_t>(bytes, offset);
    if (magic != protocol_magic) {
      result.error = protocol_error_e::bad_magic;
      return result;
    }
    if (version != protocol_version) {
      result.error = protocol_error_e::unsupported_version;
      return result;
    }
    if (!is_message_type(type)) {
      result.error = protocol_error_e::unknown_message_type;
      return result;
    }
    const auto message_type = static_cast<message_type_e>(type);
    if (length != payload_size(message_type)) {
      result.error = protocol_error_e::invalid_length;
      return result;
    }
    if (bytes.size() - offset < length) {
      result.error = protocol_error_e::truncated;
      return result;
    }
    if (bytes.size() - offset != length) {
      result.error = protocol_error_e::invalid_length;
      return result;
    }

    result.message.type = message_type;
    switch (message_type) {
      case message_type_e::hello:
      case message_type_e::hello_ack:
        result.message.capabilities = read_le<std::uint32_t>(bytes, offset);
        break;
      case message_type_e::error:
        {
          const auto error = read_le<std::uint16_t>(bytes, offset);
          result.message.error = static_cast<protocol_error_e>(error);
          break;
        }
      case message_type_e::attach:
      case message_type_e::rebind:
        result.message.controller_id = bytes[offset++];
        result.message.client_relative_id = bytes[offset++];
        offset += 2;
        break;
      case message_type_e::state:
        {
          auto &state = result.message.state;
          state.controller_id = bytes[offset++];
          state.left_trigger = bytes[offset++];
          state.right_trigger = bytes[offset++];
          ++offset;
          state.button_flags = read_le<std::uint32_t>(bytes, offset);
          const auto left_stick_x = read_le<std::uint16_t>(bytes, offset);
          const auto left_stick_y = read_le<std::uint16_t>(bytes, offset);
          const auto right_stick_x = read_le<std::uint16_t>(bytes, offset);
          const auto right_stick_y = read_le<std::uint16_t>(bytes, offset);
          state.sequence = read_le<std::uint32_t>(bytes, offset);
          state.monotonic_timestamp_us = read_le<std::uint64_t>(bytes, offset);
          state.left_stick_x = static_cast<std::int16_t>(left_stick_x);
          state.left_stick_y = static_cast<std::int16_t>(left_stick_y);
          state.right_stick_x = static_cast<std::int16_t>(right_stick_x);
          state.right_stick_y = static_cast<std::int16_t>(right_stick_y);
          break;
        }
      case message_type_e::neutralize:
      case message_type_e::detach:
        result.message.controller_id = bytes[offset];
        break;
      case message_type_e::ping:
      case message_type_e::pong:
        result.message.monotonic_timestamp_us = read_le<std::uint64_t>(bytes, offset);
        break;
      case message_type_e::status:
        result.message.controller_id = bytes[offset++];
        result.message.status = static_cast<controller_status_e>(bytes[offset]);
        break;
    }
    return result;
  }

  bool sequence_is_newer(std::uint32_t candidate, std::uint32_t current) {
    return candidate != current && static_cast<std::uint32_t>(candidate - current) < 0x80000000U;
  }
}  // namespace input::nxbt
