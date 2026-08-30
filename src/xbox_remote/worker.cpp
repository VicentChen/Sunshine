/**
 * @file src/xbox_remote/worker.cpp
 * @brief Background lifecycle and bounded input dispatch for Xbox Remote Play.
 */

#include "src/xbox_remote/worker.h"

// standard includes
#include <algorithm>
#include <utility>

// local includes
#include "src/logging.h"

namespace xbox_remote::worker {
  std::string_view state_name(state_e state) {
    switch (state) {
      case state_e::idle:
        return "idle";
      case state_e::starting:
        return "starting";
      case state_e::ready:
        return "ready";
      case state_e::stopping:
        return "stopping";
      case state_e::failed:
        return "failed";
    }
    return "unknown";
  }

  std::string_view failure_kind_name(failure_kind_e kind) {
    switch (kind) {
      case failure_kind_e::retryable:
        return "retryable";
      case failure_kind_e::reauthentication_required:
        return "reauthentication_required";
      case failure_kind_e::permanent:
        return "permanent";
    }
    return "unknown";
  }

  session_t::session_t(connection_factory_t factory, options_t options):
      factory_(std::move(factory)),
      options_(std::move(options)),
      queue_(options_.queue) {
  }

  session_t::~session_t() {
    stop();
  }

  bool session_t::start() {
    std::lock_guard lock(mutex_);
    if (state_ == state_e::starting || state_ == state_e::ready || state_ == state_e::stopping || thread_.joinable() || !factory_) {
      return false;
    }
    stop_requested_ = false;
    failure_stage_.clear();
    failure_kind_.clear();
    stage_ = "connection";
    state_ = state_e::starting;
    thread_ = std::thread([this]() {
      run();
    });
    return true;
  }

  void session_t::stop() {
    {
      std::lock_guard lock(mutex_);
      if (!thread_.joinable()) {
        state_ = state_e::idle;
        stop_requested_ = false;
        return;
      }
      stop_requested_ = true;
      state_ = state_e::stopping;
    }
    changed_.notify_all();
    thread_.join();
    std::lock_guard lock(mutex_);
    state_ = state_e::idle;
    stop_requested_ = false;
    failure_stage_.clear();
    stage_.clear();
    failure_kind_.clear();
    watchdog_armed_ = false;
  }

