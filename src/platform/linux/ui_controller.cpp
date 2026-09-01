/**
 * @file src/platform/linux/ui_controller.cpp
 * @brief Controller chord, ownership, interception, and focus policy.
 */

// local includes
#include "ui_controller.h"

namespace platf::ui {
  namespace {
    constexpr std::uint32_t navigation_up = 1U << 0U;
    constexpr std::uint32_t navigation_down = 1U << 1U;
    constexpr std::uint32_t navigation_left = 1U << 2U;
    constexpr std::uint32_t navigation_right = 1U << 3U;
    constexpr std::uint32_t navigation_confirm = 1U << 4U;
    constexpr std::uint32_t navigation_back = 1U << 5U;
  }  // namespace

  std::int8_t controller_t::axis_direction(std::int16_t value, std::int8_t previous) noexcept {
    if (previous < 0 && value < -axis_release_) {
      return -1;
    }
    if (previous > 0 && value > axis_release_) {
      return 1;
    }
    if (value <= -axis_press_) {
      return -1;
    }
    if (value >= axis_press_) {
      return 1;
    }
    return 0;
  }

  std::uint32_t controller_t::navigation_mask(const controller_input_t &input, input_state_t &state) noexcept {
    state.axis_x = axis_direction(input.left_stick_x, state.axis_x);
    state.axis_y = axis_direction(input.left_stick_y, state.axis_y);
    std::uint32_t mask = 0;
    if ((input.buttons & platf::DPAD_UP) != 0 || state.axis_y > 0) {
      mask |= navigation_up;
    }
    if ((input.buttons & platf::DPAD_DOWN) != 0 || state.axis_y < 0) {
      mask |= navigation_down;
    }
    if ((input.buttons & platf::DPAD_LEFT) != 0 || state.axis_x < 0) {
      mask |= navigation_left;
    }
    if ((input.buttons & platf::DPAD_RIGHT) != 0 || state.axis_x > 0) {
      mask |= navigation_right;
    }
    if ((input.buttons & platf::A) != 0) {
      mask |= navigation_confirm;
    }
    if ((input.buttons & platf::BACK) != 0) {
      mask |= navigation_back;
    }
    return mask;
  }

  decision_t controller_t::update(std::uint8_t controller, const controller_input_t &input, clock_t::time_point now) {
    std::lock_guard lock {mutex_};
    decision_t result {.visible = visible_};
    if (controller >= inputs_.size()) {
      result.consume = visible_;
      return result;
    }

    auto &state = inputs_[controller];
    const auto chord_buttons = input.buttons & chord_;
    const bool chord_down = chord_buttons == chord_;

    if (release_controller_ && *release_controller_ == controller) {
      result.consume = true;
      if (chord_buttons == 0) {
        release_controller_.reset();
        state = {};
        if (!visible_) {
          owner_.reset();
        }
      }
      return result;
    }

    if (!visible_) {
      if (!chord_down) {
        state.chord_since.reset();
        state.previous_navigation = navigation_mask(input, state);
        return result;
      }

      result.consume = true;
      if (!state.chord_since) {
        state.chord_since = now;
        result.neutralize = true;
      }
      if (now - *state.chord_since >= hold_time_) {
        visible_ = true;
        owner_ = controller;
        release_controller_ = controller;
        ++revision_;
        result.neutralize = true;
        result.visibility_changed = true;
        result.visible = true;
      }
      return result;
    }

    result.consume = true;
    if (!owner_ || *owner_ != controller) {
      state.chord_since.reset();
      state.previous_navigation = navigation_mask(input, state);
      return result;
    }

    if (chord_down) {
      if (!state.chord_since) {
        state.chord_since = now;
      }
      if (now - *state.chord_since >= hold_time_) {
        visible_ = false;
        release_controller_ = controller;
        ++revision_;
        result.neutralize = true;
        result.visibility_changed = true;
        result.visible = false;
      }
      return result;
    }
    state.chord_since.reset();

    const auto current_navigation = navigation_mask(input, state);
    const auto pressed = current_navigation & ~state.previous_navigation;
    state.previous_navigation = current_navigation;
    if ((pressed & navigation_up) != 0) {
      result.navigation = navigation_e::up;
      focus_ = static_cast<std::uint8_t>((focus_ + item_count_ - 1U) % item_count_);
      ++revision_;
    } else if ((pressed & navigation_down) != 0) {
      result.navigation = navigation_e::down;
      focus_ = static_cast<std::uint8_t>((focus_ + 1U) % item_count_);
      ++revision_;
    } else if ((pressed & navigation_left) != 0) {
      result.navigation = navigation_e::left;
      focus_ = static_cast<std::uint8_t>((focus_ + item_count_ - 1U) % item_count_);
      ++revision_;
    } else if ((pressed & navigation_right) != 0) {
      result.navigation = navigation_e::right;
      focus_ = static_cast<std::uint8_t>((focus_ + 1U) % item_count_);
      ++revision_;
    } else if ((pressed & navigation_confirm) != 0) {
      result.navigation = navigation_e::confirm;
    } else if ((pressed & navigation_back) != 0) {
      result.navigation = navigation_e::back;
    }
    return result;
  }

  decision_t controller_t::tick(clock_t::time_point now) {
    std::lock_guard lock {mutex_};
    decision_t result {.visible = visible_};

    // A completed toggle stays behind the full-release gate. Without this
    // check, the still-held chord could toggle again on the next video frame.
    if (release_controller_) {
      return result;
    }

    if (!visible_) {
      for (std::uint8_t controller = 0; controller < inputs_.size(); ++controller) {
        const auto &state = inputs_[controller];
        if (!state.chord_since || now - *state.chord_since < hold_time_) {
          continue;
        }
        visible_ = true;
        owner_ = controller;
        release_controller_ = controller;
        ++revision_;
        result.visibility_changed = true;
        result.visible = true;
        return result;
      }
      return result;
    }

    if (!owner_) {
      return result;
    }
    const auto &state = inputs_[*owner_];
    if (!state.chord_since || now - *state.chord_since < hold_time_) {
      return result;
    }
    visible_ = false;
    release_controller_ = *owner_;
    ++revision_;
    result.visibility_changed = true;
    result.visible = false;
    return result;
  }

  decision_t controller_t::disconnect(std::uint8_t controller) {
    std::lock_guard lock {mutex_};
    decision_t result {.visible = visible_};
    if (controller >= inputs_.size()) {
      return result;
    }
    inputs_[controller] = {};
    if (release_controller_ == controller) {
      release_controller_.reset();
    }
    if (!owner_ || *owner_ != controller) {
      return result;
    }

    owner_.reset();
    const bool changed = visible_;
    visible_ = false;
    if (changed) {
      ++revision_;
    }
    result.consume = true;
    result.neutralize = true;
    result.visibility_changed = changed;
    result.visible = false;
    return result;
  }

  void controller_t::reset() {
    std::lock_guard lock {mutex_};
    inputs_ = {};
    owner_.reset();
    release_controller_.reset();
    if (visible_) {
      visible_ = false;
      ++revision_;
    }
  }

  snapshot_t controller_t::snapshot() const {
    std::lock_guard lock {mutex_};
    return {visible_, focus_, revision_};
  }

  bool controller_t::visible() const {
    return snapshot().visible;
  }

  std::optional<std::uint8_t> controller_t::owner() const {
    std::lock_guard lock {mutex_};
    return owner_;
  }

  controller_t &global_controller() {
    static controller_t controller;
    return controller;
  }
}  // namespace platf::ui
