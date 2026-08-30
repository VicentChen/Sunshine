/**
 * @file tests/unit/test_xbox_remote_sink.cpp
 * @brief Tests for Sunshine Xbox Remote Play gamepad mapping and lifecycle.
 */

// standard includes
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// local includes
#include "../tests_common.h"
#include "src/globals.h"
#include "src/input/xbox_remote_sink.h"

namespace {
  using input::xbox_remote::session_t;
  namespace protocol = ::xbox_remote::protocol;

  /**
   * @brief Observable non-blocking session used by Xbox sink tests.
   */
  class fake_session_t final: public session_t {
  public:
    /**
     * @brief Install or remove the vibration callback.
     *
     * @param handler New callback.
     */
    void set_vibration_handler(vibration_handler_t handler) override {
      if (throw_set_handler) {
        throw std::runtime_error("handler");
      }
      vibration_handler = std::move(handler);
      calls.push_back(vibration_handler ? "handler.set" : "handler.clear");
    }

    /**
     * @brief Record attachment.
     *
     * @param frame Initial state.
     * @return Configured result.
     */
    bool attach(const protocol::gamepad_frame_t &frame) override {
      calls.push_back("attach");
      frames.push_back(frame);
      maybe_throw("attach");
      return attach_result;
    }

    /**
     * @brief Record feedback rebinding.
     *
     * @return Configured result.
     */
    bool rebind() override {
      calls.push_back("rebind");
      maybe_throw("rebind");
      if (operation_hook) {
        operation_hook();
      }
      return rebind_result;
    }

    /**
     * @brief Record a complete state.
     *
     * @param frame Submitted state.
     * @return Configured result.
     */
    bool submit(const protocol::gamepad_frame_t &frame) override {
      calls.push_back("submit");
      frames.push_back(frame);
      maybe_throw("submit");
      return submit_result;
    }

    /**
     * @brief Record neutralization.
     *
     * @return Configured result.
     */
    bool neutralize() override {
      calls.push_back("neutralize");
      maybe_throw("neutralize");
      return neutralize_result;
    }

    /**
     * @brief Record detachment.
     */
    void detach() override {
      calls.push_back("detach");
      maybe_throw("detach");
    }

    /**
     * @brief Emit a parsed vibration command to the sink.
     *
     * @param vibration Vibration command.
     */
    void emit(const protocol::vibration_t &vibration) const {
      if (vibration_handler) {
        vibration_handler(vibration);
      }
    }

    /**
     * @brief Throw when the configured operation matches.
     *
     * @param operation Current operation name.
     */
    void maybe_throw(const std::string &operation) const {
      if (throw_operation == operation) {
        throw std::runtime_error(operation);
      }
    }

    vibration_handler_t vibration_handler;  ///< Installed sink callback.
    std::vector<std::string> calls;  ///< Ordered session operations.
    std::vector<protocol::gamepad_frame_t> frames;  ///< Attached and submitted states.
    std::string throw_operation;  ///< Operation that should throw.
    bool attach_result = true;  ///< Attachment result.
    bool rebind_result = true;  ///< Rebind result.
    bool submit_result = true;  ///< Submission result.
    bool neutralize_result = true;  ///< Neutralization result.
    bool throw_set_handler = false;  ///< Whether handler registration throws.
    std::function<void()> operation_hook;  ///< Optional deterministic lifecycle-race hook.
  };

  /**
   * @brief Create an isolated feedback queue.
   *
   * @param name Queue name.
   * @return Test feedback queue.
   */
  platf::feedback_queue_t feedback_queue(std::string_view name) {
    static auto mailbox = std::make_shared<safe::mail_raw_t>();
    return mailbox->queue<platf::gamepad_feedback_msg_t>(name);
  }

