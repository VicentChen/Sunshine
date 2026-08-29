/**
 * @file src/input/nxbt_sink.cpp
 * @brief Sunshine gamepad mapping adapter for the NXBT IPC client.
 */

#include "src/input/nxbt_sink.h"

// standard includes
#include <chrono>
#include <optional>
#include <utility>

namespace input::nxbt {
  namespace {
    /**
     * @brief Convert a Sunshine global slot to a validated Bridge slot.
     *
     * @param id Sunshine global and client-relative controller identifiers.
     * @return Bridge slot, or no value when the identifier is invalid.
     */
    std::optional<std::uint8_t> controller_slot(const platf::gamepad_id_t &id) {
      if (id.globalIndex < 0 || id.globalIndex >= platf::MAX_GAMEPADS) {
        return std::nullopt;
      }
      return static_cast<std::uint8_t>(id.globalIndex);
    }

    /**
     * @brief Return the current steady-clock timestamp in protocol microseconds.
     */
    std::uint64_t now_us() {
      return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
  }  // namespace

  sink_t::sink_t(
    std::shared_ptr<client_t> client,
    face_button_policy_e face_button_policy,
    std::optional<std::uint8_t> fixed_controller_slot,
    std::uint8_t trigger_press_threshold,
    std::uint8_t trigger_release_threshold
  ):
      client_(std::move(client)),
      face_button_policy_(face_button_policy),
      fixed_controller_slot_(fixed_controller_slot),
      trigger_press_threshold_(trigger_press_threshold),
      trigger_release_threshold_(trigger_release_threshold) {
  }

  bool sink_t::alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue) {
    static_cast<void>(arrival);
    static_cast<void>(feedback_queue);
    const auto sunshine_slot = controller_slot(id);
    const auto controller_id = fixed_controller_slot_ ? fixed_controller_slot_ : sunshine_slot;
    if (!controller_id || !client_) {
      return false;
    }
    std::lock_guard lock(mutex_);
    if (fixed_controller_slot_ && fixed_sunshine_slot_ && *fixed_sunshine_slot_ != id.globalIndex) {
      return false;
    }
    auto &slot = slots_[id.globalIndex];
    if (slot.allocated || !client_->attach(*controller_id, id.clientRelativeIndex)) {
      return false;
    }
    slot = {};
    slot.allocated = true;
    if (fixed_controller_slot_) {
      fixed_sunshine_slot_ = id.globalIndex;
    }
    return true;
  }

  bool sink_t::rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue) {
    static_cast<void>(feedback_queue);
    const auto sunshine_slot = controller_slot(id);
    const auto controller_id = fixed_controller_slot_ ? fixed_controller_slot_ : sunshine_slot;
    if (!controller_id || !client_) {
      return false;
    }
    std::lock_guard lock(mutex_);
    return slots_[id.globalIndex].allocated && (!fixed_sunshine_slot_ || *fixed_sunshine_slot_ == id.globalIndex) && client_->rebind(*controller_id, id.clientRelativeIndex);
  }

  bool sink_t::update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state) {
    const auto sunshine_slot = controller_slot(id);
    const auto controller_id = fixed_controller_slot_ ? fixed_controller_slot_ : sunshine_slot;
    if (!controller_id || !client_) {
      return false;
    }
    std::lock_guard lock(mutex_);
    auto &slot = slots_[id.globalIndex];
    if (!slot.allocated) {
      return false;
    }
    controller_state_t mapped {
      .controller_id = *controller_id,
      .button_flags = map_buttons(state.buttonFlags, face_button_policy_) |
                      map_triggers(state.lt, state.rt, slot.triggers, trigger_press_threshold_, trigger_release_threshold_),
      .left_trigger = state.lt,
      .right_trigger = state.rt,
      .left_stick_x = state.lsX,
      .left_stick_y = state.lsY,
      .right_stick_x = state.rsX,
      .right_stick_y = state.rsY,
      .sequence = ++slot.sequence,
      .monotonic_timestamp_us = now_us(),
    };
    return client_->update(mapped);
  }

  void sink_t::neutralize(const platf::gamepad_id_t &id) {
    const auto sunshine_slot = controller_slot(id);
    const auto controller_id = fixed_controller_slot_ ? fixed_controller_slot_ : sunshine_slot;
    if (!controller_id || !client_) {
      return;
    }
    std::lock_guard lock(mutex_);
    auto &slot = slots_[id.globalIndex];
    if (!slot.allocated) {
      return;
    }
    slot.triggers = {};
    client_->neutralize(*controller_id);
  }

  void sink_t::free(const platf::gamepad_id_t &id) {
    const auto sunshine_slot = controller_slot(id);
    const auto controller_id = fixed_controller_slot_ ? fixed_controller_slot_ : sunshine_slot;
    if (!controller_id || !client_) {
      return;
    }
    std::lock_guard lock(mutex_);
    auto &slot = slots_[id.globalIndex];
    if (!slot.allocated) {
      return;
    }
    client_->detach(*controller_id);
    slot = {};
    if (fixed_sunshine_slot_ == id.globalIndex) {
      fixed_sunshine_slot_.reset();
    }
  }
}  // namespace input::nxbt
