/**
 * @file tests/unit/xbox/test_xbox_remote_input_lifecycle.cpp
 * @brief Tests for Xbox worker retention across Moonlight stream resume.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "src/config.h"
#include "src/globals.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"
#include "tests/tests_common.h"

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief One Xbox operation tagged with the fake connection that received it.
   */
  struct sent_item_t {
    int connection_id = 0;  ///< Monotonic fake connection identifier.
    xbox_remote::input::item_t item;  ///< Worker operation delivered to the connection.
  };

  /**
   * @brief Observations shared by every fake Xbox connection generation.
   */
  struct connection_state_t {
    std::atomic<int> created {0};  ///< Number of fake Remote Play connections created.
    std::atomic<int> closed {0};  ///< Number of fake Remote Play connections closed.
    std::mutex mutex;  ///< Protects sent item history.
    std::vector<sent_item_t> sent;  ///< Operations grouped by receiving connection.
  };

  /**
   * @brief Successful cancellable Xbox connection with generation-tagged input observations.
   */
  class connection_t final: public xbox_remote::worker::connection_t {
  public:
    /**
     * @brief Bind a fake connection to shared observations.
     *
     * @param state Shared connection observations.
     * @param connection_id Monotonic connection identifier.
     */
    connection_t(std::shared_ptr<connection_state_t> state, int connection_id):
        state_(std::move(state)),
        connection_id_(connection_id) {
    }

    /**
     * @copydoc xbox_remote::worker::connection_t::open
     */
    xbox_remote::worker::result_t open(const std::function<bool()> &cancelled) override {
      return cancelled() ? xbox_remote::worker::result_t {false, "cancelled"} : xbox_remote::worker::result_t {};
    }

    /**
     * @copydoc xbox_remote::worker::connection_t::send
     */
    bool send(const xbox_remote::input::item_t &item) override {
      std::lock_guard lock {state_->mutex};
      state_->sent.push_back({connection_id_, item});
      return true;
    }

    /**
     * @copydoc xbox_remote::worker::connection_t::poll
     */
    xbox_remote::worker::result_t poll(const std::function<bool()> &cancelled, std::optional<xbox_remote::protocol::vibration_t> &vibration) override {
      static_cast<void>(cancelled);
      static_cast<void>(vibration);
      std::this_thread::sleep_for(1ms);
      return {};
    }

    /**
     * @copydoc xbox_remote::worker::connection_t::close
     */
    xbox_remote::worker::result_t close() override {
      ++state_->closed;
      return {};
    }

  private:
    std::shared_ptr<connection_state_t> state_;  ///< Shared lifecycle and input observations.
    int connection_id_;  ///< Generation represented by this connection.
  };

  /**
   * @brief Wait for an asynchronous lifecycle or input condition.
   *
   * @param condition Condition sampled until the deadline.
   * @return True when the condition became true within one second.
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
   * @brief Test fixture owning isolated input, worker, and timer state.
   */
  class XboxRemoteInputLifecycleTest: public ::testing::Test {
  protected:
    /**
     * @brief Install a fake platform input backend and Xbox connection factory.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.controller = true;
      config::input.xbox_remote_enabled = true;
      config::input.xbox_remote_app = "Xbox";
      config::input.xbox_remote_idle_timeout = 80ms;

      if (!task_pool.running()) {
        task_pool.start(1);
        owns_task_pool_ = true;
      }

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      input::testing::set_platform_input(std::move(platform_input));

      state_ = std::make_shared<connection_state_t>();
      input::testing::set_xbox_remote_connection_factory([state = state_]() {
        const auto connection_id = ++state->created;
        return std::make_unique<connection_t>(state, connection_id);
      });
    }

    /**
     * @brief Stop workers, release retained inputs, and restore global state.
     */
    void TearDown() override {
      input::select_gamepad_output({});
      input::terminate_gamepads();
      flush_input_tasks();
      input::testing::set_xbox_remote_connection_factory({});
      input::testing::set_platform_input({});
      config::input = std::move(original_input_);
      if (owns_task_pool_) {
        task_pool.stop();
        task_pool.join();
      }
    }

    /**
     * @brief Wait until all previously queued input tasks complete.
     */
    void flush_input_tasks() {
      if (task_pool.running()) {
        task_pool.push([]() {
                 })
          .wait();
      }
    }

    /**
     * @brief Check whether one connection received a mapped button state.
     *
     * @param connection_id Fake connection generation.
     * @param button Expected Xbox button mask.
     * @return True when a matching state operation was observed.
     */
    bool received_button(int connection_id, xbox_remote::protocol::gamepad_button_e button) const {
      std::lock_guard lock {state_->mutex};
      return std::ranges::any_of(state_->sent, [connection_id, button](const auto &sent) {
        return sent.connection_id == connection_id && sent.item.kind == xbox_remote::input::item_kind_e::state &&
               sent.item.frame.button_mask == static_cast<std::uint16_t>(button);
      });
    }

    /**
     * @brief Check whether one connection received logical gamepad attachment.
     *
     * @param connection_id Fake connection generation.
     * @return True when an attach operation was observed.
     */
    bool received_attach(int connection_id) const {
      std::lock_guard lock {state_->mutex};
      return std::ranges::any_of(state_->sent, [connection_id](const auto &sent) {
        return sent.connection_id == connection_id && sent.item.kind == xbox_remote::input::item_kind_e::attach;
      });
    }

    std::shared_ptr<connection_state_t> state_;  ///< Shared fake connection observations.

  private:
    config::input_t original_input_;  ///< Input configuration restored after the test.
    bool owns_task_pool_ = false;  ///< Whether this fixture started the global task pool.
  };
}  // namespace

