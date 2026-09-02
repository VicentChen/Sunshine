/**
 * @file src/platform/linux/ui_controller.cpp
 * @brief Controller chord, ownership, interception, and focus policy.
 */

// local includes
#include "ui_controller.h"

// standard includes
#include <utility>

namespace platf::ui {
  namespace {
    constexpr std::uint32_t navigation_up = 1U << 0U;
    constexpr std::uint32_t navigation_down = 1U << 1U;
    constexpr std::uint32_t navigation_left = 1U << 2U;
    constexpr std::uint32_t navigation_right = 1U << 3U;
    constexpr std::uint32_t navigation_confirm = 1U << 4U;
    constexpr std::uint32_t navigation_back = 1U << 5U;
  }  // namespace

  bool connection_status_t::complete() const noexcept {
    return video_ready && gamepad_ready;
  }

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
    static_cast<void>(now);
    std::lock_guard lock {mutex_};
    decision_t result {.visible = visible_};
    if (controller >= inputs_.size()) {
      result.consume = visible_;
      return result;
    }

    auto &state = inputs_[controller];
    const auto chord_buttons = input.buttons & start_back_chord_;
    const auto other_buttons = input.buttons & ~start_back_chord_;
    const bool chord_down = chord_buttons == start_back_chord_;

    if (release_controller_ && *release_controller_ == controller) {
      result.consume = true;
      if ((input.buttons & release_chord_) == 0) {
        release_controller_.reset();
        release_chord_ = 0;
        state = {};
        if (!visible_) {
          owner_.reset();
        }
      }
      return result;
    }

    if (!visible_) {
      if (state.start_passthrough) {
        if ((input.buttons & platf::START) == 0) {
          state.start_passthrough = false;
        }
        state.previous_navigation = navigation_mask(input, state);
        return result;
      }

      if (state.start_deferred) {
        if (chord_down) {
          visible_ = true;
          owner_ = controller;
          release_controller_ = controller;
          release_chord_ = start_back_chord_;
          page_ = page_e::main_menu;
          ++revision_;
          result.consume = true;
          result.neutralize = true;
          result.visibility_changed = true;
          result.visible = true;
          return result;
        }
        if ((input.buttons & platf::START) == 0) {
          state.start_deferred = false;
          result.consume = true;
          result.replay_start_tap = true;
          return result;
        }
        if (other_buttons != 0) {
          state.start_deferred = false;
          state.start_passthrough = true;
          state.previous_navigation = navigation_mask(input, state);
          return result;
        }
        result.consume = true;
        return result;
      }

      if (chord_buttons == platf::START && other_buttons == 0) {
        state.start_deferred = true;
        result.consume = true;
        result.neutralize = true;
        return result;
      }

      if (chord_down) {
        visible_ = true;
        owner_ = controller;
        release_controller_ = controller;
        release_chord_ = start_back_chord_;
        page_ = page_e::main_menu;
        ++revision_;
        result.consume = true;
        result.neutralize = true;
        result.visibility_changed = true;
        result.visible = true;
        return result;
      }

      state.previous_navigation = navigation_mask(input, state);
      return result;
    }

    result.consume = true;
    if (!owner_ || *owner_ != controller) {
      state.start_deferred = false;
      state.previous_navigation = navigation_mask(input, state);
      return result;
    }

    if (chord_down) {
      visible_ = false;
      page_ = page_e::main_menu;
      release_controller_ = controller;
      release_chord_ = start_back_chord_;
      ++revision_;
      result.neutralize = true;
      result.visibility_changed = true;
      result.visible = false;
      return result;
    }

