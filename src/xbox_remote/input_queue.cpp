/**
 * @file src/xbox_remote/input_queue.cpp
 * @brief Bounded Xbox Remote Play input scheduling and packetization.
 */

#include "src/xbox_remote/input_queue.h"

// standard includes
#include <algorithm>
#include <utility>

namespace xbox_remote::input {
  namespace {
    constexpr std::uint32_t mask(protocol::gamepad_physicality_e value) {
      return static_cast<std::uint32_t>(value);
    }

    bool is_state_bearing(item_kind_e kind) {
      return kind == item_kind_e::state || kind == item_kind_e::neutralize;
    }
  }  // namespace

  outbound_queue_t::outbound_queue_t(queue_options_t options):
      options_(std::move(options)) {
  }

  void outbound_queue_t::purge_expired(time_point_t now) {
    while (!edges_.empty() && now - edges_.front().timestamp >= options_.edge_ttl) {
      edges_.pop_front();
    }
  }

  bool outbound_queue_t::push_control(
    item_kind_e kind,
    const protocol::gamepad_frame_t &frame,
    time_point_t now
  ) {
    if (options_.control_capacity == 0 || controls_.size() >= options_.control_capacity) {
      return false;
    }
    controls_.push_back({{kind, frame}, next_version_++, now});
    return true;
  }

  bool outbound_queue_t::attach(const protocol::gamepad_frame_t &frame, time_point_t now) {
    controls_.clear();
    edges_.clear();
    emitted_version_ = 0;
    if (!push_control(item_kind_e::attach, frame, now)) {
      latest_.reset();
      return false;
    }
    latest_ = versioned_item_t {{item_kind_e::state, frame}, next_version_++, now};
    return true;
  }

  void outbound_queue_t::submit(const protocol::gamepad_frame_t &frame, time_point_t now) {
    purge_expired(now);
    const auto version = next_version_++;
    const bool digital_edge = latest_ && latest_->item.frame.button_mask != frame.button_mask;
    latest_ = versioned_item_t {{item_kind_e::state, frame}, version, now};
    if (!digital_edge || options_.edge_capacity == 0) {
      if (digital_edge) {
        ++dropped_edges_;
      }
      return;
    }
    if (edges_.size() >= options_.edge_capacity) {
      edges_.pop_front();
      ++dropped_edges_;
    }
    edges_.push_back(*latest_);
  }

  bool outbound_queue_t::neutralize(time_point_t now) {
    controls_.clear();
    edges_.clear();
    latest_ = versioned_item_t {{item_kind_e::neutralize, {}}, next_version_, now};
    if (!push_control(item_kind_e::neutralize, {}, now)) {
      latest_.reset();
      return false;
    }
    return true;
  }

  bool outbound_queue_t::detach(time_point_t now) {
    controls_.clear();
    edges_.clear();
    latest_.reset();
    return push_control(item_kind_e::detach, {}, now);
  }

  void outbound_queue_t::on_reconnect() {
    controls_.clear();
    edges_.clear();
    if (latest_) {
      latest_->item.kind = item_kind_e::state;
      latest_->version = next_version_++;
    }
  }

  std::optional<item_t> outbound_queue_t::take(time_point_t now) {
    if (!controls_.empty()) {
      auto next = std::move(controls_.front());
      controls_.pop_front();
      if (is_state_bearing(next.item.kind)) {
        emitted_version_ = std::max(emitted_version_, next.version);
      }
      return std::move(next.item);
    }

    purge_expired(now);
    if (!edges_.empty()) {
      auto next = std::move(edges_.front());
      edges_.pop_front();
      emitted_version_ = std::max(emitted_version_, next.version);
      return std::move(next.item);
    }
    if (latest_ && latest_->version > emitted_version_) {
      emitted_version_ = latest_->version;
      return latest_->item;
    }
    return std::nullopt;
  }

  std::uint64_t outbound_queue_t::dropped_edges() const {
    return dropped_edges_;
  }

  std::size_t outbound_queue_t::edge_count() const {
    return edges_.size();
  }

  packetizer_t::packetizer_t(std::uint32_t initial_sequence):
      sequence_(initial_sequence) {
  }

  std::vector<std::uint8_t> packetizer_t::encode(const protocol::gamepad_frame_t &frame, double timestamp_ms) {
    protocol::input_header_t header {protocol::report_type_e::gamepad, sequence_++, timestamp_ms};
    return protocol::encode_gamepad_packet(header, frame);
  }

  void packetizer_t::reset(std::uint32_t initial_sequence) {
    sequence_ = initial_sequence;
  }

  std::uint32_t packetizer_t::next_sequence() const {
    return sequence_;
  }

  std::uint32_t activity_mask(const protocol::gamepad_frame_t &frame) {
    using protocol::gamepad_button_e;
    using protocol::gamepad_physicality_e;
    std::uint32_t result = 0;
    const auto pressed = [&](gamepad_button_e button) {
      return (frame.button_mask & static_cast<std::uint16_t>(button)) != 0;
    };
    const auto add_button = [&](gamepad_button_e button, gamepad_physicality_e physicality) {
      if (pressed(button)) {
        result |= mask(physicality);
      }
    };

    add_button(gamepad_button_e::dpad_up, gamepad_physicality_e::dpad_up);
    add_button(gamepad_button_e::dpad_down, gamepad_physicality_e::dpad_down);
    add_button(gamepad_button_e::dpad_left, gamepad_physicality_e::dpad_left);
    add_button(gamepad_button_e::dpad_right, gamepad_physicality_e::dpad_right);
    add_button(gamepad_button_e::menu, gamepad_physicality_e::menu);
    add_button(gamepad_button_e::view, gamepad_physicality_e::view);
    add_button(gamepad_button_e::left_thumb, gamepad_physicality_e::left_thumb);
    add_button(gamepad_button_e::right_thumb, gamepad_physicality_e::right_thumb);
    add_button(gamepad_button_e::left_shoulder, gamepad_physicality_e::left_shoulder);
    add_button(gamepad_button_e::right_shoulder, gamepad_physicality_e::right_shoulder);
    add_button(gamepad_button_e::nexus, gamepad_physicality_e::nexus);
    add_button(gamepad_button_e::a, gamepad_physicality_e::a);
    add_button(gamepad_button_e::b, gamepad_physicality_e::b);
    add_button(gamepad_button_e::x, gamepad_physicality_e::x);
    add_button(gamepad_button_e::y, gamepad_physicality_e::y);
    if (frame.left_trigger != 0) {
      result |= mask(gamepad_physicality_e::left_trigger);
    }
    if (frame.right_trigger != 0) {
      result |= mask(gamepad_physicality_e::right_trigger);
    }
    if (frame.left_stick_x != 0) {
      result |= mask(gamepad_physicality_e::left_thumb_x);
    }
    if (frame.left_stick_y != 0) {
      result |= mask(gamepad_physicality_e::left_thumb_y);
    }
    if (frame.right_stick_x != 0) {
      result |= mask(gamepad_physicality_e::right_thumb_x);
    }
    if (frame.right_stick_y != 0) {
      result |= mask(gamepad_physicality_e::right_thumb_y);
    }
    return result;
  }
}  // namespace xbox_remote::input
