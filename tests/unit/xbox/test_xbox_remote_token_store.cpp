/**
 * @file tests/unit/xbox/test_xbox_remote_token_store.cpp
 * @brief Tests for secure Xbox Remote Play OAuth credential persistence.
 */

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
  // system includes
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include "tests/tests_common.h"
#include "src/xbox_remote/token_store.h"

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Temporary directory removed after each token-store test.
   */
  class XboxRemoteTokenStoreTest: public ::testing::Test {
  protected:
    /**
     * @brief Create an isolated test directory.
     */
    void SetUp() override {
      static std::uint64_t sequence = 0;
      directory_ = std::filesystem::temp_directory_path() /
                   ("sunshine-xbox-token-store-" + std::to_string(++sequence));
      std::filesystem::remove_all(directory_);
      std::filesystem::create_directory(directory_);
      path_ = directory_ / "tokens.json";
    }

    /**
     * @brief Remove the isolated test directory.
     */
    void TearDown() override {
      std::filesystem::permissions(directory_, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
      std::filesystem::remove_all(directory_);
    }

    /**
     * @brief Return deterministic credentials with visibly secret test values.
     *
     * @return Complete credentials.
     */
    xbox_remote::auth::oauth_credentials_t credentials() const {
      return {
        "access-secret-fixture",
        "refresh-secret-fixture",
        std::chrono::system_clock::time_point {1700000000s},
      };
    }

    std::filesystem::path directory_;  ///< Isolated test directory.
    std::filesystem::path path_;  ///< Token-file path inside the directory.
  };
}  // namespace

TEST(XboxRemoteTokenRefreshTest, UsesInjectedWallClockAndEarlyRefreshMargin) {
  using xbox_remote::auth::oauth_credentials_t;
  using xbox_remote::auth::should_refresh;
  const auto now = std::chrono::system_clock::time_point {1000s};

  oauth_credentials_t credentials {"access", "refresh", now + 301s};
  EXPECT_FALSE(should_refresh(credentials, now));
  credentials.expires_at = now + 300s;
  EXPECT_TRUE(should_refresh(credentials, now));
  credentials.expires_at = now - 1s;
  EXPECT_TRUE(should_refresh(credentials, now));
  credentials.access_token.clear();
  credentials.expires_at = now + 1h;
  EXPECT_TRUE(should_refresh(credentials, now));
  credentials.access_token = "access";
  EXPECT_FALSE(should_refresh(credentials, now, -1s));
}

#ifndef _WIN32
TEST_F(XboxRemoteTokenStoreTest, SavesLoadsAndAtomicallyReplacesOwnerOnlyCredentials) {
  using namespace xbox_remote::auth;
  token_store_t store {path_};
  auto original = credentials();
  ASSERT_TRUE(store.save(original));

  struct stat metadata {};
  ASSERT_EQ(::stat(path_.c_str(), &metadata), 0);
  EXPECT_EQ(metadata.st_mode & 0777, 0600);
  EXPECT_FALSE(std::filesystem::exists(directory_ / ".tokens.json.tmp"));

  oauth_credentials_t loaded;
  ASSERT_TRUE(store.load(loaded));
  EXPECT_EQ(loaded.access_token, original.access_token);
  EXPECT_EQ(loaded.refresh_token, original.refresh_token);
  EXPECT_EQ(loaded.expires_at, original.expires_at);

  auto replacement = original;
  replacement.access_token = "replacement-access-secret";
  replacement.refresh_token = "replacement-refresh-secret";
  replacement.expires_at += 1h;
  ASSERT_TRUE(store.save(replacement));
  ASSERT_TRUE(store.load(loaded));
  EXPECT_EQ(loaded.access_token, replacement.access_token);
  EXPECT_EQ(loaded.refresh_token, replacement.refresh_token);
  EXPECT_EQ(loaded.expires_at, replacement.expires_at);
}

TEST_F(XboxRemoteTokenStoreTest, RejectsInsecurePermissionsAndSymbolicLinks) {
  using namespace xbox_remote::auth;
  token_store_t store {path_};
  ASSERT_TRUE(store.save(credentials()));
  ASSERT_EQ(::chmod(path_.c_str(), 0644), 0);

  oauth_credentials_t loaded;
  EXPECT_EQ(store.load(loaded).error, token_store_error_e::insecure_file);

  const auto target = directory_ / "target.json";
  ASSERT_EQ(::rename(path_.c_str(), target.c_str()), 0);
  ASSERT_EQ(::symlink(target.c_str(), path_.c_str()), 0);
  EXPECT_EQ(store.load(loaded).error, token_store_error_e::insecure_file);
}

TEST_F(XboxRemoteTokenStoreTest, CorruptAndOversizedFilesDoNotReplaceExistingMemoryCredentials) {
  using namespace xbox_remote::auth;
  oauth_credentials_t loaded = credentials();
  loaded.access_token = "keep-existing-access";
  loaded.refresh_token = "keep-existing-refresh";
  const auto expected = loaded;

  {
    std::ofstream output(path_);
    output << "{";
  }
  ASSERT_EQ(::chmod(path_.c_str(), 0600), 0);
  token_store_t store {path_};
  EXPECT_EQ(store.load(loaded).error, token_store_error_e::invalid_data);
  EXPECT_EQ(loaded.access_token, expected.access_token);
  EXPECT_EQ(loaded.refresh_token, expected.refresh_token);
  EXPECT_EQ(loaded.expires_at, expected.expires_at);

  {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    output.seekp(64 * 1024);
    output.put('x');
  }
  ASSERT_EQ(::chmod(path_.c_str(), 0600), 0);
  EXPECT_EQ(store.load(loaded).error, token_store_error_e::too_large);
  EXPECT_EQ(loaded.access_token, expected.access_token);
}

TEST_F(XboxRemoteTokenStoreTest, ReturnsStructuredSanitizedErrors) {
  using namespace xbox_remote::auth;
  token_store_t store {path_};
  oauth_credentials_t loaded;
  const auto missing = store.load(loaded);
  EXPECT_EQ(missing.error, token_store_error_e::not_found);
  EXPECT_EQ(missing.message.find("secret"), std::string::npos);

  const oauth_credentials_t incomplete;
  const auto rejected = store.save(incomplete);
  EXPECT_EQ(rejected.error, token_store_error_e::invalid_data);
  EXPECT_EQ(rejected.message.find("token value"), std::string::npos);

  token_store_t invalid {directory_};
  EXPECT_EQ(invalid.save(credentials()).error, token_store_error_e::io_error);
}

TEST_F(XboxRemoteTokenStoreTest, SaveFailurePreservesPreviouslyCommittedCredentials) {
  using namespace xbox_remote::auth;
  token_store_t store {path_};
  const auto original = credentials();
  ASSERT_TRUE(store.save(original));

  ASSERT_EQ(::chmod(directory_.c_str(), 0500), 0);
  auto replacement = original;
  replacement.access_token = "must-not-commit";
  const auto failed = store.save(replacement);
  ASSERT_EQ(::chmod(directory_.c_str(), 0700), 0);
  EXPECT_EQ(failed.error, token_store_error_e::io_error);

  oauth_credentials_t loaded;
  ASSERT_TRUE(store.load(loaded));
  EXPECT_EQ(loaded.access_token, original.access_token);
}
#endif
