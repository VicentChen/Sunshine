/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for retained stream input and virtual gamepad lifecycle behavior.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "../tests_common.h"
#include "src/config.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"

namespace {
  using namespace std::chrono_literals;

#ifdef SUNSHINE_XBOX_REMOTE_PLAY
  /**
   * @brief Shared observations from application-scoped fake Xbox connections.
   */
  struct xbox_connection_state_t {
    std::atomic<int> created {0};  ///< Number of factory invocations.
    std::atomic<int> closed {0};  ///< Number of closed connections.
    std::mutex mutex;  ///< Protects sent operation history.
    std::vector<xbox_remote::input::item_kind_e> sent;  ///< Operations accepted by fake transports.
  };

  /**
   * @brief Successful cancellable fake connection used by input lifecycle tests.
   */
  class xbox_connection_t final: public xbox_remote::worker::connection_t {
  public:
    /**
     * @brief Bind the fake to shared observations.
     *
     * @param state Shared test state.
     */
    explicit xbox_connection_t(std::shared_ptr<xbox_connection_state_t> state):
        state_(std::move(state)) {
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
      state_->sent.push_back(item.kind);
      return true;
    }

    /**
     * @copydoc xbox_remote::worker::connection_t::poll
     */
    xbox_remote::worker::result_t poll(const std::function<bool()> &cancelled, std::optional<xbox_remote::protocol::vibration_t> &vibration) override {
      static_cast<void>(vibration);
      std::this_thread::sleep_for(1ms);
      return cancelled() ? xbox_remote::worker::result_t {} : xbox_remote::worker::result_t {};
    }

    /**
     * @copydoc xbox_remote::worker::connection_t::close
     */
    xbox_remote::worker::result_t close() override {
      ++state_->closed;
      return {};
    }

  private:
    std::shared_ptr<xbox_connection_state_t> state_;  ///< Shared test observations.
  };

  /**
   * @brief Wait briefly for the asynchronous Xbox worker to reach one state.
   *
   * @param expected Fixed worker state name.
   * @return @c true when the state was observed before the short deadline.
   */
  bool wait_for_xbox_state(std::string_view expected) {
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < deadline) {
      if (input::xbox_remote_status().state == expected) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
#endif

  /**
   * @brief Fixture that installs a fake global virtual input backend.
   */
  class InputGamepadSessionTest: public ::testing::Test {
  protected:
    /**
     * @brief Preserve configuration and install observable fake devices.
     */
    void SetUp() override {
      original_input_ = config::input;
      config::input.controller = true;
      config::input.gamepad = "xseries";
      config::input.controller_output = "virtual";

      auto platform_input = platf::input();
      ASSERT_TRUE(platform_input);
      auto &context = platf::virtualhid::get_input_context(platform_input);
      context = platf::virtualhid::input_context_t {lvh::BackendKind::fake};
      context_ = &context;
      runtime_ = context.runtime.get();
      ASSERT_NE(runtime_, nullptr);
      input::testing::set_platform_input(std::move(platform_input));
    }

    /**
     * @brief Destroy retained test sessions and restore configuration.
     */
    void TearDown() override {
      input::select_gamepad_output({});
      input::testing::set_xbox_remote_connection_factory({});
      input::terminate_gamepads();
      input::testing::set_platform_input({});
      context_ = nullptr;
      runtime_ = nullptr;
      config::input = std::move(original_input_);
    }

    /**
     * @brief Access the fake runtime installed for the current test.
     *
     * @return Fake libvirtualhid runtime.
     */
    lvh::Runtime &runtime() const {
      return *runtime_;
    }

    /**
     * @brief Access the shared libvirtualhid input context installed for the test.
     *
     * @return Fake input context.
     */
    platf::virtualhid::input_context_t &context() const {
      return *context_;
    }

  private:
    platf::virtualhid::input_context_t *context_ = nullptr;  ///< Fake input context installed in the global backend.
    lvh::Runtime *runtime_ = nullptr;  ///< Fake runtime installed in the global input backend.
    config::input_t original_input_;  ///< Input configuration restored after each test.
  };
}  // namespace

TEST_F(InputGamepadSessionTest, ReusesGamepadsAcrossPauseAndDestroysThemOnTermination) {
  const std::string session_id = "paired-client-certificate";
  auto first_mail = std::make_shared<safe::mail_raw_t>();
  auto first = input::alloc(first_mail, session_id);
  ASSERT_NE(first, nullptr);
  const auto active_devices_before_gamepad = runtime().active_device_count();

  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  const auto original_id = input::testing::alloc_gamepad(first, 0, metadata);
  ASSERT_GE(original_id, 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  auto *adapter = platf::virtualhid::gamepad_adapter_for_testing(context(), original_id);
  ASSERT_NE(adapter, nullptr);
  platf::virtualhid::gamepad_update(context(), original_id, {platf::A, 255, 0, 0, 0, 0, 0});
  EXPECT_TRUE(adapter->state().buttons.test(lvh::GamepadButton::a));
  input::testing::neutralize_gamepads(first);
  EXPECT_FALSE(adapter->state().buttons.test(lvh::GamepadButton::a));
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  const std::weak_ptr<input::input_t> paused = first;
  first.reset();
  EXPECT_FALSE(paused.expired());
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  auto resumed_mail = std::make_shared<safe::mail_raw_t>();
  auto resumed = input::alloc(resumed_mail, session_id);
  ASSERT_NE(resumed, nullptr);
  EXPECT_EQ(paused.lock(), resumed);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), original_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);