    const auto current_navigation = navigation_mask(input, state);
    const auto pressed = current_navigation & ~state.previous_navigation;
    state.previous_navigation = current_navigation;
    if ((pressed & navigation_up) != 0) {
      result.navigation = navigation_e::up;
      if (page_ == page_e::main_menu) {
        focus_ = static_cast<std::uint8_t>((focus_ + item_count_ - 1U) % item_count_);
        ++revision_;
      } else if (page_ == page_e::ui_size) {
        ui_size_focus_ = static_cast<std::uint8_t>((ui_size_focus_ + ui_size_item_count_ - 1U) % ui_size_item_count_);
        ++revision_;
      } else if (page_ == page_e::profile && profile_scroll_steps_ != 0) {
        --profile_scroll_steps_;
        ++revision_;
      }
    } else if ((pressed & navigation_down) != 0) {
      result.navigation = navigation_e::down;
      if (page_ == page_e::main_menu) {
        focus_ = static_cast<std::uint8_t>((focus_ + 1U) % item_count_);
        ++revision_;
      } else if (page_ == page_e::ui_size) {
        ui_size_focus_ = static_cast<std::uint8_t>((ui_size_focus_ + 1U) % ui_size_item_count_);
        ++revision_;
      } else if (page_ == page_e::profile && profile_scroll_steps_ < profile_scroll_step_limit) {
        ++profile_scroll_steps_;
        ++revision_;
      }
    } else if ((pressed & navigation_left) != 0) {
      result.navigation = navigation_e::left;
    } else if ((pressed & navigation_right) != 0) {
      result.navigation = navigation_e::right;
    } else if ((pressed & navigation_confirm) != 0) {
      result.navigation = navigation_e::confirm;
      if (page_ == page_e::main_menu) {
        if (focus_ == 0) {
          page_ = page_e::connection_status;
          ++revision_;
        } else if (focus_ == 1) {
          page_ = page_e::profile;
          profile_scroll_steps_ = 0;
          ++revision_;
        } else if (focus_ == 2) {
          page_ = page_e::ui_size;
          ui_size_focus_ = static_cast<std::uint8_t>(ui_size_);
          ++revision_;
        } else {
          visible_ = false;
          owner_.reset();
          page_ = page_e::main_menu;
          ++revision_;
          result.neutralize = true;
          result.visibility_changed = true;
          result.visible = false;
          result.action = action_e::close_modal;
        }
      } else if (page_ == page_e::ui_size) {
        const auto selected = static_cast<ui_size_e>(ui_size_focus_);
        if (ui_size_ != selected) {
          ui_size_ = selected;
          ++revision_;
        }
      }
    } else if ((pressed & navigation_back) != 0) {
      result.navigation = navigation_e::back;
      if (page_ != page_e::main_menu) {
        page_ = page_e::main_menu;
        ++revision_;
      } else {
        visible_ = false;
        owner_.reset();
        ++revision_;
        result.neutralize = true;
        result.visibility_changed = true;
        result.visible = false;
        result.action = action_e::close_modal;
      }
    }
    return result;
  }

  decision_t controller_t::tick(clock_t::time_point now) {
    static_cast<void>(now);
    std::lock_guard lock {mutex_};
    return {.visible = visible_};
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
      release_chord_ = 0;
    }
    if (!owner_ || *owner_ != controller) {
      return result;
    }

    owner_.reset();
    const bool changed = visible_;
    visible_ = false;
    page_ = page_e::main_menu;
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
    release_chord_ = 0;
    page_ = page_e::main_menu;
    profile_scroll_steps_ = 0;
    if (visible_) {
      visible_ = false;
      ++revision_;
    }
  }

  snapshot_t controller_t::snapshot() const {
    std::lock_guard lock {mutex_};
    const bool automatic = connection_initialized_ && !connection_settled_;
    const auto page = visible_ || !automatic ? page_ : page_e::connection_status;
    return {
      .visible = visible_ || automatic,
      .modal = visible_,
      .page = page,
      .focus = focus_,
      .connection = connection_,
      .profile = profile_,
      .ui_size = ui_size_,
      .ui_size_focus = ui_size_focus_,
      .profile_scroll_steps = profile_scroll_steps_,
      .revision = revision_
    };
  }

  void controller_t::update_connection(connection_status_t status, const clock_t::time_point now) {
    std::lock_guard lock {mutex_};
    bool state_changed = !connection_initialized_ || connection_ != status;
    const bool was_settled = connection_settled_;

    if (!status.complete()) {
      connection_ready_since_.reset();
      connection_settled_ = false;
    } else if (!connection_initialized_ || !connection_.complete() || !connection_ready_since_) {
      connection_ready_since_ = now;
      connection_settled_ = false;
    } else if (!connection_settled_ && now - *connection_ready_since_ >= ready_hold_time_) {
      connection_settled_ = true;
    }

    connection_ = std::move(status);
    connection_initialized_ = true;
    state_changed = state_changed || was_settled != connection_settled_;
    if (state_changed) {
      ++revision_;
    }
  }

  void controller_t::update_profile(profile_status_t status) {
    std::lock_guard lock {mutex_};
    if (profile_ == status) {
      return;
    }
    profile_ = std::move(status);
    if (visible_ && page_ == page_e::profile) {
      ++revision_;
    }
  }

  bool controller_t::visible() const {
    std::lock_guard lock {mutex_};
    return visible_;
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
