/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

#ifdef SUNSHINE_RKMPP_TESTS
  #include <src/config.h>
#endif

int main(int argc, char **argv) {
#ifdef SUNSHINE_RKMPP_TESTS
  // Defense in depth: keep RKMPP validation detached from the user's real
  // Sunshine state even if a future test invokes production initialization.
  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  config::nvhttp.file_state = SUNSHINE_TEST_BIN_DIR "/rkmpp-test-state/sunshine_state.json";
  config::sunshine.credentials_file = config::nvhttp.file_state;
#endif

  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
