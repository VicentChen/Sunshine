/**
 * @file tests/unit/test_nxbt_client.cpp
 * @brief Tests for the reconnecting NXBT client worker and gamepad sink.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "../tests_common.h"
#include "src/input/nxbt_client.h"
#include "src/input/nxbt_mapping.h"
#include "src/input/nxbt_sink.h"

#if defined(__linux__)
  // system includes
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Reply behavior selected for an injected packet transport.
   */
  enum class reply_mode_e {
    normal,  ///< Acknowledge hello and every ping.
    permission_denied,  ///< Reject every connection with a permissions error.
    version_error,  ///< Reject hello with unsupported-version.
    malformed,  ///< Reply to hello with an invalid packet.
    no_reply,  ///< Accept transport operations but never return a packet.
    no_pong_first_connection,  ///< Acknowledge hello but drop pings on connection one.
    bridge_error,  ///< Acknowledge hello then reject the first attachment.
    controller_status,  ///< Acknowledge hello then report a connected controller.
  };

  /**
   * @brief Message record tagged with its injected connection generation.
   */
  struct sent_record_t {
    int connection = 0;  ///< One-based connection generation.
    input::nxbt::message_t message;  ///< Decoded packet sent by the client.
  };

  /**
   * @brief Shared observable behavior for every injected transport generation.
   */
  struct transport_script_t {
    reply_mode_e mode = reply_mode_e::normal;  ///< Selected reply behavior.
    bool allow_hello_ack = true;  ///< Test-controlled handshake gate.
    bool disconnect_first_after_state = false;  ///< Force recovery after the first state.
    bool first_disconnected = false;  ///< Whether the forced disconnect was consumed.
    int connections = 0;  ///< Number of constructed successful connection generations.
    std::mutex mutex;  ///< Protects script state and records.
    std::vector<sent_record_t> sent;  ///< Every valid packet sent by the client.
  };

  /**
   * @brief Injectable bounded packet transport with scripted protocol replies.
   */
  class scripted_transport_t final: public input::nxbt::transport_t {
  public:
    /**
     * @brief Bind a transport generation to shared scripted state.
     *
     * @param script Shared behavior and observations.
     */
    explicit scripted_transport_t(std::shared_ptr<transport_script_t> script):
        script_(std::move(script)) {
    }

    /**
     * @brief Establish or reject one injected connection immediately.
     */
    std::pair<input::nxbt::transport_result_e, std::error_code> connect(const std::string &, std::chrono::milliseconds) override {
      std::lock_guard lock(script_->mutex);
      if (script_->mode == reply_mode_e::permission_denied) {
        return {input::nxbt::transport_result_e::disconnected, std::make_error_code(std::errc::permission_denied)};
      }
      connection_ = ++script_->connections;
      connected_ = true;
      return {input::nxbt::transport_result_e::success, {}};
    }

    /**
     * @brief Record a packet and create its scripted immediate reply.
     */
    std::pair<input::nxbt::transport_result_e, std::error_code> send(const std::vector<std::uint8_t> &packet, std::chrono::milliseconds) override {
      if (!connected_) {
        return {input::nxbt::transport_result_e::disconnected, std::make_error_code(std::errc::not_connected)};
      }
      const auto decoded = input::nxbt::decode_message(packet);
      if (decoded.error != input::nxbt::protocol_error_e::none) {
        return {input::nxbt::transport_result_e::disconnected, std::make_error_code(std::errc::protocol_error)};
      }
      std::lock_guard lock(script_->mutex);
      script_->sent.push_back({connection_, decoded.message});
      if (decoded.message.type == input::nxbt::message_type_e::hello) {
        hello_seen_ = true;
      } else if (decoded.message.type == input::nxbt::message_type_e::attach && script_->mode == reply_mode_e::bridge_error) {
        replies_.push_back(input::nxbt::encode_message({
          .type = input::nxbt::message_type_e::error,
          .error = input::nxbt::protocol_error_e::invalid_length,
        }));
      } else if (decoded.message.type == input::nxbt::message_type_e::attach && script_->mode == reply_mode_e::controller_status) {
        replies_.push_back(input::nxbt::encode_message({
          .type = input::nxbt::message_type_e::status,
          .controller_id = decoded.message.controller_id,
          .status = input::nxbt::controller_status_e::connected,
        }));
      } else if (decoded.message.type == input::nxbt::message_type_e::ping && !(script_->mode == reply_mode_e::no_pong_first_connection && connection_ == 1)) {
        replies_.push_back(input::nxbt::encode_message({
          .type = input::nxbt::message_type_e::pong,
          .monotonic_timestamp_us = decoded.message.monotonic_timestamp_us,
        }));
      }
      return {input::nxbt::transport_result_e::success, {}};
    }

    /**
     * @brief Return a queued reply, scripted disconnect, or bounded timeout.
     */
    input::nxbt::receive_result_t receive(std::chrono::milliseconds timeout) override {
      {
        std::lock_guard lock(script_->mutex);
        if (script_->disconnect_first_after_state && connection_ == 1 && !script_->first_disconnected) {
          const auto sent_state = std::any_of(script_->sent.begin(), script_->sent.end(), [](const sent_record_t &record) {
            return record.connection == 1 && record.message.type == input::nxbt::message_type_e::state;
          });
          if (sent_state) {
            script_->first_disconnected = true;
            connected_ = false;
            return {input::nxbt::transport_result_e::disconnected, {}, std::make_error_code(std::errc::connection_reset)};
          }
        }
        if (hello_seen_ && !hello_replied_ && script_->allow_hello_ack) {
          hello_replied_ = true;
          if (script_->mode == reply_mode_e::version_error) {
            return {input::nxbt::transport_result_e::success, input::nxbt::encode_message({
                                                                .type = input::nxbt::message_type_e::error,
                                                                .error = input::nxbt::protocol_error_e::unsupported_version,
                                                              }),
                    {}};
          }
          if (script_->mode == reply_mode_e::malformed) {
            return {input::nxbt::transport_result_e::success, {0, 1, 2}, {}};
          }
          if (script_->mode != reply_mode_e::no_reply) {
            return {input::nxbt::transport_result_e::success, input::nxbt::encode_message({.type = input::nxbt::message_type_e::hello_ack}), {}};
          }
        }
        if (!replies_.empty()) {
          auto packet = std::move(replies_.front());
          replies_.pop_front();
          return {input::nxbt::transport_result_e::success, std::move(packet), {}};
        }
      }
      std::this_thread::sleep_for(std::min(timeout, 2ms));
      return {};
    }

    /**
     * @brief Mark this injected generation closed.
     */
    void close() override {
      connected_ = false;
    }

  private:
    std::shared_ptr<transport_script_t> script_;  ///< Shared behavior and observations.
    int connection_ = 0;  ///< This transport's connection generation.
    bool connected_ = false;  ///< Whether the injected transport is connected.
    bool hello_seen_ = false;  ///< Whether this generation received hello.
    bool hello_replied_ = false;  ///< Whether this generation completed hello handling.
    std::deque<std::vector<std::uint8_t>> replies_;  ///< Immediate packet replies.
  };

  /**
   * @brief Build a transport factory sharing one script across generations.
   *
   * @param script Shared behavior and observations.
   * @return Injectable client transport factory.
   */
  input::nxbt::transport_factory_t factory_for(const std::shared_ptr<transport_script_t> &script) {
    return [script]() {
      return std::make_unique<scripted_transport_t>(script);
    };
  }

  /**
   * @brief Return short timings suitable for worker unit tests.
   */
  input::nxbt::client_options_t test_options() {
    return {
      .endpoint = "injected",
      .connect_timeout = 5ms,
      .io_timeout = 3ms,
      .handshake_timeout = 50ms,
      .reconnect_delay = 5ms,
      .heartbeat_interval = 15ms,
      .heartbeat_timeout = 30ms,
      .error_log_interval = 500ms,
    };
  }

  /**
   * @brief Poll a predicate until a short test deadline.
   *
   * @tparam Predicate Callable returning whether the observation arrived.
   * @param predicate Condition to poll.
   * @return @c true if the condition became true.
   */
  template<typename Predicate>
  bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(2ms);
    }
    return false;
  }

  /**
   * @brief Copy all recorded messages under the script lock.
   */
  std::vector<sent_record_t> records(const std::shared_ptr<transport_script_t> &script) {
    std::lock_guard lock(script->mutex);
    return script->sent;
  }

  /**
   * @brief Count a message type, optionally within one connection generation.
   */
  std::size_t count_type(const std::vector<sent_record_t> &sent, input::nxbt::message_type_e type, int connection = 0) {
    return static_cast<std::size_t>(std::count_if(sent.begin(), sent.end(), [type, connection](const sent_record_t &record) {
      return record.message.type == type && (connection == 0 || record.connection == connection);
    }));
  }

