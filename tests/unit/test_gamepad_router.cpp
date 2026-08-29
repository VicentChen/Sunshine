/**
 * @file tests/unit/test_gamepad_router.cpp
 * @brief Tests for virtual-HID and NXBT gamepad output routing.
 */

// standard includes
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "../tests_common.h"
#include "src/input/gamepad_router.h"

namespace {
  /**
   * @brief Observable sink with configurable allocation, rebind, and update results.
   */
  class fake_sink_t final: public input::gamepad::sink_t {
  public:
    /**
     * @brief Create a named fake sink that records every lifecycle call.
     *
     * @param name Name appended to the shared call log.
     * @param calls Shared observable call log.
     */
    fake_sink_t(std::string name, std::vector<std::string> &calls):
        name_(std::move(name)),
        calls_(calls) {
    }

    /**
     * @brief Record allocation and return the configured allocation result.
     *
     * @param id Sunshine controller identifiers.
     * @param arrival Client controller metadata.
     * @param feedback_queue Client feedback queue.
     * @return Configured allocation result.
     */
    bool alloc(const platf::gamepad_id_t &id, const platf::gamepad_arrival_t &arrival, platf::feedback_queue_t feedback_queue) override {
      static_cast<void>(id);
      static_cast<void>(arrival);
      static_cast<void>(feedback_queue);
      calls_.push_back(name_ + ".alloc");
      return alloc_result;
    }

    /**
     * @brief Record rebind and return the configured result.
     *
     * @param id Sunshine controller identifiers.
     * @param feedback_queue Client feedback queue.
     * @return Configured rebind result.
     */
    bool rebind(const platf::gamepad_id_t &id, platf::feedback_queue_t feedback_queue) override {
      static_cast<void>(id);
      static_cast<void>(feedback_queue);
      calls_.push_back(name_ + ".rebind");
      return rebind_result;
    }

    /**
     * @brief Record one state update and return the configured result.
     *
     * @param id Sunshine controller identifiers.
     * @param state Complete controller state.
     * @return Configured update result.
     */
    bool update(const platf::gamepad_id_t &id, const platf::gamepad_state_t &state) override {
      static_cast<void>(id);
      static_cast<void>(state);
      calls_.push_back(name_ + ".update");
      return update_result;
    }

    /**
     * @brief Record controller neutralization.
     *
     * @param id Sunshine controller identifiers.
     */
    void neutralize(const platf::gamepad_id_t &id) override {
      static_cast<void>(id);
      calls_.push_back(name_ + ".neutralize");
    }

    /**
     * @brief Record controller release.
     *
     * @param id Sunshine controller identifiers.
     */
    void free(const platf::gamepad_id_t &id) override {
      static_cast<void>(id);
      calls_.push_back(name_ + ".free");
    }

    bool alloc_result = true;  ///< Result returned by alloc().
    bool rebind_result = true;  ///< Result returned by rebind().
    bool update_result = true;  ///< Result returned by update().

  private:
    std::string name_;  ///< Log name for this fake sink.
    std::vector<std::string> &calls_;  ///< Shared lifecycle call log.
  };

  /**
   * @brief Construct a valid deterministic Sunshine gamepad identifier.
   *
   * @param global_index Sunshine global gamepad slot.
   * @return Controller identifiers for router tests.
   */
  platf::gamepad_id_t id_for(int global_index = 0) {
    return {global_index, 2};
  }
}  // namespace

TEST(GamepadRouterTest, RoutesOnlyTheSelectedOutput) {
  std::vector<std::string> calls;
  auto virtual_sink = std::make_shared<fake_sink_t>("virtual", calls);
  auto nxbt_sink = std::make_shared<fake_sink_t>("nxbt", calls);
  input::gamepad::router_t virtual_router {input::gamepad::output_mode_e::virtual_output, virtual_sink, nxbt_sink};
  ASSERT_TRUE(virtual_router.alloc(id_for(), {}, {}));
  EXPECT_TRUE(virtual_router.update(id_for(), {}));
  virtual_router.neutralize(id_for());
  virtual_router.free(id_for());
  EXPECT_EQ(calls, (std::vector<std::string> {"virtual.alloc", "virtual.update", "virtual.neutralize", "virtual.free"}));

  calls.clear();
  input::gamepad::router_t nxbt_router {input::gamepad::output_mode_e::nxbt, virtual_sink, nxbt_sink};
  ASSERT_TRUE(nxbt_router.alloc(id_for(), {}, {}));
  EXPECT_TRUE(nxbt_router.update(id_for(), {}));
  nxbt_router.neutralize(id_for());
  nxbt_router.free(id_for());
  EXPECT_EQ(calls, (std::vector<std::string> {"nxbt.alloc", "nxbt.update", "nxbt.neutralize", "nxbt.free"}));
}

