/**
 * @file src/input/xbox_remote_sink.cpp
 * @brief Sunshine-to-Xbox controller mapping and feedback routing.
 */

#include "src/input/xbox_remote_sink.h"

// standard includes
#include <algorithm>
#include <utility>

// local includes
#include "src/xbox_remote/input_queue.h"

namespace input::xbox_remote {
  namespace {
    /**
     * @brief Add one Xbox button when its Sunshine source bit is pressed.
     *
     * @param source Sunshine button mask.
     * @param source_bit Sunshine button bit.
     * @param destination Xbox button bit.
     * @param result Accumulated Xbox button mask.
     */
    void map_button(
      std::uint32_t source,
      std::uint32_t source_bit,
      ::xbox_remote::protocol::gamepad_button_e destination,
      std::uint16_t &result
    ) {
      if ((source & source_bit) != 0) {
        result |= static_cast<std::uint16_t>(destination);
      }
    }

    /**
     * @brief Convert an Xbox percentage to Moonlight's unsigned 16-bit rumble range.
     *
     * @param percent Inclusive percentage from zero through one hundred.
     * @return Rounded unsigned 16-bit intensity.
     */
    std::uint16_t rumble_intensity(std::uint8_t percent) {
      const auto bounded = std::min<std::uint32_t>(percent, 100U);
      return static_cast<std::uint16_t>((bounded * 65535U + 50U) / 100U);
    }

  }  // namespace

  ::xbox_remote::protocol::gamepad_frame_t map_state(const platf::gamepad_state_t &state) {
    using ::xbox_remote::protocol::gamepad_button_e;
    ::xbox_remote::protocol::gamepad_frame_t frame;
    map_button(state.buttonFlags, platf::DPAD_UP, gamepad_button_e::dpad_up, frame.button_mask);
    map_button(state.buttonFlags, platf::DPAD_DOWN, gamepad_button_e::dpad_down, frame.button_mask);
    map_button(state.buttonFlags, platf::DPAD_LEFT, gamepad_button_e::dpad_left, frame.button_mask);
    map_button(state.buttonFlags, platf::DPAD_RIGHT, gamepad_button_e::dpad_right, frame.button_mask);
    map_button(state.buttonFlags, platf::START, gamepad_button_e::menu, frame.button_mask);
    map_button(state.buttonFlags, platf::BACK, gamepad_button_e::view, frame.button_mask);
    map_button(state.buttonFlags, platf::LEFT_STICK, gamepad_button_e::left_thumb, frame.button_mask);
    map_button(state.buttonFlags, platf::RIGHT_STICK, gamepad_button_e::right_thumb, frame.button_mask);
    map_button(state.buttonFlags, platf::LEFT_BUTTON, gamepad_button_e::left_shoulder, frame.button_mask);
    map_button(state.buttonFlags, platf::RIGHT_BUTTON, gamepad_button_e::right_shoulder, frame.button_mask);
    map_button(state.buttonFlags, platf::HOME, gamepad_button_e::nexus, frame.button_mask);
    map_button(state.buttonFlags, platf::A, gamepad_button_e::a, frame.button_mask);
    map_button(state.buttonFlags, platf::B, gamepad_button_e::b, frame.button_mask);
    map_button(state.buttonFlags, platf::X, gamepad_button_e::x, frame.button_mask);
    map_button(state.buttonFlags, platf::Y, gamepad_button_e::y, frame.button_mask);
    frame.left_stick_x = state.lsX;
    frame.left_stick_y = state.lsY;
    frame.right_stick_x = state.rsX;
    frame.right_stick_y = state.rsY;
    frame.left_trigger = ::xbox_remote::input::expand_trigger(state.lt);
    frame.right_trigger = ::xbox_remote::input::expand_trigger(state.rt);
    frame.physical_physicality = ::xbox_remote::input::activity_mask(frame);
    if ((state.buttonFlags & platf::MISC_BUTTON) != 0) {
      frame.physical_physicality |= static_cast<std::uint32_t>(::xbox_remote::protocol::gamepad_physicality_e::misc);
    }
    return frame;
  }