#if defined(__linux__)
  /**
   * @brief Minimal real Unix packet server used to exercise the production transport.
   */
  class unix_test_server_t {
  public:
    /**
     * @brief Create a temporary endpoint and start the server thread.
     */
    unix_test_server_t() {
      directory_ = std::filesystem::temp_directory_path() /
                   ("sunshine-nxbt-client-" + std::to_string(::getpid()) + "-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directory(directory_);
      path_ = directory_ / "control.sock";
      worker_ = std::thread([this]() {
        serve();
      });
      std::unique_lock lock(mutex_);
      condition_.wait_for(lock, 1s, [this]() {
        return ready_;
      });
    }

    /**
     * @brief Stop the server and remove its exact temporary endpoint directory.
     */
    ~unix_test_server_t() {
      stop_.store(true);
      const auto client_fd = client_fd_.load();
      if (client_fd >= 0) {
        ::shutdown(client_fd, SHUT_RDWR);
      }
      const auto server_fd = server_fd_.load();
      if (server_fd >= 0) {
        ::shutdown(server_fd, SHUT_RDWR);
      }
      if (worker_.joinable()) {
        worker_.join();
      }
      std::error_code ignored;
      std::filesystem::remove_all(directory_, ignored);
    }

    /**
     * @brief Return the temporary socket endpoint path.
     *
     * @return Native temporary socket path.
     */
    std::string path() const {
      return path_.string();
    }

    /**
     * @brief Copy decoded packets received by the server.
     *
     * @return Received protocol messages.
     */
    std::vector<input::nxbt::message_t> messages() const {
      std::lock_guard lock(mutex_);
      return messages_;
    }

  private:
    /**
     * @brief Accept one client and provide hello and pong replies.
     */
    void serve() {
      const auto server_fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
      server_fd_.store(server_fd);
      if (server_fd < 0) {
        signal_ready();
        return;
      }
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto path = path_.string();
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      if (::bind(server_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0 || ::listen(server_fd, 1) < 0) {
        signal_ready();
        return;
      }
      signal_ready();
      const auto client_fd = ::accept(server_fd, nullptr, nullptr);
      client_fd_.store(client_fd);
      std::array<std::uint8_t, 64> buffer {};
      while (!stop_.load() && client_fd >= 0) {
        const auto received = ::recv(client_fd, buffer.data(), buffer.size(), 0);
        if (received <= 0) {
          break;
        }
        const std::vector<std::uint8_t> packet(buffer.begin(), buffer.begin() + received);
        const auto decoded = input::nxbt::decode_message(packet);
        if (decoded.error != input::nxbt::protocol_error_e::none) {
          continue;
        }
        {
          std::lock_guard lock(mutex_);
          messages_.push_back(decoded.message);
        }
        if (decoded.message.type == input::nxbt::message_type_e::hello) {
          send(input::nxbt::message_t {.type = input::nxbt::message_type_e::hello_ack});
        } else if (decoded.message.type == input::nxbt::message_type_e::ping) {
          send(input::nxbt::message_t {
            .type = input::nxbt::message_type_e::pong,
            .monotonic_timestamp_us = decoded.message.monotonic_timestamp_us,
          });
        }
      }
      const auto owned_client_fd = client_fd_.exchange(-1);
      if (owned_client_fd >= 0) {
        ::close(owned_client_fd);
      }
      const auto owned_server_fd = server_fd_.exchange(-1);
      if (owned_server_fd >= 0) {
        ::close(owned_server_fd);
      }
    }

    /**
     * @brief Send one encoded reply to the connected test client.
     *
     * @param message Reply message.
     */
    void send(const input::nxbt::message_t &message) const {
      const auto packet = input::nxbt::encode_message(message);
      static_cast<void>(::send(client_fd_.load(), packet.data(), packet.size(), MSG_NOSIGNAL));
    }

    /**
     * @brief Publish that socket creation has completed.
     */
    void signal_ready() {
      std::lock_guard lock(mutex_);
      ready_ = true;
      condition_.notify_all();
    }

    std::filesystem::path directory_;  ///< Exact owned temporary directory.
    std::filesystem::path path_;  ///< Temporary socket path.
    mutable std::mutex mutex_;  ///< Protects readiness and received messages.
    std::condition_variable condition_;  ///< Announces socket readiness.
    bool ready_ = false;  ///< Whether bind/listen has completed or failed.
    std::atomic<bool> stop_ {false};  ///< Requests server shutdown.
    std::thread worker_;  ///< Server accept and receive worker.
    std::atomic<int> server_fd_ {-1};  ///< Owned listening socket descriptor.
    std::atomic<int> client_fd_ {-1};  ///< Owned accepted socket descriptor.
    std::vector<input::nxbt::message_t> messages_;  ///< Decoded received packets.
  };
#endif
}  // namespace

TEST(NxbtClientTest, NegotiatesAndPreservesControllerLifecycleOrder) {
  auto script = std::make_shared<transport_script_t>();
  script->allow_hello_ack = false;
  input::nxbt::client_t client {test_options(), factory_for(script)};
  ASSERT_TRUE(client.attach(3, 2));
  input::nxbt::controller_state_t state {.controller_id = 3, .button_flags = input::nxbt::nxbt_a, .sequence = 7, .monotonic_timestamp_us = 8};
  ASSERT_TRUE(client.update(state));
  {
    std::lock_guard lock(script->mutex);
    script->allow_hello_ack = true;
  }
  ASSERT_TRUE(wait_until([&]() {
    return count_type(records(script), input::nxbt::message_type_e::state) == 1;
  }));
  client.neutralize(3);
  client.detach(3);
  ASSERT_TRUE(wait_until([&]() {
    return count_type(records(script), input::nxbt::message_type_e::detach) == 1;
  }));

  const auto sent = records(script);
  std::vector<input::nxbt::message_type_e> controller_messages;
  for (const auto &record : sent) {
    if (record.message.type != input::nxbt::message_type_e::hello && record.message.type != input::nxbt::message_type_e::ping) {
      controller_messages.push_back(record.message.type);
    }
  }
  EXPECT_EQ(controller_messages, (std::vector<input::nxbt::message_type_e> {
                                   input::nxbt::message_type_e::attach,
                                   input::nxbt::message_type_e::neutralize,
                                   input::nxbt::message_type_e::state,
                                   input::nxbt::message_type_e::neutralize,
                                   input::nxbt::message_type_e::neutralize,
                                   input::nxbt::message_type_e::detach,
                                 }));
}

TEST(NxbtClientTest, MissingSocketNeverBlocksInputOrBuildsAStateBacklog) {
  auto options = test_options();
  options.endpoint = "/tmp/sunshine-nxbt-definitely-missing/control.sock";
  std::atomic<int> failures {0};
  const auto started = std::chrono::steady_clock::now();
  input::nxbt::client_t client {options, input::nxbt::make_unix_transport_factory(), [&failures](const input::nxbt::client_event_t &event) {
                                  if (event.type == input::nxbt::client_event_e::connect_failed) {
                                    ++failures;
                                  }
                                }};
  ASSERT_TRUE(client.attach(0, 0));
  for (std::uint32_t sequence = 1; sequence <= 1000; ++sequence) {
    ASSERT_TRUE(client.update({.controller_id = 0, .sequence = sequence, .monotonic_timestamp_us = sequence}));
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, 100ms);
  EXPECT_EQ(client.pending_state_count(), 1U);
  EXPECT_TRUE(wait_until([&failures]() {
    return failures.load() == 1;
  }));
}

TEST(NxbtClientTest, ReportsPermissionVersionAndMalformedFailuresWithRateLimiting) {
  for (const auto mode : {reply_mode_e::permission_denied, reply_mode_e::version_error, reply_mode_e::malformed}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto script = std::make_shared<transport_script_t>();
    script->mode = mode;
    std::mutex event_mutex;
    std::vector<input::nxbt::client_event_t> events;
    {
      input::nxbt::client_t client {test_options(), factory_for(script), [&](const input::nxbt::client_event_t &event) {
                                      std::lock_guard lock(event_mutex);
                                      events.push_back(event);
                                    }};
      ASSERT_TRUE(wait_until([&]() {
        std::lock_guard lock(event_mutex);
        return !events.empty();
      }));
      std::this_thread::sleep_for(40ms);
    }
    std::lock_guard lock(event_mutex);
    ASSERT_EQ(events.size(), 1U);
    if (mode == reply_mode_e::permission_denied) {
      EXPECT_EQ(events.front().type, input::nxbt::client_event_e::connect_failed);
      EXPECT_EQ(events.front().system_error, std::make_error_code(std::errc::permission_denied));
    } else if (mode == reply_mode_e::version_error) {
      EXPECT_EQ(events.front().type, input::nxbt::client_event_e::handshake_failed);
      EXPECT_EQ(events.front().protocol_error, input::nxbt::protocol_error_e::unsupported_version);
    } else {
      EXPECT_EQ(events.front().type, input::nxbt::client_event_e::malformed_reply);
      EXPECT_EQ(events.front().protocol_error, input::nxbt::protocol_error_e::truncated);
    }
  }
}

TEST(NxbtClientTest, ReportsBridgeErrorsAndControllerStatus) {
  for (const auto mode : {reply_mode_e::bridge_error, reply_mode_e::controller_status}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto script = std::make_shared<transport_script_t>();
    script->mode = mode;
    std::mutex event_mutex;
    std::vector<input::nxbt::client_event_t> events;
    input::nxbt::client_t client {test_options(), factory_for(script), [&](const input::nxbt::client_event_t &event) {
                                    std::lock_guard lock(event_mutex);
                                    events.push_back(event);
                                  }};
    ASSERT_TRUE(client.attach(6, 1));
    ASSERT_TRUE(wait_until([&]() {
      std::lock_guard lock(event_mutex);
      return std::any_of(events.begin(), events.end(), [mode](const input::nxbt::client_event_t &event) {
        return event.type == (mode == reply_mode_e::bridge_error ? input::nxbt::client_event_e::bridge_error : input::nxbt::client_event_e::controller_status);
      });
    }));
    std::lock_guard lock(event_mutex);
    const auto expected = mode == reply_mode_e::bridge_error ? input::nxbt::client_event_e::bridge_error : input::nxbt::client_event_e::controller_status;
    const auto event = std::find_if(events.begin(), events.end(), [expected](const input::nxbt::client_event_t &candidate) {
      return candidate.type == expected;
    });
    ASSERT_NE(event, events.end());
    if (mode == reply_mode_e::bridge_error) {
      EXPECT_EQ(event->protocol_error, input::nxbt::protocol_error_e::invalid_length);
    } else {
      EXPECT_EQ(event->controller_id, 6);
      EXPECT_EQ(event->controller_status, input::nxbt::controller_status_e::connected);
    }
  }
}

TEST(NxbtClientTest, RejectsInvalidOrContradictoryLogicalOperations) {
  auto script = std::make_shared<transport_script_t>();
  input::nxbt::client_t client {test_options(), factory_for(script)};
  EXPECT_FALSE(client.attach(platf::MAX_GAMEPADS, 0));
  EXPECT_FALSE(client.rebind(1, 0));
  EXPECT_FALSE(client.rebind(platf::MAX_GAMEPADS, 0));
  EXPECT_FALSE(client.update({.controller_id = 1}));
  EXPECT_FALSE(client.update({.controller_id = platf::MAX_GAMEPADS}));
  client.neutralize(1);
  client.neutralize(platf::MAX_GAMEPADS);
  client.detach(1);
  client.detach(platf::MAX_GAMEPADS);
  EXPECT_TRUE(client.attach(1, 0));
  EXPECT_FALSE(client.attach(1, 0));
}

#if defined(__linux__)
TEST(NxbtClientTest, ProductionUnixTransportExchangesPacketBoundaries) {
  unix_test_server_t server;
  auto options = test_options();
  options.endpoint = server.path();
  input::nxbt::client_t client {options, input::nxbt::make_unix_transport_factory()};
  ASSERT_TRUE(client.attach(4, 2));
  ASSERT_TRUE(client.update({.controller_id = 4, .button_flags = input::nxbt::nxbt_home, .sequence = 1, .monotonic_timestamp_us = 2}));
  ASSERT_TRUE(wait_until([&]() {
    const auto messages = server.messages();
    return std::any_of(messages.begin(), messages.end(), [](const input::nxbt::message_t &message) {
      return message.type == input::nxbt::message_type_e::state;
    });
  }));
  client.detach(4);
  ASSERT_TRUE(wait_until([&]() {
    const auto messages = server.messages();
    return std::any_of(messages.begin(), messages.end(), [](const input::nxbt::message_t &message) {
      return message.type == input::nxbt::message_type_e::detach;
    });
  }));
}
#endif

TEST(NxbtClientTest, CoalescesBurstAndReplaysNeutralAttachAndLatestStateAfterRestart) {
  auto script = std::make_shared<transport_script_t>();
  script->allow_hello_ack = false;
  script->disconnect_first_after_state = true;
  input::nxbt::client_t client {test_options(), factory_for(script)};
  ASSERT_TRUE(client.attach(1, 4));
  for (std::uint32_t sequence = 1; sequence <= 1000; ++sequence) {
    ASSERT_TRUE(client.update({.controller_id = 1, .button_flags = sequence, .sequence = sequence, .monotonic_timestamp_us = sequence}));
  }
  EXPECT_EQ(client.pending_state_count(), 1U);
  {
    std::lock_guard lock(script->mutex);
    script->allow_hello_ack = true;
  }
  ASSERT_TRUE(wait_until([&]() {
    const auto sent = records(script);
    return count_type(sent, input::nxbt::message_type_e::state, 2) == 1;
  }));
  const auto sent = records(script);
  std::vector<input::nxbt::message_type_e> second_generation;
  for (const auto &record : sent) {
    if (record.connection == 2) {
      second_generation.push_back(record.message.type);
    }
  }
  ASSERT_GE(second_generation.size(), 4U);
  EXPECT_EQ(second_generation[0], input::nxbt::message_type_e::hello);
  EXPECT_EQ(second_generation[1], input::nxbt::message_type_e::attach);
  EXPECT_EQ(second_generation[2], input::nxbt::message_type_e::neutralize);
  EXPECT_EQ(second_generation[3], input::nxbt::message_type_e::state);
  const auto final_state = std::find_if(sent.rbegin(), sent.rend(), [](const sent_record_t &record) {
    return record.connection == 2 && record.message.type == input::nxbt::message_type_e::state;
  });
  ASSERT_NE(final_state, sent.rend());
  EXPECT_EQ(final_state->message.state.sequence, 1000U);
}

TEST(NxbtClientTest, HeartbeatTimeoutReconnectsAndPongKeepsRecoveryAlive) {
  auto script = std::make_shared<transport_script_t>();
  script->mode = reply_mode_e::no_pong_first_connection;
  std::atomic<int> connected {0};
  std::atomic<int> heartbeat_timeouts {0};
  input::nxbt::client_t client {test_options(), factory_for(script), [&](const input::nxbt::client_event_t &event) {
                                  if (event.type == input::nxbt::client_event_e::connected) {
                                    ++connected;
                                  } else if (event.type == input::nxbt::client_event_e::heartbeat_timeout) {
                                    ++heartbeat_timeouts;
                                  }
                                }};
  ASSERT_TRUE(wait_until([&]() {
    return heartbeat_timeouts.load() == 1 && connected.load() >= 2;
  }));
  std::this_thread::sleep_for(60ms);
  EXPECT_EQ(heartbeat_timeouts.load(), 1);
  EXPECT_GE(count_type(records(script), input::nxbt::message_type_e::ping, 2), 1U);
}

TEST(NxbtClientTest, DestructionInterruptsAnUnresponsiveHandshakeWithinItsIoBound) {
  auto script = std::make_shared<transport_script_t>();
  script->mode = reply_mode_e::no_reply;
  auto options = test_options();
  options.handshake_timeout = 5s;
  const auto started = std::chrono::steady_clock::now();
  {
    input::nxbt::client_t client {options, factory_for(script)};
    ASSERT_TRUE(wait_until([&]() {
      return count_type(records(script), input::nxbt::message_type_e::hello) == 1;
    }));
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, 200ms);
}

TEST(NxbtSinkTest, MapsStateAndResetsTriggerHysteresisAcrossNeutralize) {
  auto script = std::make_shared<transport_script_t>();
  script->allow_hello_ack = false;
  auto client = std::make_shared<input::nxbt::client_t>(test_options(), factory_for(script));
  input::nxbt::sink_t sink {client, input::nxbt::face_button_policy_e::positions};
  const platf::gamepad_id_t id {2, 5};
  ASSERT_TRUE(sink.alloc(id, {}, {}));
  ASSERT_TRUE(sink.update(id, {platf::A | platf::HOME, 64, 0, -32768, 32767, 12, -13}));
  sink.neutralize(id);
  ASSERT_TRUE(sink.update(id, {platf::B, 49, 255, 1, 2, 3, 4}));
  {
    std::lock_guard lock(script->mutex);
    script->allow_hello_ack = true;
  }
  ASSERT_TRUE(wait_until([&]() {
    return count_type(records(script), input::nxbt::message_type_e::state) == 1;
  }));
  const auto sent = records(script);
  const auto state = std::find_if(sent.begin(), sent.end(), [](const sent_record_t &record) {
    return record.message.type == input::nxbt::message_type_e::state;
  });
  ASSERT_NE(state, sent.end());
  EXPECT_EQ(state->message.state.controller_id, 2);
  EXPECT_EQ(state->message.state.button_flags, input::nxbt::nxbt_a | input::nxbt::nxbt_zr);
  EXPECT_EQ(state->message.state.left_trigger, 49);
  EXPECT_EQ(state->message.state.right_trigger, 255);
  EXPECT_EQ(state->message.state.left_stick_x, 1);
  EXPECT_EQ(state->message.state.left_stick_y, 2);
  EXPECT_EQ(state->message.state.right_stick_x, 3);
  EXPECT_EQ(state->message.state.right_stick_y, 4);
  EXPECT_EQ(state->message.state.sequence, 2U);
  EXPECT_GT(state->message.state.monotonic_timestamp_us, 0U);
  EXPECT_TRUE(sink.rebind({2, 6}, {}));
  sink.free({2, 6});
  EXPECT_FALSE(sink.update(id, {}));
  EXPECT_FALSE(sink.alloc({-1, 0}, {}, {}));
  EXPECT_FALSE(sink.rebind({platf::MAX_GAMEPADS, 0}, {}));
}

TEST(NxbtSinkTest, RoutesOneSunshineGamepadToConfiguredFixedSlotAndThresholds) {
  auto script = std::make_shared<transport_script_t>();
  script->allow_hello_ack = false;
  auto client = std::make_shared<input::nxbt::client_t>(test_options(), factory_for(script));
  input::nxbt::sink_t sink {client, input::nxbt::face_button_policy_e::labels, 7, 90, 70};
  ASSERT_TRUE(sink.alloc({3, 1}, {}, {}));
  EXPECT_FALSE(sink.alloc({4, 2}, {}, {}));
  ASSERT_TRUE(sink.update({3, 1}, {platf::A, 89, 90, 0, 0, 0, 0}));
  {
    std::lock_guard lock(script->mutex);
    script->allow_hello_ack = true;
  }
  ASSERT_TRUE(wait_until([&]() {
    return count_type(records(script), input::nxbt::message_type_e::state) == 1;
  }));
  const auto sent = records(script);
  const auto state = std::find_if(sent.begin(), sent.end(), [](const sent_record_t &record) {
    return record.message.type == input::nxbt::message_type_e::state;
  });
  ASSERT_NE(state, sent.end());
  EXPECT_EQ(state->message.state.controller_id, 7);
  EXPECT_EQ(state->message.state.button_flags, input::nxbt::nxbt_a | input::nxbt::nxbt_zr);
  EXPECT_TRUE(sink.rebind({3, 6}, {}));
  sink.neutralize({3, 6});
  sink.free({3, 6});
  EXPECT_TRUE(sink.alloc({4, 2}, {}, {}));
}

TEST(NxbtClientTest, ExposesSanitizedConnectionAndControllerDiagnostics) {
  auto script = std::make_shared<transport_script_t>();
  script->mode = reply_mode_e::controller_status;
  auto client = std::make_shared<input::nxbt::client_t>(test_options(), factory_for(script));
  ASSERT_TRUE(client->attach(2, 1));
  ASSERT_TRUE(wait_until([&]() {
    const auto diagnostics = client->diagnostics();
    return diagnostics.socket_connected && diagnostics.controller_statuses[2] == input::nxbt::controller_status_e::connected;
  }));
  const auto diagnostics = client->diagnostics();
  EXPECT_EQ(diagnostics.endpoint, "injected");
  EXPECT_EQ(diagnostics.negotiated_protocol_version, input::nxbt::protocol_version);
  EXPECT_TRUE(diagnostics.heartbeat_healthy);
  EXPECT_FALSE(diagnostics.has_last_error);
  EXPECT_EQ(input::nxbt::client_event_name(input::nxbt::client_event_e::heartbeat_timeout), "heartbeat_timeout");
  EXPECT_EQ(input::nxbt::controller_status_name(input::nxbt::controller_status_e::reconnecting), "reconnecting");
}
