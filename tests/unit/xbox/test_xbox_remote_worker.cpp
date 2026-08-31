/**
 * @file tests/unit/xbox/test_xbox_remote_worker.cpp
 * @brief Tests for the cancellable Xbox Remote Play background worker.
 */

#include "tests/tests_common.h"
#include "src/xbox_remote/worker.h"

#ifdef SUNSHINE_XBOX_REMOTE_PLAY
  #include "src/xbox_remote/auth.h"
  #include "src/xbox_remote/production_connection.h"
  #include "src/xbox_remote/session.h"
  #include "src/xbox_remote/startup.h"
  #include "src/xbox_remote/transport.h"
#endif

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  namespace worker = xbox_remote::worker;

  /**
   * @brief State retained after a fake connection is destroyed.
   */
  struct fake_state_t {
    std::mutex mutex;  ///< Protects observations from the worker thread.
    std::vector<xbox_remote::input::item_t> sent;  ///< Sent controller operations.
    worker::result_t open_result;  ///< Configured connection result.
    worker::result_t poll_result;  ///< Configured poll result.
    std::optional<xbox_remote::protocol::vibration_t> vibration;  ///< One feedback command.
    std::vector<worker::connection_t::progress_handler_t> progress_handlers;  ///< Per-epoch progress callbacks.
    bool send_result = true;  ///< Configured send result.
    worker::result_t close_result;  ///< Configured cleanup result.
    bool opened = false;  ///< Whether open was called.
    bool block_open_until_cancelled = false;  ///< Whether open waits for worker cancellation.
    int created = 0;  ///< Number of constructed connection epochs.
    int closed = 0;  ///< Number of closed connection epochs.
    int poll_failures_remaining = 0;  ///< Retryable poll failures injected before success.
  };

  /**
   * @brief Injectable connection with thread-safe observations.
   */
  class fake_connection_t final: public worker::connection_t {
  public:
    /**
     * @brief Bind to retained fake state.
     *
     * @param state Shared observations and configured results.
     */
    explicit fake_connection_t(std::shared_ptr<fake_state_t> state):
        state_(std::move(state)) {
    }

    /**
     * @copydoc worker::connection_t::open
     */
    worker::result_t open(const std::function<bool()> &cancelled) override {
      if (cancelled()) {
        return {false, "cancelled"};
      }
      worker::result_t result;
      bool block;
      {
        std::lock_guard lock(state_->mutex);
        state_->opened = true;
        result = state_->open_result;
        block = state_->block_open_until_cancelled;
      }
      while (block && !cancelled()) {
        std::this_thread::sleep_for(1ms);
      }
      return block ? worker::result_t {false, "cancelled"} : result;
    }

    /**
     * @copydoc worker::connection_t::set_progress_handler
     */
    void set_progress_handler(progress_handler_t handler) override {
      std::lock_guard lock(state_->mutex);
      state_->progress_handlers.push_back(std::move(handler));
    }

    /**
     * @copydoc worker::connection_t::send
     */
    bool send(const xbox_remote::input::item_t &item) override {
      std::lock_guard lock(state_->mutex);
      state_->sent.push_back(item);
      return state_->send_result;
    }

    /**
     * @copydoc worker::connection_t::poll
     */
    worker::result_t poll(const std::function<bool()> &cancelled, std::optional<xbox_remote::protocol::vibration_t> &vibration) override {
      if (cancelled()) {
        return {};
      }
      std::lock_guard lock(state_->mutex);
      if (state_->poll_failures_remaining > 0) {
        --state_->poll_failures_remaining;
        return {false, "keepalive", worker::failure_kind_e::retryable};
      }
      vibration = std::exchange(state_->vibration, std::nullopt);
      return state_->poll_result;
    }

    /**
     * @copydoc worker::connection_t::close
     */
    worker::result_t close() override {
      std::lock_guard lock(state_->mutex);
      ++state_->closed;
      return state_->close_result;
    }

  private:
    std::shared_ptr<fake_state_t> state_;  ///< Retained fake observations.
  };

  /**
   * @brief Wait briefly for an asynchronous condition.
   *
   * @param condition Condition sampled without long sleeps.
   * @return @c true when the condition became true.
   */
  bool wait_until(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (condition()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return condition();
  }

  /**
   * @brief Create a fresh fake connection factory.
   *
   * @param state Shared fake state.
   * @return Worker connection factory.
   */
  worker::connection_factory_t factory_for(const std::shared_ptr<fake_state_t> &state) {
    return [state]() {
      {
        std::lock_guard lock(state->mutex);
        ++state->created;
      }
      return std::make_unique<fake_connection_t>(state);
    };
  }

  /**
   * @brief Return worker options that make terminal fault tests immediate.
   *
   * @return Options with no reconnect attempts.
   */
  worker::options_t no_retry_options() {
    worker::options_t options;
    options.poll_interval = 1ms;
    options.maximum_reconnect_attempts = 0;
    return options;
  }
}  // namespace