  sink_t::sink_t(std::shared_ptr<session_t> session):
      session_(std::move(session)),
      feedback_(std::make_shared<feedback_state_t>()) {
    if (session_) {
      const std::weak_ptr<feedback_state_t> weak_feedback {feedback_};
      session_->set_vibration_handler([weak_feedback](const auto &vibration) {
        deliver_vibration(weak_feedback, vibration);
      });
    }
  }

  sink_t::~sink_t() {
    if (session_) {
      try {
        session_->set_vibration_handler({});
      } catch (...) {
      }
    }
  }

  void sink_t::deliver_vibration(
    const std::weak_ptr<feedback_state_t> &weak_state,
    const ::xbox_remote::protocol::vibration_t &vibration
  ) {
    if (vibration.gamepad_index != 0) {
      return;
    }
    const auto state = weak_state.lock();
    if (!state) {
      return;
    }
    platf::feedback_queue_t queue;
    std::uint8_t client_relative_index = 0;
    {
      std::lock_guard lock(state->mutex);
      if (!state->global_index || !state->queue) {
        return;
      }
      queue = state->queue;
      client_relative_index = state->client_relative_index;
    }
    queue->raise(platf::gamepad_feedback_msg_t::make_rumble(client_relative_index, rumble_intensity(vibration.left_motor_percent), rumble_intensity(vibration.right_motor_percent)));
    queue->raise(platf::gamepad_feedback_msg_t::make_rumble_triggers(client_relative_index, rumble_intensity(vibration.left_trigger_percent), rumble_intensity(vibration.right_trigger_percent)));
  }

  bool sink_t::alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue) {
    static_cast<void>(arrival);
    if (!session_ || !feedback_queue || id.globalIndex < 0 || id.globalIndex >= platf::MAX_GAMEPADS) {
      return false;
    }
    {
      std::lock_guard lock(feedback_->mutex);
      if (feedback_->global_index) {
        return false;
      }
      feedback_->global_index = id.globalIndex;
      feedback_->client_relative_index = id.clientRelativeIndex;
      feedback_->queue = feedback_queue;
    }
    try {
      if (!session_->attach({})) {
        std::lock_guard lock(feedback_->mutex);
        feedback_->global_index.reset();
        feedback_->queue.reset();
        return false;
      }
    } catch (...) {
      std::lock_guard lock(feedback_->mutex);
      feedback_->global_index.reset();
      feedback_->queue.reset();
      return false;
    }
    return true;
  }

  bool sink_t::rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue) {
    if (!feedback_queue || !owns_slot(id)) {
      return false;
    }
    try {
      if (!session_->rebind()) {
        return false;
      }
    } catch (...) {
      return false;
    }
    std::lock_guard lock(feedback_->mutex);
    if (feedback_->global_index != id.globalIndex) {
      return false;
    }
    feedback_->client_relative_index = id.clientRelativeIndex;
    feedback_->queue = std::move(feedback_queue);
    return true;
  }

  bool sink_t::update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state) {
    if (!owns_slot(id)) {
      return false;
    }
    try {
      return session_->submit(map_state(state));
    } catch (...) {
      return false;
    }
  }

  void sink_t::neutralize(const platf::gamepad_id_t &id) {
    if (!owns_slot(id)) {
      return;
    }
    try {
      session_->neutralize();
    } catch (...) {
    }
  }

  void sink_t::free(const platf::gamepad_id_t &id) {
    if (!session_) {
      return;
    }
    {
      std::lock_guard lock(feedback_->mutex);
      if (feedback_->global_index != id.globalIndex) {
        return;
      }
      feedback_->global_index.reset();
      feedback_->queue.reset();
    }
    try {
      session_->detach();
    } catch (...) {
    }
  }

  bool sink_t::owns_slot(const platf::gamepad_id_t &id) const {
    if (!session_) {
      return false;
    }
    std::lock_guard lock(feedback_->mutex);
    return feedback_->global_index == id.globalIndex;
  }
}  // namespace input::xbox_remote
