/**
 * @file src/input/gamepad_router.cpp
 * @brief Implementation of the routable Sunshine gamepad outputs.
 */

#include "src/input/gamepad_router.h"

// standard includes
#include <utility>
#include <vector>

namespace input::gamepad {
  namespace {
    /**
     * @brief Return selected sinks in their deterministic forward output order.
     *
     * @param mode Selected router mode.
     * @param virtual_sink Host virtual-HID sink.
     * @param nxbt_sink NXBT Bridge sink.
     * @param xbox_remote_sink Xbox Remote Play sink.
     * @return Selected non-null sinks.
     */
    std::vector<std::shared_ptr<sink_t>> selected_sinks(
      output_mode_e mode,
      const std::shared_ptr<sink_t> &virtual_sink,
      const std::shared_ptr<sink_t> &nxbt_sink,
      const std::shared_ptr<sink_t> &xbox_remote_sink
    ) {
      std::vector<std::shared_ptr<sink_t>> sinks;
      if ((mode == output_mode_e::virtual_output || mode == output_mode_e::both || mode == output_mode_e::virtual_and_xbox_remote || mode == output_mode_e::all) && virtual_sink) {
        sinks.push_back(virtual_sink);
      }
      if ((mode == output_mode_e::nxbt || mode == output_mode_e::both || mode == output_mode_e::nxbt_and_xbox_remote || mode == output_mode_e::all) && nxbt_sink) {
        sinks.push_back(nxbt_sink);
      }
      if ((mode == output_mode_e::xbox_remote || mode == output_mode_e::virtual_and_xbox_remote || mode == output_mode_e::nxbt_and_xbox_remote || mode == output_mode_e::all) && xbox_remote_sink) {
        sinks.push_back(xbox_remote_sink);
      }
      return sinks;
    }

    /**
     * @brief Check that every sink required by a mode is available.
     *
     * @param mode Selected router mode.
     * @param virtual_sink Host virtual-HID sink.
     * @param nxbt_sink NXBT Bridge sink.
     * @param xbox_remote_sink Xbox Remote Play sink.
     * @return @c true when the mode has all of its required sinks.
     */
    bool fully_configured(
      output_mode_e mode,
      const std::shared_ptr<sink_t> &virtual_sink,
      const std::shared_ptr<sink_t> &nxbt_sink,
      const std::shared_ptr<sink_t> &xbox_remote_sink
    ) {
      return mode == output_mode_e::disabled ||
             (mode == output_mode_e::virtual_output && virtual_sink) ||
             (mode == output_mode_e::nxbt && nxbt_sink) ||
             (mode == output_mode_e::both && virtual_sink && nxbt_sink) ||
             (mode == output_mode_e::xbox_remote && xbox_remote_sink) ||
             (mode == output_mode_e::virtual_and_xbox_remote && virtual_sink && xbox_remote_sink) ||
             (mode == output_mode_e::nxbt_and_xbox_remote && nxbt_sink && xbox_remote_sink) ||
             (mode == output_mode_e::all && virtual_sink && nxbt_sink && xbox_remote_sink);
    }
  }  // namespace

  failure_log_limiter_t::failure_log_limiter_t(std::chrono::steady_clock::duration interval):
      interval_(interval) {
  }

  bool failure_log_limiter_t::should_log(std::chrono::steady_clock::time_point now) {
    if (last_log_ && now - *last_log_ < interval_) {
      return false;
    }
    last_log_ = now;
    return true;
  }

  router_t::router_t(
    output_mode_e mode,
    std::shared_ptr<sink_t> virtual_sink,
    std::shared_ptr<sink_t> nxbt_sink,
    std::shared_ptr<sink_t> xbox_remote_sink
  ):
      mode_(mode),
      virtual_sink_(std::move(virtual_sink)),
      nxbt_sink_(std::move(nxbt_sink)),
      xbox_remote_sink_(std::move(xbox_remote_sink)) {
  }

  output_mode_e router_t::mode() const {
    return mode_;
  }

  bool router_t::alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue) {
    if (!valid(id) || !fully_configured(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_)) {
      return false;
    }
    const auto sinks = selected_sinks(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_);
    std::vector<std::shared_ptr<sink_t>> allocated;
    for (const auto &sink : sinks) {
      if (sink->alloc(id, arrival, feedback_queue)) {
        allocated.push_back(sink);
        continue;
      }
      for (auto iter = allocated.rbegin(); iter != allocated.rend(); ++iter) {
        (*iter)->free(id);
      }
      return false;
    }
    return true;
  }

  bool router_t::rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue) {
    if (!valid(id) || !fully_configured(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_)) {
      return false;
    }
    const auto sinks = selected_sinks(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_);
    bool rebound = true;
    for (const auto &sink : sinks) {
      rebound = sink->rebind(id, feedback_queue) && rebound;
    }
    return rebound;
  }

  bool router_t::update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state) {
    if (!valid(id) || !fully_configured(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_)) {
      return false;
    }
    const auto sinks = selected_sinks(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_);
    bool updated = true;
    for (const auto &sink : sinks) {
      updated = sink->update(id, state) && updated;
    }
    return updated;
  }

  void router_t::neutralize(const platf::gamepad_id_t &id) {
    if (!valid(id)) {
      return;
    }
    for (const auto &sink : selected_sinks(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_)) {
      sink->neutralize(id);
    }
  }

  void router_t::free(const platf::gamepad_id_t &id) {
    if (!valid(id)) {
      return;
    }
    const auto sinks = selected_sinks(mode_, virtual_sink_, nxbt_sink_, xbox_remote_sink_);
    for (auto iter = sinks.rbegin(); iter != sinks.rend(); ++iter) {
      (*iter)->free(id);
    }
  }

  bool router_t::valid(const platf::gamepad_id_t &id) {
    return id.globalIndex >= 0 && id.globalIndex < platf::MAX_GAMEPADS;
  }
}  // namespace input::gamepad
