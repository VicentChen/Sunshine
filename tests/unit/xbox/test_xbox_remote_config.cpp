/**
 * @file tests/unit/xbox/test_xbox_remote_config.cpp
 * @brief Tests for Xbox Remote Play configuration defaults and validation.
 */

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <utility>

// local includes
#include "src/config.h"
#include "tests/tests_common.h"

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Preserve input configuration and provide a private temporary token file.
   */
  class XboxRemoteConfigTest: public ::testing::Test {
  protected:
    /**
     * @brief Save configuration and create a regular owner-writable token placeholder.
     */
    void SetUp() override {
      original_ = config::input;
      token_file_ = std::filesystem::temp_directory_path() / "sunshine-xbox-remote-config-token.json";
      std::ofstream {token_file_} << "{}";
      std::filesystem::permissions(token_file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }

    /**
     * @brief Remove the placeholder and restore process-wide configuration.
     */
    void TearDown() override {
      std::error_code ignored;
      std::filesystem::remove(token_file_, ignored);
      config::input = std::move(original_);
    }

    std::filesystem::path token_file_;  ///< Test-owned token placeholder.

  private:
    config::input_t original_;  ///< Input settings restored after each test.
  };
}  // namespace

TEST_F(XboxRemoteConfigTest, DefaultsToAutomaticXboxApplicationWithSafeSelectors) {
  EXPECT_TRUE(config::input.xbox_remote_enabled);
  EXPECT_EQ(config::input.xbox_remote_app, "Xbox");
  EXPECT_TRUE(std::filesystem::path {config::input.xbox_remote_token_file}.is_absolute());
  EXPECT_TRUE(config::input.xbox_remote_console_id.empty());
  EXPECT_TRUE(config::input.xbox_remote_wake);
  EXPECT_EQ(config::input.xbox_remote_idle_timeout, 5min);
}

TEST_F(XboxRemoteConfigTest, AppliesCompleteValidatedConfiguration) {
  config::apply_config_for_test(
    "xbox_remote_enabled = true\n"
    "xbox_remote_app = HDMI Input\n"
    "xbox_remote_token_file = " +
    token_file_.string() +
    "\n"
    "xbox_remote_console_id = stable-console\n"
    "xbox_remote_wake = false\n"
    "xbox_remote_idle_timeout = 42\n"
  );

  EXPECT_TRUE(config::input.xbox_remote_enabled);
  EXPECT_EQ(config::input.xbox_remote_app, "HDMI Input");
  EXPECT_EQ(config::input.xbox_remote_token_file, token_file_);
  EXPECT_EQ(config::input.xbox_remote_console_id, "stable-console");
  EXPECT_FALSE(config::input.xbox_remote_wake);
  EXPECT_EQ(config::input.xbox_remote_idle_timeout, 42s);
}

TEST_F(XboxRemoteConfigTest, AllowsUniqueConsoleAutoSelection) {
  config::apply_config_for_test(
    "xbox_remote_enabled = true\n"
    "xbox_remote_app = Xbox\n"
    "xbox_remote_token_file = " +
    token_file_.string() +
    "\n"
    "xbox_remote_console_id = \n"
  );

  EXPECT_TRUE(config::input.xbox_remote_enabled);
  EXPECT_TRUE(config::input.xbox_remote_console_id.empty());
}

TEST_F(XboxRemoteConfigTest, RejectsIncompleteRelativeMissingAndReadOnlyStores) {
  config::apply_config_for_test(
    "xbox_remote_enabled = true\n"
    "xbox_remote_app = \n"
    "xbox_remote_token_file = relative.json\n"
    "xbox_remote_console_id = \n"
  );
  EXPECT_FALSE(config::input.xbox_remote_enabled);

  config::input.xbox_remote_enabled = true;
  config::input.xbox_remote_app = "Xbox";
  config::input.xbox_remote_console_id = "stable-console";
  config::apply_config_for_test("xbox_remote_token_file = /tmp/sunshine-xbox-remote-definitely-missing.json\n");
  EXPECT_FALSE(config::input.xbox_remote_enabled);

  std::filesystem::permissions(token_file_, std::filesystem::perms::owner_read);
  config::input.xbox_remote_enabled = true;
  config::apply_config_for_test("xbox_remote_token_file = " + token_file_.string() + "\n");
  EXPECT_FALSE(config::input.xbox_remote_enabled);

  std::filesystem::permissions(token_file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read);
  config::input.xbox_remote_enabled = true;
  config::apply_config_for_test("xbox_remote_token_file = " + token_file_.string() + "\n");
  EXPECT_FALSE(config::input.xbox_remote_enabled);
}

TEST_F(XboxRemoteConfigTest, BoundsRemotePlayIdleTimeout) {
  config::input.xbox_remote_idle_timeout = 5min;
  config::apply_config_for_test("xbox_remote_idle_timeout = -1\n");
  EXPECT_EQ(config::input.xbox_remote_idle_timeout, 5min);

  config::apply_config_for_test("xbox_remote_idle_timeout = 86401\n");
  EXPECT_EQ(config::input.xbox_remote_idle_timeout, 5min);

  config::apply_config_for_test("xbox_remote_idle_timeout = 0\n");
  EXPECT_EQ(config::input.xbox_remote_idle_timeout, 0s);
}