TEST(GamepadRouterTest, AllocatesBothInOrderAndRollsBackOnSecondFailure) {
  std::vector<std::string> calls;
  auto virtual_sink = std::make_shared<fake_sink_t>("virtual", calls);
  auto nxbt_sink = std::make_shared<fake_sink_t>("nxbt", calls);
  input::gamepad::router_t router {input::gamepad::output_mode_e::both, virtual_sink, nxbt_sink};
  ASSERT_TRUE(router.alloc(id_for(), {}, {}));
  router.neutralize(id_for());
  router.free(id_for());
  EXPECT_EQ(calls, (std::vector<std::string> {"virtual.alloc", "nxbt.alloc", "virtual.neutralize", "nxbt.neutralize", "nxbt.free", "virtual.free"}));

  calls.clear();
  nxbt_sink->alloc_result = false;
  EXPECT_FALSE(router.alloc(id_for(), {}, {}));
  EXPECT_EQ(calls, (std::vector<std::string> {"virtual.alloc", "nxbt.alloc", "virtual.free"}));
}

TEST(GamepadRouterTest, DeliversUpdatesAfterTemporaryFailureAndRebindsSelectedSinks) {
  std::vector<std::string> calls;
  auto virtual_sink = std::make_shared<fake_sink_t>("virtual", calls);
  auto nxbt_sink = std::make_shared<fake_sink_t>("nxbt", calls);
  input::gamepad::router_t router {input::gamepad::output_mode_e::both, virtual_sink, nxbt_sink};
  virtual_sink->update_result = false;
  EXPECT_FALSE(router.update(id_for(), {}));
  EXPECT_EQ(calls, (std::vector<std::string> {"virtual.update", "nxbt.update"}));
  calls.clear();
  nxbt_sink->rebind_result = false;
  EXPECT_FALSE(router.rebind(id_for(), {}));
  EXPECT_EQ(calls, (std::vector<std::string> {"virtual.rebind", "nxbt.rebind"}));
}

TEST(GamepadRouterTest, RejectsEveryOutOfRangeGlobalSlot) {
  std::vector<std::string> calls;
  auto virtual_sink = std::make_shared<fake_sink_t>("virtual", calls);
  input::gamepad::router_t router {input::gamepad::output_mode_e::virtual_output, virtual_sink, {}};
  for (const auto global_index : {-1, platf::MAX_GAMEPADS}) {
    SCOPED_TRACE(global_index);
    EXPECT_FALSE(router.alloc(id_for(global_index), {}, {}));
    EXPECT_FALSE(router.rebind(id_for(global_index), {}));
    EXPECT_FALSE(router.update(id_for(global_index), {}));
    router.neutralize(id_for(global_index));
    router.free(id_for(global_index));
  }
  EXPECT_TRUE(calls.empty());
}

TEST(GamepadRouterTest, AcceptsEverySupportedGlobalSlot) {
  std::vector<std::string> calls;
  auto virtual_sink = std::make_shared<fake_sink_t>("virtual", calls);
  input::gamepad::router_t router {input::gamepad::output_mode_e::virtual_output, virtual_sink, {}};
  for (int global_index = 0; global_index < platf::MAX_GAMEPADS; ++global_index) {
    SCOPED_TRACE(global_index);
    EXPECT_TRUE(router.alloc(id_for(global_index), {}, {}));
    EXPECT_TRUE(router.rebind(id_for(global_index), {}));
    EXPECT_TRUE(router.update(id_for(global_index), {}));
    router.neutralize(id_for(global_index));
    router.free(id_for(global_index));
  }
  EXPECT_EQ(calls.size(), static_cast<std::size_t>(platf::MAX_GAMEPADS * 5));
}

TEST(GamepadRouterTest, RejectsModesWhoseSelectedSinkIsUnavailable) {
  std::vector<std::string> calls;
  auto virtual_sink = std::make_shared<fake_sink_t>("virtual", calls);
  auto nxbt_sink = std::make_shared<fake_sink_t>("nxbt", calls);
  std::vector<input::gamepad::router_t> routers {
    {input::gamepad::output_mode_e::virtual_output, {}, nxbt_sink},
    {input::gamepad::output_mode_e::nxbt, virtual_sink, {}},
    {input::gamepad::output_mode_e::both, virtual_sink, {}},
    {input::gamepad::output_mode_e::both, {}, nxbt_sink},
  };
  for (auto &router : routers) {
    EXPECT_FALSE(router.alloc(id_for(), {}, {}));
    EXPECT_FALSE(router.rebind(id_for(), {}));
    EXPECT_FALSE(router.update(id_for(), {}));
  }
  EXPECT_TRUE(calls.empty());
}

TEST(GamepadRouterTest, RateLimitsRepeatedFailureLogsWithoutSleeping) {
  using namespace std::chrono_literals;
  input::gamepad::failure_log_limiter_t limiter {5s};
  const std::chrono::steady_clock::time_point start {100s};
  EXPECT_TRUE(limiter.should_log(start));
  EXPECT_FALSE(limiter.should_log(start + 4999ms));
  EXPECT_TRUE(limiter.should_log(start + 5s));
  EXPECT_FALSE(limiter.should_log(start + 9s));
  EXPECT_TRUE(limiter.should_log(start + 10s));
}
