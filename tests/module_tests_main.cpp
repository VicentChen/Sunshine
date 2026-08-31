/**
 * @file tests/module_tests_main.cpp
 * @brief Isolated entry point for project-specific unit test modules.
 */
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

#include <src/config.h>

#ifndef SUNSHINE_MODULE_TESTS
  #error "module_tests_main.cpp must only be used by a module test target"
#endif

/**
 * @brief Run one isolated project-specific unit test module.
 *
 * @param argc Number of command-line arguments.
 * @param argv Command-line arguments forwarded to GoogleTest.
 * @return GoogleTest process exit status.
 */
int main(int argc, char **argv) {
  // Keep module validation detached from the user's real Sunshine state even
  // if a future test invokes production initialization.
  config::sunshine.flags[config::flag::FRESH_STATE] = true;
  config::nvhttp.file_state = SUNSHINE_TEST_BIN_DIR "/" SUNSHINE_TEST_MODULE "-test-state/sunshine_state.json";
  config::sunshine.credentials_file = config::nvhttp.file_state;

  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