  /**
   * @brief Return all supported Sunshine buttons for mapping coverage.
   *
   * @return Combined button mask.
   */
  std::uint32_t every_mapped_button() {
    return platf::DPAD_UP | platf::DPAD_DOWN | platf::DPAD_LEFT | platf::DPAD_RIGHT |
           platf::START | platf::BACK | platf::LEFT_STICK | platf::RIGHT_STICK |
           platf::LEFT_BUTTON | platf::RIGHT_BUTTON | platf::HOME | platf::A |
           platf::B | platf::X | platf::Y | platf::MISC_BUTTON;
  }
}  // namespace

TEST(XboxRemoteSinkTest, MapsCompleteSunshineStateWithoutChangingAxisSigns) {
  const platf::gamepad_state_t state {
    every_mapped_button(),
    1,
    255,
    -32768,
    32767,
    -1234,
    5678,
  };
  const auto frame = input::xbox_remote::map_state(state);
  EXPECT_EQ(frame.gamepad_index, 0);
  EXPECT_EQ(frame.button_mask, 0xFFFE);
  EXPECT_EQ(frame.left_stick_x, -32768);
  EXPECT_EQ(frame.left_stick_y, 32767);
  EXPECT_EQ(frame.right_stick_x, -1234);
  EXPECT_EQ(frame.right_stick_y, 5678);
  EXPECT_EQ(frame.left_trigger, 257);
  EXPECT_EQ(frame.right_trigger, 65535);
  EXPECT_EQ(frame.physical_physicality, 0x003FFFFF);
  EXPECT_EQ(frame.virtual_physicality, 0);

  const auto neutral = input::xbox_remote::map_state({});
  EXPECT_EQ(neutral.button_mask, 0);
  EXPECT_EQ(neutral.physical_physicality, 0);
}

TEST(XboxRemoteSinkTest, RoutesLifecycleAndRejectsASecondLogicalController) {
  auto session = std::make_shared<fake_session_t>();
  const auto queue = feedback_queue("xbox-sink-lifecycle");
  input::xbox_remote::sink_t sink {session};
  const platf::gamepad_id_t owner {3, 2};
  ASSERT_TRUE(sink.alloc(owner, {}, queue));
  EXPECT_FALSE(sink.alloc({4, 1}, {}, queue));
  EXPECT_FALSE(sink.update({4, 1}, {}));

  const platf::gamepad_state_t state {platf::A, 0, 0, 10, 20, 30, 40};
  EXPECT_TRUE(sink.update(owner, state));
  EXPECT_TRUE(sink.rebind({3, 7}, queue));
  sink.neutralize(owner);
  sink.free(owner);
  sink.neutralize(owner);
  sink.free(owner);
  EXPECT_EQ(session->calls, (std::vector<std::string> {
                              "handler.set",
                              "attach",
                              "submit",
                              "rebind",
                              "neutralize",
                              "detach",
                            }));
  ASSERT_EQ(session->frames.size(), 2);
  EXPECT_EQ(session->frames.front().button_mask, 0);
  EXPECT_EQ(session->frames.back().button_mask, static_cast<std::uint16_t>(protocol::gamepad_button_e::a));
}

