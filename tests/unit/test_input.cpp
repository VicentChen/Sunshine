/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for retained stream input and virtual gamepad lifecycle behavior.
 */

// standard includes
#include <memory>
#include <string>

// local includes
#include "../tests_common.h"
#include "src/config.h"
#include "src/input.h"
#include "src/platform/virtualhid_input.h"

namespace {
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
  const auto active_devices_before_gamepad = runtime().active_device_count();

  input::select_gamepad_output("HDMI Input");
  auto hdmi_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "hdmi-input-routing");
  ASSERT_GE(input::testing::alloc_gamepad(hdmi_input, 0, metadata), 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad);
  input::terminate_gamepads("hdmi-input-routing");

  input::select_gamepad_output("Nintendo Switch");
  auto switch_input = input::alloc(std::make_shared<safe::mail_raw_t>(), "switch-input-routing");
  ASSERT_GE(input::testing::alloc_gamepad(switch_input, 0, metadata), 0);
  EXPECT_EQ(runtime().active_device_count(), active_devices_before_gamepad + 1);
}
