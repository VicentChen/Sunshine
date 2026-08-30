/**
 * @file src/xbox_remote/startup.cpp
 * @brief Xbox data-channel startup-handshake state machine.
 */

#include "src/xbox_remote/startup.h"

// standard includes
#include <algorithm>
#include <array>
#include <utility>

namespace xbox_remote::startup {
  namespace {
    constexpr std::array<std::string_view, 3> required_channels {"control", "input", "message"};
  }

  coordinator_t::coordinator_t(sender_t &sender, options_t options):
      sender_(sender),
      options_(std::move(options)) {
  }

  result_t<bool> coordinator_t::fail(error_e code, std::string stage, std::string message) {
    failure_ = {code, std::move(stage), std::move(message)};
    state_ = code == error_e::cancelled ? state_e::cancelled : state_e::failed;
    return {{}, failure_};
  }

  result_t<bool> coordinator_t::send_text(std::string_view channel, std::string payload, std::string_view stage) {
    if (!sender_.send_text(channel, payload)) {
      return fail(error_e::send_failed, std::string(stage), "Xbox startup channel rejected a text message");
    }
    return {true, {}};
  }

  result_t<bool> coordinator_t::send_binary(
    std::string_view channel,
    std::vector<std::uint8_t> payload,
    std::string_view stage
  ) {
    if (!sender_.send_binary(channel, payload)) {
      return fail(error_e::send_failed, std::string(stage), "Xbox startup channel rejected a binary message");
    }
    return {true, {}};
  }

  result_t<bool> coordinator_t::start(time_point_t now) {
    if (state_ != state_e::idle) {
      return fail(error_e::invalid_state, "handshake", "Xbox startup handshake was already started");
    }
    auto sent = send_text("message", protocol::make_message_handshake(options_.handshake_id), "handshake_send");
    if (!sent) {
      return sent;
    }
    ack_deadline_ = now + options_.ack_timeout;
    state_ = state_e::waiting_ack;
    return {true, {}};
  }

  result_t<bool> coordinator_t::on_message(std::string_view payload, time_point_t now) {
    const auto acknowledgement = protocol::parse_message_handshake_ack(payload);
    if (state_ == state_e::waiting_gamepad_add || state_ == state_e::ready) {
      if (acknowledgement) {
        return {false, {}};
      }
      return {false, {}};
    }
    if (state_ != state_e::waiting_ack) {
      return fail(error_e::invalid_state, "handshake_ack", "Xbox startup is not waiting for an acknowledgement");
    }
    if (!acknowledgement) {
      return fail(error_e::invalid_ack, "handshake_ack", "Xbox returned an invalid startup acknowledgement");
    }

    auto sent = send_text("control", protocol::make_authorization_request(), "authorization");
    if (!sent) {
      return sent;
    }
    sent = send_text("control", protocol::make_gamepad_changed(options_.gamepad_index, false), "gamepad_remove");
    if (!sent) {
      return sent;
    }
    gamepad_add_at_ = now + options_.gamepad_add_delay;
    state_ = state_e::waiting_gamepad_add;
    return {true, {}};
  }

  result_t<bool> coordinator_t::finish_startup(time_point_t now) {
    auto sent = send_text("control", protocol::make_gamepad_changed(options_.gamepad_index, true), "gamepad_add");
    if (!sent) {
      return sent;
    }
    for (auto &message : protocol::make_startup_messages(options_.startup_messages)) {
      sent = send_text("message", std::move(message), "startup_capabilities");
      if (!sent) {
        return sent;
      }
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
    protocol::input_header_t header {protocol::report_type_e::client_metadata, 0, elapsed};
    auto binary_sent = send_binary("input", protocol::encode_client_metadata(header, 0), "client_metadata");
    if (!binary_sent) {
      return binary_sent;
    }
    state_ = state_e::ready;
    return {true, {}};
  }

  result_t<bool> coordinator_t::poll(time_point_t now, bool cancelled) {
    if (state_ == state_e::failed || state_ == state_e::cancelled) {
      return {{}, failure_};
    }
    if (cancelled) {
      return fail(error_e::cancelled, "startup", "Xbox startup was cancelled");
    }
    if (state_ == state_e::waiting_ack && now >= ack_deadline_) {
      return fail(error_e::timeout, "handshake_ack", "Xbox startup acknowledgement timed out");
    }
    if (state_ == state_e::waiting_gamepad_add && now >= gamepad_add_at_) {
      return finish_startup(now);
    }
    return {state_ == state_e::ready, {}};
  }

  result_t<bool> coordinator_t::on_channel_closed(std::string_view channel) {
    if (state_ == state_e::failed || state_ == state_e::cancelled) {
      return {{}, failure_};
    }
    if (std::ranges::find(required_channels, channel) == required_channels.end()) {
      return {false, {}};
    }
    return fail(error_e::channel_closed, "data_channel", "A required Xbox startup channel closed");
  }

  state_e coordinator_t::state() const {
    return state_;
  }

  const failure_t &coordinator_t::failure() const {
    return failure_;
  }
}  // namespace xbox_remote::startup
