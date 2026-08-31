/**
 * @file tests/unit/test_nvhttp_state.cpp
 * @brief Tests isolated GameStream state loading behavior.
 */

#include "src/config.h"
#include "src/nvhttp.h"
#include "tests/tests_common.h"

#include <filesystem>
#include <fstream>

namespace {
  /** @brief A malformed state without a root object is replaced in memory safely. */
  TEST(NvHttpStateTest, MissingRootGeneratesHostIdentityWithoutThrowing) {
    const auto original_file_state = config::nvhttp.file_state;
    const auto original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];
    const std::filesystem::path state_path = SUNSHINE_TEST_BIN_DIR "/rkmpp-test-state/missing-root.json";
    std::filesystem::create_directories(state_path.parent_path());
    {
      std::ofstream state_file {state_path};
      ASSERT_TRUE(state_file.is_open());
      state_file << R"({"unexpected":true})";
    }

    config::nvhttp.file_state = state_path.string();
    config::sunshine.flags[config::flag::FRESH_STATE] = true;
    std::string unique_id;
    EXPECT_NO_THROW(unique_id = nvhttp::testing::load_state_unique_id());
    EXPECT_FALSE(unique_id.empty());

    config::nvhttp.file_state = original_file_state;
    config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
    std::filesystem::remove(state_path);
  }
}  // namespace