TEST_F(XboxRemoteInputLifecycleTest, ReusesWorkerWithinGraceAndMigratesRetainedGamepadAfterExpiry) {
  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_until([]() {
    return input::xbox_remote_status().state == "ready";
  }));
  ASSERT_EQ(state_->created, 1);

  auto first = input::alloc(std::make_shared<safe::mail_raw_t>(), "moonlight-client");
  ASSERT_GE(input::testing::alloc_gamepad(first, 0, {}), 0);
  ASSERT_TRUE(wait_until([this]() {
    return received_attach(1);
  }));
  ASSERT_TRUE(input::testing::update_gamepad(first, 0, {platf::A, 0, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(wait_until([this]() {
    return received_button(1, xbox_remote::protocol::gamepad_button_e::a);
  }));

  input::testing::neutralize_gamepads(first);
  input::suspend_xbox_remote_for_disconnected_stream();
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(input::xbox_remote_status().state, "ready");
  EXPECT_EQ(state_->closed, 0);

  input::resume_xbox_remote_for_stream("Xbox");
  auto resumed = input::alloc(std::make_shared<safe::mail_raw_t>(), "moonlight-client");
  flush_input_tasks();
  EXPECT_EQ(resumed, first);
  EXPECT_EQ(state_->created, 1);
  ASSERT_TRUE(input::testing::update_gamepad(resumed, 0, {platf::B, 0, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(wait_until([this]() {
    return received_button(1, xbox_remote::protocol::gamepad_button_e::b);
  }));

  input::testing::neutralize_gamepads(resumed);
  input::suspend_xbox_remote_for_disconnected_stream();
  ASSERT_TRUE(wait_until([this]() {
    return state_->closed == 1 && input::xbox_remote_status().state == "idle";
  }));

  input::resume_xbox_remote_for_stream("Xbox");
  ASSERT_TRUE(wait_until([this]() {
    return state_->created == 2 && input::xbox_remote_status().state == "ready";
  }));
  const auto original_id = input::testing::gamepad_id(resumed, 0);
  auto recreated = input::alloc(std::make_shared<safe::mail_raw_t>(), "moonlight-client");
  flush_input_tasks();
  EXPECT_EQ(recreated, resumed);
  EXPECT_EQ(input::testing::gamepad_id(recreated, 0), original_id);
  ASSERT_TRUE(wait_until([this]() {
    return received_attach(2);
  }));
  ASSERT_TRUE(input::testing::update_gamepad(recreated, 0, {platf::X, 0, 0, 0, 0, 0, 0}));
  ASSERT_TRUE(wait_until([this]() {
    return received_button(2, xbox_remote::protocol::gamepad_button_e::x);
  }));

  input::testing::neutralize_gamepads(recreated);
  input::suspend_xbox_remote_for_disconnected_stream();
  input::select_gamepad_output({});
  EXPECT_EQ(state_->closed, 2);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(state_->closed, 2);
}

TEST_F(XboxRemoteInputLifecycleTest, ZeroGraceStopsImmediatelyAndOnlyXboxResumeRecreatesWorker) {
  config::input.xbox_remote_idle_timeout = 0ms;
  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_until([]() {
    return input::xbox_remote_status().state == "ready";
  }));

  input::suspend_xbox_remote_for_disconnected_stream();
  EXPECT_EQ(input::xbox_remote_status().state, "idle");
  EXPECT_EQ(state_->closed, 1);

  input::resume_xbox_remote_for_stream("HDMI Input");
  EXPECT_EQ(state_->created, 1);
  input::resume_xbox_remote_for_stream("Xbox");
  ASSERT_TRUE(wait_until([this]() {
    return state_->created == 2 && input::xbox_remote_status().state == "ready";
  }));
}