TEST(XboxRemoteSinkTest, DeliversFourMotorFeedbackToTheLatestMoonlightBinding) {
  auto session = std::make_shared<fake_session_t>();
  auto first = feedback_queue("xbox-sink-feedback-first");
  auto resumed = feedback_queue("xbox-sink-feedback-resumed");
  input::xbox_remote::sink_t sink {session};
  ASSERT_TRUE(sink.alloc({0, 2}, {}, first));
  ASSERT_TRUE(sink.rebind({0, 9}, resumed));

  session->emit({0, 100, 50, 1, 0, 200, 0, 0});
  EXPECT_FALSE(first->pop(std::chrono::milliseconds {0}));
  auto ordinary = resumed->pop(std::chrono::milliseconds {10});
  auto triggers = resumed->pop(std::chrono::milliseconds {10});
  ASSERT_TRUE(ordinary);
  ASSERT_TRUE(triggers);
  EXPECT_EQ(ordinary->type, platf::gamepad_feedback_e::rumble);
  EXPECT_EQ(ordinary->id, 9);
  EXPECT_EQ(ordinary->data.rumble.lowfreq, 65535);
  EXPECT_EQ(ordinary->data.rumble.highfreq, 32768);
  EXPECT_EQ(triggers->type, platf::gamepad_feedback_e::rumble_triggers);
  EXPECT_EQ(triggers->id, 9);
  EXPECT_EQ(triggers->data.rumble_triggers.left_trigger, 655);
  EXPECT_EQ(triggers->data.rumble_triggers.right_trigger, 0);

  session->emit({1, 100, 100, 100, 100, 0, 0, 0});
  EXPECT_FALSE(resumed->pop(std::chrono::milliseconds {0}));
  session->emit({0, 255, 255, 255, 255, 0, 0, 0});
  ordinary = resumed->pop(std::chrono::milliseconds {10});
  triggers = resumed->pop(std::chrono::milliseconds {10});
  ASSERT_TRUE(ordinary);
  ASSERT_TRUE(triggers);
  EXPECT_EQ(ordinary->data.rumble.lowfreq, 65535);
  EXPECT_EQ(triggers->data.rumble_triggers.left_trigger, 65535);
  sink.free({0, 9});
  session->emit({0, 100, 100, 100, 100, 0, 0, 0});
  EXPECT_FALSE(resumed->pop(std::chrono::milliseconds {0}));
}

TEST(XboxRemoteSinkTest, IsolatesUnavailableAndThrowingSessionOperations) {
  const auto queue = feedback_queue("xbox-sink-failures");
  input::xbox_remote::sink_t missing {nullptr};
  EXPECT_FALSE(missing.alloc({0, 0}, {}, queue));
  EXPECT_FALSE(missing.update({0, 0}, {}));
  missing.free({0, 0});

  auto session = std::make_shared<fake_session_t>();
  input::xbox_remote::sink_t sink {session};
  session->attach_result = false;
  EXPECT_FALSE(sink.alloc({0, 0}, {}, queue));
  session->attach_result = true;
  session->throw_operation = "attach";
  EXPECT_FALSE(sink.alloc({0, 0}, {}, queue));
  session->throw_operation.clear();
  ASSERT_TRUE(sink.alloc({0, 0}, {}, queue));

  session->submit_result = false;
  EXPECT_FALSE(sink.update({0, 0}, {}));
  session->throw_operation = "submit";
  EXPECT_FALSE(sink.update({0, 0}, {}));
  session->throw_operation.clear();
  session->rebind_result = false;
  EXPECT_FALSE(sink.rebind({0, 1}, queue));
  session->rebind_result = true;
  session->throw_operation = "rebind";
  EXPECT_FALSE(sink.rebind({0, 1}, queue));
  EXPECT_FALSE(sink.rebind({0, 1}, {}));
  session->throw_operation.clear();
  session->operation_hook = [&sink]() {
    sink.free({0, 0});
  };
  EXPECT_FALSE(sink.rebind({0, 1}, queue));
  session->operation_hook = {};
  ASSERT_TRUE(sink.alloc({0, 0}, {}, queue));
  session->throw_operation = "neutralize";
  sink.neutralize({0, 0});
  session->throw_operation = "detach";
  sink.free({0, 0});

  EXPECT_FALSE(sink.alloc({-1, 0}, {}, queue));
  EXPECT_FALSE(sink.alloc({platf::MAX_GAMEPADS, 0}, {}, queue));
  EXPECT_FALSE(sink.alloc({0, 0}, {}, {}));
}

TEST(XboxRemoteSinkTest, ClearsCallbackEvenWhenSessionRejectsCleanup) {
  auto session = std::make_shared<fake_session_t>();
  {
    input::xbox_remote::sink_t sink {session};
    session->throw_set_handler = true;
  }
  EXPECT_TRUE(session->vibration_handler);
  session->emit({0, 100, 100, 100, 100, 0, 0, 0});
}