TEST(XboxRemoteWorkerTest, NamesEveryPublicState) {
  EXPECT_EQ(worker::state_name(worker::state_e::idle), "idle");
  EXPECT_EQ(worker::state_name(worker::state_e::starting), "starting");
  EXPECT_EQ(worker::state_name(worker::state_e::ready), "ready");
  EXPECT_EQ(worker::state_name(worker::state_e::stopping), "stopping");
  EXPECT_EQ(worker::state_name(worker::state_e::failed), "failed");
  EXPECT_EQ(worker::state_name(static_cast<worker::state_e>(99)), "unknown");
  EXPECT_EQ(worker::failure_kind_name(worker::failure_kind_e::retryable), "retryable");
  EXPECT_EQ(worker::failure_kind_name(worker::failure_kind_e::reauthentication_required), "reauthentication_required");
  EXPECT_EQ(worker::failure_kind_name(worker::failure_kind_e::permanent), "permanent");
  EXPECT_EQ(worker::failure_kind_name(static_cast<worker::failure_kind_e>(99)), "unknown");
}

#ifdef SUNSHINE_XBOX_REMOTE_PLAY
TEST(XboxRemoteWorkerTest, ClassifiesRecoveryPoliciesWithoutSensitiveDiagnostics) {
  using worker::failure_kind_e;
  namespace production = xbox_remote::production;

  EXPECT_EQ(production::classify_authentication_failure({xbox_remote::auth::auth_error_e::network_error}), failure_kind_e::retryable);
  EXPECT_EQ(production::classify_authentication_failure({xbox_remote::auth::auth_error_e::timeout}), failure_kind_e::retryable);
  EXPECT_EQ(production::classify_authentication_failure({xbox_remote::auth::auth_error_e::cancelled}), failure_kind_e::retryable);
  EXPECT_EQ(production::classify_authentication_failure({xbox_remote::auth::auth_error_e::http_error}), failure_kind_e::reauthentication_required);

  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::unauthorized}), failure_kind_e::reauthentication_required);
  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::not_found}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::duplicate_console}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::ambiguous_console}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::invalid_response}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::rate_limited}), failure_kind_e::retryable);
  EXPECT_EQ(production::classify_session_failure({xbox_remote::session::error_e::timeout}), failure_kind_e::retryable);

  EXPECT_EQ(production::classify_transport_failure({xbox_remote::transport::error_e::invalid_sdp}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_transport_failure({xbox_remote::transport::error_e::invalid_candidate}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_transport_failure({xbox_remote::transport::error_e::peer_failed}), failure_kind_e::retryable);

  EXPECT_EQ(production::classify_startup_failure({xbox_remote::startup::error_e::invalid_ack}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_startup_failure({xbox_remote::startup::error_e::invalid_state}), failure_kind_e::permanent);
  EXPECT_EQ(production::classify_startup_failure({xbox_remote::startup::error_e::channel_closed}), failure_kind_e::retryable);
}
#endif

TEST(XboxRemoteWorkerTest, StartsAsynchronouslySendsLatestStateAndCleansUp) {
  auto fake = std::make_shared<fake_state_t>();
  worker::options_t options;
  options.poll_interval = 1ms;
  worker::session_t session {factory_for(fake), options};
  std::vector<xbox_remote::protocol::vibration_t> vibration;
  session.set_vibration_handler([&vibration](const auto &value) {
    vibration.push_back(value);
  });
  xbox_remote::protocol::gamepad_frame_t initial;
  initial.button_mask = 1;
  ASSERT_TRUE(session.attach(initial));
  ASSERT_TRUE(session.start());
  EXPECT_FALSE(session.start());
  ASSERT_TRUE(wait_until([&session]() {
    return session.state() == worker::state_e::ready;
  }));
  EXPECT_EQ(session.stage(), "ready");

  xbox_remote::protocol::gamepad_frame_t latest;
  latest.button_mask = 2;
  EXPECT_TRUE(session.submit(latest));
  EXPECT_TRUE(session.rebind());
  {
    std::lock_guard lock(fake->mutex);
    fake->vibration = xbox_remote::protocol::vibration_t {0, 1, 2, 3, 4, 5, 6, 7};
  }
  ASSERT_TRUE(wait_until([&vibration]() {
    return !vibration.empty();
  }));
  EXPECT_TRUE(session.neutralize());
  session.detach();
  session.stop();
  EXPECT_EQ(session.state(), worker::state_e::idle);
  EXPECT_TRUE(session.failure_stage().empty());
  EXPECT_TRUE(session.stage().empty());
  {
    std::lock_guard lock(fake->mutex);
    EXPECT_TRUE(fake->opened);
    EXPECT_EQ(fake->closed, 1);
    ASSERT_GE(fake->sent.size(), 4);
    EXPECT_EQ(fake->sent[fake->sent.size() - 2].kind, xbox_remote::input::item_kind_e::neutralize);
    EXPECT_EQ(fake->sent.back().kind, xbox_remote::input::item_kind_e::detach);
  }
  session.stop();
}

TEST(XboxRemoteWorkerTest, SurfacesFactoryOpenSendAndPollFailures) {
  worker::session_t missing_factory {worker::connection_factory_t {}};
  EXPECT_FALSE(missing_factory.start());

  worker::session_t null_connection {[]() -> std::unique_ptr<worker::connection_t> {
    return {};
  }};
  ASSERT_TRUE(null_connection.start());
  ASSERT_TRUE(wait_until([&null_connection]() {
    return null_connection.state() == worker::state_e::failed;
  }));
  EXPECT_EQ(null_connection.failure_stage(), "connection_factory");
  EXPECT_EQ(null_connection.stage(), "connection_factory");
  null_connection.stop();

  auto open_failure = std::make_shared<fake_state_t>();
  open_failure->open_result = {false, "authentication", worker::failure_kind_e::reauthentication_required};
  worker::session_t failed_open {factory_for(open_failure), no_retry_options()};
  ASSERT_TRUE(failed_open.start());
  ASSERT_TRUE(wait_until([&failed_open]() {
    return failed_open.state() == worker::state_e::failed;
  }));
  EXPECT_EQ(failed_open.failure_stage(), "authentication");
  EXPECT_EQ(failed_open.stage(), "authentication");
  EXPECT_EQ(failed_open.failure_kind(), "reauthentication_required");
  failed_open.stop();

  auto send_failure = std::make_shared<fake_state_t>();
  send_failure->send_result = false;
  worker::session_t failed_send {factory_for(send_failure), no_retry_options()};
  ASSERT_TRUE(failed_send.attach({}));
  ASSERT_TRUE(failed_send.start());
  ASSERT_TRUE(wait_until([&failed_send]() {
    return failed_send.state() == worker::state_e::failed;
  }));
  EXPECT_EQ(failed_send.failure_stage(), "input_send");
  EXPECT_EQ(failed_send.stage(), "input_send");
  EXPECT_EQ(failed_send.failure_kind(), "retryable");
  EXPECT_FALSE(failed_send.attach({}));
  EXPECT_FALSE(failed_send.submit({}));
  EXPECT_FALSE(failed_send.rebind());
  failed_send.stop();

  auto poll_failure = std::make_shared<fake_state_t>();
  poll_failure->poll_result = {false, "keepalive"};
  worker::session_t failed_poll {factory_for(poll_failure), no_retry_options()};
  ASSERT_TRUE(failed_poll.start());
  ASSERT_TRUE(wait_until([&failed_poll]() {
    return failed_poll.state() == worker::state_e::failed;
  }));
  EXPECT_EQ(failed_poll.failure_stage(), "keepalive");
  EXPECT_EQ(failed_poll.stage(), "keepalive");
  EXPECT_EQ(failed_poll.failure_kind(), "retryable");
  failed_poll.stop();
}

TEST(XboxRemoteWorkerTest, ReconnectsWithNewEpochAndIgnoresOldProgressCallbacks) {
  auto fake = std::make_shared<fake_state_t>();
  fake->poll_failures_remaining = 1;
  fake->close_result = {false, "delete_unconfirmed", worker::failure_kind_e::retryable};
  worker::options_t options;
  options.poll_interval = 1ms;
  options.reconnect_initial_backoff = 1ms;
  options.reconnect_max_backoff = 2ms;
  options.maximum_reconnect_attempts = 2;
  worker::session_t session {factory_for(fake), options};
  ASSERT_TRUE(session.submit({}));
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(wait_until([&session, &fake]() {
    int created;
    {
      std::lock_guard lock(fake->mutex);
      created = fake->created;
    }
    return created == 2 && session.state() == worker::state_e::ready;
  }));
  EXPECT_EQ(session.epoch(), 2);
  worker::connection_t::progress_handler_t old_progress;
  {
    std::lock_guard lock(fake->mutex);
    ASSERT_EQ(fake->progress_handlers.size(), 2);
    old_progress = fake->progress_handlers.front();
  }
  old_progress("stale_epoch");
  EXPECT_EQ(session.stage(), "ready");
  session.stop();
  std::lock_guard lock(fake->mutex);
  EXPECT_EQ(fake->closed, 2);
}

TEST(XboxRemoteWorkerTest, WatchdogNeutralizesAStalePressedStateBeforeStop) {
  auto fake = std::make_shared<fake_state_t>();
  worker::options_t options;
  options.poll_interval = 1ms;
  options.watchdog_timeout = 10ms;
  worker::session_t session {factory_for(fake), options};
  xbox_remote::protocol::gamepad_frame_t pressed;
  pressed.button_mask = static_cast<std::uint16_t>(xbox_remote::protocol::gamepad_button_e::a);
  ASSERT_TRUE(session.submit(pressed));
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(wait_until([&session]() {
    return session.state() == worker::state_e::ready;
  }));
  ASSERT_TRUE(wait_until([&fake]() {
    std::lock_guard lock(fake->mutex);
    return std::ranges::any_of(fake->sent, [](const auto &item) {
      return item.kind == xbox_remote::input::item_kind_e::neutralize;
    });
  }));
  EXPECT_EQ(session.state(), worker::state_e::ready);
  session.stop();
}

TEST(XboxRemoteWorkerTest, ExhaustsBoundedRetryBudgetAndStops) {
  auto fake = std::make_shared<fake_state_t>();
  fake->poll_result = {false, "keepalive", worker::failure_kind_e::retryable};
  worker::options_t options;
  options.poll_interval = 1ms;
  options.reconnect_initial_backoff = 1ms;
  options.reconnect_max_backoff = 2ms;
  options.maximum_reconnect_attempts = 2;
  worker::session_t session {factory_for(fake), options};
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(wait_until([&session]() {
    return session.state() == worker::state_e::failed;
  }));
  EXPECT_EQ(session.failure_stage(), "keepalive");
  EXPECT_EQ(session.failure_kind(), "retryable");
  EXPECT_EQ(session.epoch(), 3);
  {
    std::lock_guard lock(fake->mutex);
    EXPECT_EQ(fake->created, 3);
    EXPECT_EQ(fake->closed, 3);
  }
  session.stop();
}

TEST(XboxRemoteWorkerTest, PermanentFailureDoesNotConsumeRetryBudget) {
  auto fake = std::make_shared<fake_state_t>();
  fake->open_result = {false, "configuration", worker::failure_kind_e::permanent};
  worker::options_t options;
  options.poll_interval = 1ms;
  options.reconnect_initial_backoff = 1ms;
  options.maximum_reconnect_attempts = 3;
  worker::session_t session {factory_for(fake), options};
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(wait_until([&session]() {
    return session.state() == worker::state_e::failed;
  }));
  EXPECT_EQ(session.failure_kind(), "permanent");
  std::lock_guard lock(fake->mutex);
  EXPECT_EQ(fake->created, 1);
  EXPECT_EQ(fake->closed, 1);
}

TEST(XboxRemoteWorkerTest, StopCancelsAnInFlightOpenPromptly) {
  auto fake = std::make_shared<fake_state_t>();
  fake->block_open_until_cancelled = true;
  worker::session_t session {factory_for(fake)};
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(wait_until([&fake]() {
    std::lock_guard lock(fake->mutex);
    return fake->opened;
  }));
  const auto started = std::chrono::steady_clock::now();
  session.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
  EXPECT_EQ(session.state(), worker::state_e::idle);
  std::lock_guard lock(fake->mutex);
  EXPECT_EQ(fake->closed, 1);
}