  input::terminate_gamepads(session_id);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), -1);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad);

  auto replacement = input::alloc(std::make_shared<safe::mail_raw_t>(), session_id);
  EXPECT_NE(replacement, resumed);
}

TEST_F(InputGamepadSessionTest, RefreshesSharedMouseAfterLicenseStateChanges) {
  ASSERT_NE(context().mouse, nullptr);
  const auto original_mouse_id = context().mouse->device_id();
  const auto active_devices = runtime().active_device_count();

  input::refresh_virtual_mouse();

  ASSERT_NE(context().mouse, nullptr);
  EXPECT_NE(context().mouse->device_id(), original_mouse_id);
  EXPECT_EQ(runtime().active_device_count(), active_devices);
}

TEST_F(InputGamepadSessionTest, ReportsNxbtAvailableWithoutVirtualGamepadSupport) {
  input::testing::set_platform_input({});
  EXPECT_TRUE(input::probe_gamepads());

  config::input.controller_output = "nxbt";
  EXPECT_FALSE(input::probe_gamepads());

  config::input.controller_output = "both";
  EXPECT_FALSE(input::probe_gamepads());
}

TEST_F(InputGamepadSessionTest, PrewarmsFirstNxbtControllerWhenSessionStarts) {
  config::input.controller_output = "nxbt";
  config::input.nxbt_socket = "/tmp/sunshine-nxbt-prewarm-missing.sock";
  input::testing::reconfigure_gamepad_router();

  auto session = input::alloc(std::make_shared<safe::mail_raw_t>(), "nxbt-prewarm-session");
  EXPECT_EQ(input::testing::gamepad_id(session, 0), 0);

  input::terminate_gamepads("nxbt-prewarm-session");
  config::input.controller_output = "virtual";
  input::testing::reconfigure_gamepad_router();
}

TEST_F(InputGamepadSessionTest, RoutesOnlyNintendoSwitchControllerInputToTheConfiguredOutput) {
  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};

  input::select_gamepad_output("HDMI Input");
  auto hdmi_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "hdmi-input-routing");
  const auto active_devices_before_hdmi_gamepad = runtime().active_device_count();
  ASSERT_GE(input::testing::alloc_gamepad(hdmi_input, 0, metadata), 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_hdmi_gamepad);
  input::terminate_gamepads("hdmi-input-routing");

  input::select_gamepad_output("Nintendo Switch");
  auto switch_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "switch-input-routing");
  const auto active_devices_before_switch_gamepad = runtime().active_device_count();
  ASSERT_GE(input::testing::alloc_gamepad(switch_input, 0, metadata), 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_switch_gamepad + 1);
}

