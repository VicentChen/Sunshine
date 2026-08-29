/**
 * @file tests/unit/test_nxbt_config.cpp
 * @brief Tests for NXBT controller-output configuration parsing and validation.
 */

// standard includes
#include <chrono>
#include <utility>

// local includes
#include "../tests_common.h"
#include "src/config.h"

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Preserve process-wide input configuration around each parser test.
   */
  class NxbtConfigTest: public ::testing::Test {
  protected:
    /**
     * @brief Save the active input settings.
     */
    void SetUp() override {
      original_ = config::input;
    }

    /**
     * @brief Restore the input settings changed by the parser.
     */
    void TearDown() override {
      config::input = std::move(original_);
    }

  private:
    config::input_t original_;  ///< Input settings restored after the test.
  };
}  // namespace

TEST_F(NxbtConfigTest, DefaultsToVirtualOutputWithSafeNxbtValues) {
  EXPECT_EQ(config::input.controller_output, "virtual");
  EXPECT_EQ(config::input.nxbt_socket, "/run/nxbt-bridge/control.sock");
  EXPECT_EQ(config::input.nxbt_controller_slot, 0);
  EXPECT_EQ(config::input.nxbt_face_buttons, "labels");
  EXPECT_EQ(config::input.nxbt_trigger_press_threshold, 64);
  EXPECT_EQ(config::input.nxbt_trigger_release_threshold, 48);
  EXPECT_EQ(config::input.nxbt_watchdog_timeout, 150ms);
}

TEST_F(NxbtConfigTest, AppliesEverySupportedNxbtSetting) {
  config::apply_config_for_test(
    "controller_output = both\n"
    "nxbt_socket = /tmp/sunshine-nxbt.sock\n"
    "nxbt_controller_slot = 7\n"
    "nxbt_face_buttons = positions\n"
    "nxbt_trigger_press_threshold = 90\n"
    "nxbt_trigger_release_threshold = 70\n"
    "nxbt_watchdog_timeout = 450\n"
  );

#if defined(__linux__)
  EXPECT_EQ(config::input.controller_output, "both");
#else
  EXPECT_EQ(config::input.controller_output, "virtual");
#endif
  EXPECT_EQ(config::input.nxbt_socket, "/tmp/sunshine-nxbt.sock");
  EXPECT_EQ(config::input.nxbt_controller_slot, 7);
  EXPECT_EQ(config::input.nxbt_face_buttons, "positions");
  EXPECT_EQ(config::input.nxbt_trigger_press_threshold, 90);
  EXPECT_EQ(config::input.nxbt_trigger_release_threshold, 70);
  EXPECT_EQ(config::input.nxbt_watchdog_timeout, 450ms);
}

TEST_F(NxbtConfigTest, RejectsInvalidPathsRangesPoliciesAndHysteresis) {
  config::input.controller_output = "virtual";
  config::input.nxbt_socket = "/run/known.sock";
  config::input.nxbt_controller_slot = 3;
  config::input.nxbt_face_buttons = "labels";
  config::input.nxbt_trigger_press_threshold = 100;
  config::input.nxbt_trigger_release_threshold = 80;
  config::input.nxbt_watchdog_timeout = 200ms;

  config::apply_config_for_test(
    "controller_output = unsupported\n"
    "nxbt_socket = relative.sock\n"
    "nxbt_controller_slot = 16\n"
    "nxbt_face_buttons = diagonal\n"
    "nxbt_trigger_press_threshold = 75\n"
    "nxbt_trigger_release_threshold = 75\n"
    "nxbt_watchdog_timeout = 49\n"
  );

  EXPECT_EQ(config::input.controller_output, "virtual");
  EXPECT_EQ(config::input.nxbt_socket, "/run/known.sock");
  EXPECT_EQ(config::input.nxbt_controller_slot, 3);
  EXPECT_EQ(config::input.nxbt_face_buttons, "labels");
  EXPECT_EQ(config::input.nxbt_trigger_press_threshold, 100);
  EXPECT_EQ(config::input.nxbt_trigger_release_threshold, 80);
  EXPECT_EQ(config::input.nxbt_watchdog_timeout, 200ms);
}