  state_e session_t::state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }

  std::string session_t::failure_stage() const {
    std::lock_guard lock(mutex_);
    return failure_stage_;
  }

  std::string session_t::stage() const {
    std::lock_guard lock(mutex_);
    return stage_;
  }

  std::string session_t::failure_kind() const {
    std::lock_guard lock(mutex_);
    return failure_kind_;
  }

  std::uint64_t session_t::epoch() const {
    std::lock_guard lock(mutex_);
    return epoch_;
  }

  void session_t::set_vibration_handler(vibration_handler_t handler) {
    std::lock_guard lock(mutex_);
    vibration_handler_ = std::move(handler);
  }

  bool session_t::attach(const protocol::gamepad_frame_t &frame) {
    std::lock_guard lock(mutex_);
    if (state_ == state_e::failed) {
      return false;
    }
    const auto accepted = queue_.attach(frame, input::outbound_queue_t::clock_t::now());
    last_input_ = input::outbound_queue_t::clock_t::now();
    watchdog_armed_ = input::activity_mask(frame) != 0 || frame.physical_physicality != 0 || frame.virtual_physicality != 0;
    changed_.notify_one();
    return accepted;
  }

  bool session_t::rebind() {
    std::lock_guard lock(mutex_);
    if (state_ == state_e::failed) {
      return false;
    }
    queue_.on_reconnect();
    changed_.notify_one();
    return true;
  }

  bool session_t::submit(const protocol::gamepad_frame_t &frame) {
    std::lock_guard lock(mutex_);
    if (state_ == state_e::failed) {
      return false;
    }
    queue_.submit(frame, input::outbound_queue_t::clock_t::now());
    last_input_ = input::outbound_queue_t::clock_t::now();
    watchdog_armed_ = input::activity_mask(frame) != 0 || frame.physical_physicality != 0 || frame.virtual_physicality != 0;
    changed_.notify_one();
    return true;
  }

  bool session_t::neutralize() {
    std::lock_guard lock(mutex_);
    const auto accepted = queue_.neutralize(input::outbound_queue_t::clock_t::now());
    watchdog_armed_ = false;
    changed_.notify_one();
    return accepted;
  }

  void session_t::detach() {
    std::lock_guard lock(mutex_);
    queue_.detach(input::outbound_queue_t::clock_t::now());
    watchdog_armed_ = false;
    changed_.notify_one();
  }

  void session_t::run() {
    const auto cancel = [this]() {
      return cancelled();
    };
    std::size_t reconnect_attempt = 0;
    auto backoff = options_.reconnect_initial_backoff;
    while (!cancel()) {
      std::uint64_t epoch;
      {
        std::lock_guard lock(mutex_);
        epoch = ++epoch_;
      }
      auto connection = factory_();
      if (!connection) {
        std::lock_guard lock(mutex_);
        state_ = state_e::failed;
        failure_stage_ = "connection_factory";
        stage_ = failure_stage_;
        failure_kind_ = std::string {failure_kind_name(failure_kind_e::permanent)};
        BOOST_LOG(error) << "Xbox Remote Play state=failed stage=" << failure_stage_ << " kind=" << failure_kind_;
        return;
      }
      connection->set_progress_handler([this, epoch](std::string_view stage) {
        std::string changed_stage;
        {
          std::lock_guard lock(mutex_);
          if (stop_requested_ || epoch_ != epoch || stage.empty() || stage_ == stage) {
            return;
          }
          stage_ = stage;
          changed_stage = stage_;
        }
        BOOST_LOG(info) << "Xbox Remote Play state=starting stage=" << changed_stage;
      });

      auto result = connection->open(cancel);
      if (result.ok) {
        {
          std::lock_guard lock(mutex_);
          if (!stop_requested_) {
            queue_.on_reconnect();
            state_ = state_e::ready;
            stage_ = "ready";
            failure_stage_.clear();
            failure_kind_.clear();
            BOOST_LOG(info) << "Xbox Remote Play state=ready stage=ready epoch=" << epoch;
          }
        }

        while (!cancel()) {
          std::optional<input::item_t> item;
          {
            std::unique_lock lock(mutex_);
            const auto now = input::outbound_queue_t::clock_t::now();
            if (watchdog_armed_ && options_.watchdog_timeout > std::chrono::milliseconds::zero() && now - last_input_ >= options_.watchdog_timeout) {
              queue_.neutralize(now);
              watchdog_armed_ = false;
              BOOST_LOG(warning) << "Xbox Remote Play input watchdog neutralized stale state epoch=" << epoch;
            }
            item = queue_.take(now);
            if (!item) {
              changed_.wait_for(lock, options_.poll_interval, [this]() {
                return stop_requested_;
              });
            }
          }
          if (item && !connection->send(*item)) {
            result = {false, "input_send", failure_kind_e::retryable};
            break;
          }

          std::optional<protocol::vibration_t> vibration;
          result = connection->poll(cancel, vibration);
          if (!result.ok) {
            if (result.stage.empty()) {
              result.stage = "connection_poll";
            }
            break;
          }
          vibration_handler_t handler;
          if (vibration) {
            std::lock_guard lock(mutex_);
            if (epoch_ == epoch) {
              handler = vibration_handler_;
            }
          }
          if (handler && vibration) {
            handler(*vibration);
          }
        }
      }

      connection->send({input::item_kind_e::neutralize, {}});
      connection->send({input::item_kind_e::detach, {}});
      const auto cleanup = connection->close();
      if (!cleanup.ok) {
        BOOST_LOG(warning) << "Xbox Remote Play cleanup stage=" << cleanup.stage << " kind=" << failure_kind_name(cleanup.failure_kind);
      }
      if (cancel()) {
        return;
      }
      if (result.ok) {
        result = {false, "connection_closed", failure_kind_e::retryable};
      }

      const bool retry = result.failure_kind == failure_kind_e::retryable && reconnect_attempt < options_.maximum_reconnect_attempts;
      if (!retry) {
        std::lock_guard lock(mutex_);
        queue_.neutralize(input::outbound_queue_t::clock_t::now());
        watchdog_armed_ = false;
        state_ = state_e::failed;
        failure_stage_ = result.stage.empty() ? "connection_failure" : result.stage;
        stage_ = failure_stage_;
        failure_kind_ = std::string {failure_kind_name(result.failure_kind)};
        BOOST_LOG(error) << "Xbox Remote Play state=failed stage=" << failure_stage_ << " kind=" << failure_kind_;
        return;
      }

      ++reconnect_attempt;
      {
        std::unique_lock lock(mutex_);
        queue_.neutralize(input::outbound_queue_t::clock_t::now());
        watchdog_armed_ = false;
        state_ = state_e::starting;
        stage_ = "reconnect_backoff";
        failure_stage_.clear();
        failure_kind_.clear();
        BOOST_LOG(warning) << "Xbox Remote Play state=starting stage=reconnect_backoff attempt=" << reconnect_attempt;
        changed_.wait_for(lock, backoff, [this]() {
          return stop_requested_;
        });
      }
      if (backoff < options_.reconnect_max_backoff) {
        backoff = std::min(options_.reconnect_max_backoff, backoff * 2);
      }
    }
  }

  bool session_t::cancelled() const {
    std::lock_guard lock(mutex_);
    return stop_requested_;
  }
}  // namespace xbox_remote::worker