#ifdef SUNSHINE_XBOX_REMOTE_PLAY
TEST_F(InputGamepadSessionTest, StartsXboxWorkerAutomaticallyForXboxApplicationAndStopsWithNeutralCleanup) {
  auto state = std::make_shared<xbox_connection_state_t>();
  input::testing::set_xbox_remote_connection_factory([state]() {
    ++state->created;
    return std::make_unique<xbox_connection_t>(state);
  });
  config::input.xbox_remote_app = "Xbox";

  input::select_gamepad_output("HDMI Input");
  EXPECT_EQ(state->created, 0);
  EXPECT_EQ(input::xbox_remote_status().state, "idle");

  config::input.xbox_remote_enabled = false;
  input::select_gamepad_output("Xbox");
  EXPECT_EQ(state->created, 0);
  EXPECT_EQ(input::xbox_remote_status().state, "idle");

  config::input.xbox_remote_enabled = true;
  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_for_xbox_state("ready"));
  EXPECT_EQ(state->created, 1);

  auto session = input::alloc(std::make_shared<safe::mail_raw_t>(), "xbox-routing");
  const auto active_devices_before_gamepad = runtime().active_device_count();
  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  ASSERT_GE(input::testing::alloc_gamepad(session, 0, metadata), 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad);

  input::suspend_xbox_remote_for_disconnected_stream();
  EXPECT_EQ(input::xbox_remote_status().state, "idle");
  EXPECT_EQ(state->closed, 1);
  {
    std::lock_guard lock {state->mutex};
    ASSERT_GE(state->sent.size(), 2);
    EXPECT_EQ(state->sent[state->sent.size() - 2], xbox_remote::input::item_kind_e::neutralize);
    EXPECT_EQ(state->sent.back(), xbox_remote::input::item_kind_e::detach);
  }

  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_for_xbox_state("ready"));
  EXPECT_EQ(state->created, 2);
  input::select_gamepad_output({});
  EXPECT_EQ(state->closed, 2);
}

TEST_F(InputGamepadSessionTest, ReplacesRepeatedXboxApplicationStartsDeterministically) {
  auto state = std::make_shared<xbox_connection_state_t>();
  input::testing::set_xbox_remote_connection_factory([state]() {
    ++state->created;
    return std::make_unique<xbox_connection_t>(state);
  });
  config::input.xbox_remote_app = "Xbox";

  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_for_xbox_state("ready"));
  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_for_xbox_state("ready"));
  EXPECT_EQ(state->created, 2);
  EXPECT_EQ(state->closed, 1);

  input::select_gamepad_output({});
  EXPECT_EQ(state->closed, 2);
}

TEST_F(InputGamepadSessionTest, TransfersRetainedXboxControllerToANewMoonlightClient) {
  auto state = std::make_shared<xbox_connection_state_t>();
  input::testing::set_xbox_remote_connection_factory([state]() {
    ++state->created;
    return std::make_unique<xbox_connection_t>(state);
  });
  config::input.xbox_remote_enabled = true;
  config::input.xbox_remote_app = "Xbox";
  input::select_gamepad_output("Xbox");
  ASSERT_TRUE(wait_for_xbox_state("ready"));

  auto first = input::alloc(std::make_shared<safe::mail_raw_t>(), "moonlight-client-a");
  const platf::gamepad_arrival_t metadata {LI_CTYPE_XBOX, 0, 0};
  const auto first_id = input::testing::alloc_gamepad(first, 0, metadata);
  ASSERT_GE(first_id, 0);

  const auto attach_deadline = std::chrono::steady_clock::now() + 500ms;
  while (std::chrono::steady_clock::now() < attach_deadline) {
    bool attached;
    {
      std::lock_guard lock {state->mutex};
      attached = std::ranges::count(state->sent, xbox_remote::input::item_kind_e::attach) >= 1;
    }
    if (attached) {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }
  std::size_t attach_count;
  std::size_t detach_count;
  {
    std::lock_guard lock {state->mutex};
    attach_count = std::ranges::count(state->sent, xbox_remote::input::item_kind_e::attach);
    detach_count = std::ranges::count(state->sent, xbox_remote::input::item_kind_e::detach);
  }
  ASSERT_EQ(attach_count, 1);
  ASSERT_EQ(detach_count, 0);

  auto resumed = input::alloc(std::make_shared<safe::mail_raw_t>(), "moonlight-client-a");
  EXPECT_EQ(resumed, first);
  EXPECT_EQ(input::testing::gamepad_id(resumed, 0), first_id);

  auto second = input::alloc(std::make_shared<safe::mail_raw_t>(), "moonlight-client-b");
  const auto release_deadline = std::chrono::steady_clock::now() + 500ms;
  while (input::testing::gamepad_id(first, 0) >= 0 && std::chrono::steady_clock::now() < release_deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(input::testing::gamepad_id(first, 0), -1);
  EXPECT_EQ(input::testing::gamepad_id(second, 0), first_id);
  EXPECT_EQ(input::testing::alloc_gamepad(second, 0, metadata), first_id);
  {
    std::lock_guard lock {state->mutex};
    EXPECT_EQ(std::ranges::count(state->sent, xbox_remote::input::item_kind_e::attach), attach_count);
    EXPECT_EQ(std::ranges::count(state->sent, xbox_remote::input::item_kind_e::detach), detach_count);
  }
}
#endif
