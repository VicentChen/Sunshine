/**
 * @file tests/unit/test_xbox_remote_session.cpp
 * @brief Offline tests for Xbox Home discovery and REST session lifecycle.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// library includes
#include <nlohmann/json.hpp>

// local includes
#include "../tests_common.h"
#include "src/xbox_remote/session.h"

namespace {
  using namespace std::chrono_literals;
  using xbox_remote::auth::http_response_t;

  /**
   * @brief Fake HTTP method used by scripted responses.
   */
  enum class method_e {
    get,  ///< HTTP GET.
    post,  ///< HTTP POST.
    remove,  ///< HTTP DELETE.
  };

  /**
   * @brief One captured REST request.
   */
  struct request_t {
    method_e method = method_e::get;  ///< HTTP method.
    std::string url;  ///< Requested URL.
    std::string body;  ///< Submitted body.
    std::map<std::string, std::string> headers;  ///< Submitted headers.
    std::chrono::seconds timeout {};  ///< Submitted deadline.
  };

  /**
   * @brief One expected method and response.
   */
  struct scripted_response_t {
    method_e method = method_e::get;  ///< Expected method.
    http_response_t response;  ///< Response to return.
  };

  /**
   * @brief FIFO fake supporting GET, POST, and DELETE.
   */
  class fake_http_t: public xbox_remote::auth::http_client_t {
  public:
    /**
     * @brief Capture an HTTP POST.
     */
    http_response_t post(
      std::string_view url,
      std::string_view body,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override {
      return next({method_e::post, std::string {url}, std::string {body}, headers, timeout});
    }

    /**
     * @brief Capture an HTTP GET.
     */
    http_response_t get(
      std::string_view url,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override {
      return next({method_e::get, std::string {url}, {}, headers, timeout});
    }

    /**
     * @brief Capture an HTTP DELETE.
     */
    http_response_t remove(
      std::string_view url,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override {
      return next({method_e::remove, std::string {url}, {}, headers, timeout});
    }

    /**
     * @brief Return the next scripted response.
     *
     * @param request Captured request.
     * @return Scripted response or a transport failure when exhausted.
     */
    http_response_t next(request_t request) {
      requests.push_back(std::move(request));
      if (responses.empty()) {
        return {0, {}, true};
      }
      EXPECT_EQ(responses.front().method, requests.back().method);
      auto response = std::move(responses.front().response);
      responses.pop_front();
      return response;
    }

    std::deque<scripted_response_t> responses;  ///< Scripted responses.
    std::vector<request_t> requests;  ///< Captured requests.
  };

  /**
   * @brief Deterministic clocks and waits for REST tests.
   */
  class fake_runtime_t: public xbox_remote::auth::runtime_t {
  public:
    /**
     * @brief Return fake wall time.
     */
    std::chrono::system_clock::time_point system_now() const override {
      return system_time;
    }

    /**
     * @brief Return fake monotonic time.
     */
    std::chrono::steady_clock::time_point steady_now() const override {
      return steady_time;
    }

    /**
     * @brief Advance clocks unless cancelled.
     */
    bool wait_for(std::chrono::seconds duration, const std::function<bool()> &cancelled) override {
      waits.push_back(duration);
      if (cancelled && cancelled()) {
        return false;
      }
      system_time += duration;
      steady_time += duration;
      return true;
    }

    std::chrono::system_clock::time_point system_time {1700000000s};  ///< Fake wall time.
    std::chrono::steady_clock::time_point steady_time {100s};  ///< Fake monotonic time.
    std::vector<std::chrono::seconds> waits;  ///< Requested waits.
  };

  /**
   * @brief Construct a usable in-memory Xbox session context.
   *
   * @return Test context.
   */
  xbox_remote::auth::session_context_t context() {
    return {
      "user-hash-secret-fixture",
      "web-token-secret-fixture",
      "gs-token-secret-fixture",
      "https://gssv.example/",
      std::chrono::system_clock::time_point {1700003600s},
    };
  }

  /**
   * @brief Return a valid configuration response.
   *
   * @param keepalive Service keepalive interval.
   * @return Compact JSON response.
   */
  std::string configuration(std::uint32_t keepalive = 3) {
    return "{\"serverDetails\":{\"ipV4Address\":\"192.0.2.10\",\"ipV4Port\":9002,\"iceExchangePath\":\"/ice/path\",\"srtp\":{\"key\":\"fixture-key\"}},\"keepAlivePulseInSeconds\":" + std::to_string(keepalive) + "}";
  }

  /**
   * @brief Combine caller-visible error fields for secret checks.
   *
   * @param error Session failure.
   * @return Caller-visible text.
   */
  std::string error_text(const xbox_remote::session::failure_t &error) {
    return error.stage + " " + error.message;
  }
}  // namespace

TEST(XboxRemoteSessionTest, DiscoversAndSelectsConfiguredOrUniqueConsole) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"On"},{"serverId":"console-b","deviceName":"Desk","consoleType":"XboxSeriesS","powerState":"ConnectedStandby"}]})"}});
  client_t client {http, runtime, auth};

  const auto consoles = client.discover({});
  ASSERT_TRUE(consoles);
  ASSERT_EQ(consoles.value.size(), 2);
  EXPECT_EQ(http.requests.front().url, "https://gssv.example/v6/servers/home?mr=50");
  EXPECT_EQ(http.requests.front().headers.at("Authorization"), "Bearer gs-token-secret-fixture");
  EXPECT_NE(http.requests.front().headers.at("X-MS-Device-Info").find("www.xbox.com"), std::string::npos);
  EXPECT_EQ(http.requests.front().timeout, 15s);

  const auto selected = client.select_console(consoles.value, "console-b");
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected.value.device_name, "Desk");
  EXPECT_EQ(client.select_console(consoles.value, "missing").error.code, error_e::not_found);

  const std::vector unique {consoles.value.front()};
  const auto automatic = client.select_console(unique, "");
  ASSERT_TRUE(automatic);
  EXPECT_EQ(automatic.value.device_name, "Living Room");
  EXPECT_EQ(client.select_console({}, "").error.code, error_e::not_found);
  EXPECT_EQ(client.select_console(consoles.value, "").error.code, error_e::ambiguous_console);

  auto duplicate = consoles.value;
  duplicate.push_back(consoles.value.front());
  EXPECT_EQ(client.select_console(duplicate, "console-a").error.code, error_e::duplicate_console);
}

TEST(XboxRemoteSessionTest, ValidatesWakePreconditionsAndSkipsAConsoleAlreadyOn) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  client_t client {http, runtime, auth};
  wake_options_t options {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s};
  xbox_remote::protocol::console_t console {"console-a", "Living Room", "XboxSeriesX", "On"};

  const auto already_on = client.wake_and_wait(console, options, {});
  ASSERT_TRUE(already_on);
  EXPECT_EQ(already_on.value.power_state, "On");
  EXPECT_TRUE(http.requests.empty());

  console.server_id.clear();
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::invalid_response);
  console.server_id = "console-a";
  options.command_session_id = "not-a-uuid";
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::invalid_response);
  options.command_session_id = "01234567-89ab-cdef-0123-456789abcdeg";
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::invalid_response);
  options.command_session_id = "01234567-89AB-CDEF-0123-456789ABCDEF";
  options.poll_interval = 0s;
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::invalid_response);
  options.poll_interval = 1s;
  options.wake_timeout = 0s;
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::invalid_response);

  options.wake_timeout = 3s;
  console.power_state = "Off";
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::failed_state);
  console.power_state = "ConnectedStandby";
  auth.web_token.clear();
  EXPECT_EQ(client.wake_and_wait(console, options, {}).error.code, error_e::invalid_response);
}

TEST(XboxRemoteSessionTest, SendsXccsWakeCommandAndWaitsForOn) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  http.responses.push_back({method_e::post, {202, {}}});
  http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"ConnectedStandby"}]})"}});
  http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"On"}]})"}});
  wake_options_t options {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s};
  const xbox_remote::protocol::console_t console {"console-a", "Living Room", "XboxSeriesX", "ConnectedStandby"};

  const auto woken = client_t(http, runtime, auth).wake_and_wait(console, options, {});
  ASSERT_TRUE(woken);
  EXPECT_EQ(woken.value.power_state, "On");
  EXPECT_EQ(woken.http_status, 202);
  ASSERT_EQ(http.requests.size(), 3);
  EXPECT_EQ(http.requests[0].url, "https://xccs.xboxlive.com/commands");
  EXPECT_EQ(http.requests[0].timeout, 15s);
  EXPECT_EQ(http.requests[0].headers.at("Authorization"), "XBL3.0 x=user-hash-secret-fixture;web-token-secret-fixture");
  EXPECT_EQ(http.requests[0].headers.at("skillplatform"), "RemoteManagement");
  EXPECT_EQ(http.requests[0].headers.at("x-xbl-contract-version"), "4");
  const auto body = nlohmann::json::parse(http.requests[0].body);
  EXPECT_EQ(body.at("destination"), "Xbox");
  EXPECT_EQ(body.at("type"), "Power");
  EXPECT_EQ(body.at("command"), "WakeUp");
  EXPECT_EQ(body.at("sessionId"), options.command_session_id);
  EXPECT_EQ(body.at("sourceId"), "com.microsoft.smartglass");
  EXPECT_EQ(body.at("linkedXboxId"), "console-a");
  EXPECT_TRUE(body.at("parameters").empty());
  EXPECT_EQ(runtime.waits, (std::vector<std::chrono::seconds> {1s}));
}

TEST(XboxRemoteSessionTest, DoesNotReplayAnAmbiguousWakePost) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  http.responses.push_back({method_e::post, {0, {}, true}});
  http.responses.push_back({method_e::get, {200, R"({"results":[]})"}});
  http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"On"}]})"}});
  const xbox_remote::protocol::console_t console {"console-a", "Living Room", "XboxSeriesX", "ConnectedStandby"};

  const auto woken = client_t(http, runtime, auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, {});
  ASSERT_TRUE(woken);
  EXPECT_EQ(std::ranges::count(http.requests, method_e::post, &request_t::method), 1);
  EXPECT_EQ(runtime.waits, (std::vector<std::chrono::seconds> {1s}));
}

TEST(XboxRemoteSessionTest, RefreshesKnownUnauthorizedWakeAndSanitizesFailures) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  http.responses.push_back({method_e::post, {401, {}}});
  http.responses.push_back({method_e::post, {202, {}}});
  http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"On"}]})"}});
  const xbox_remote::protocol::console_t console {"console-a", "Living Room", "XboxSeriesX", "ConnectedStandby"};
  std::size_t refreshes = 0;
  client_t client {http, runtime, auth, [&refreshes](xbox_remote::auth::session_context_t &refreshed) {
                     ++refreshes;
                     refreshed.web_token = "refreshed-web-secret-fixture";
                     return true;
                   }};

  ASSERT_TRUE(client.wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, {}));
  EXPECT_EQ(refreshes, 1);
  EXPECT_EQ(http.requests[1].headers.at("Authorization"), "XBL3.0 x=user-hash-secret-fixture;refreshed-web-secret-fixture");

  fake_http_t rejected_http;
  auto rejected_auth = context();
  rejected_http.responses.push_back({method_e::post, {403, {}}});
  const auto rejected = client_t(rejected_http, runtime, rejected_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, {});
  EXPECT_EQ(rejected.error.code, error_e::unauthorized);
  EXPECT_EQ(error_text(rejected.error).find("secret-fixture"), std::string::npos);

  fake_http_t unauthorized_http;
  auto unauthorized_auth = context();
  unauthorized_http.responses.push_back({method_e::post, {401, {}}});
  EXPECT_EQ(client_t(unauthorized_http, runtime, unauthorized_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, {}).error.code, error_e::unauthorized);
}

TEST(XboxRemoteSessionTest, BoundsWakePollingCancellationAndFailurePaths) {
  using namespace xbox_remote::session;
  const xbox_remote::protocol::console_t console {"console-a", "Living Room", "XboxSeriesX", "ConnectedStandby"};

  fake_http_t timeout_http;
  fake_runtime_t timeout_runtime;
  auto timeout_auth = context();
  timeout_http.responses.push_back({method_e::post, {202, {}}});
  timeout_http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"ConnectedStandby"}]})"}});
  timeout_http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"ConnectedStandby"}]})"}});
  EXPECT_EQ(client_t(timeout_http, timeout_runtime, timeout_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 2s}, {}).error.code, error_e::timeout);
  EXPECT_EQ(timeout_runtime.waits, (std::vector<std::chrono::seconds> {1s, 1s}));

  fake_http_t ambiguous_http;
  fake_runtime_t ambiguous_runtime;
  auto ambiguous_auth = context();
  ambiguous_http.responses.push_back({method_e::post, {0, {}, true}});
  ambiguous_http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"ConnectedStandby"}]})"}});
  const auto ambiguous = client_t(ambiguous_http, ambiguous_runtime, ambiguous_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 1s}, {});
  EXPECT_EQ(ambiguous.error.code, error_e::network_error);
  EXPECT_EQ(ambiguous.error.stage, "wake_command");

  fake_http_t cancelled_http;
  fake_runtime_t cancelled_runtime;
  auto cancelled_auth = context();
  std::size_t checks = 0;
  EXPECT_EQ(client_t(cancelled_http, cancelled_runtime, cancelled_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, [&checks]() {
                                                                         return ++checks == 1;
                                                                       })
              .error.code,
            error_e::cancelled);

  fake_http_t wait_cancel_http;
  fake_runtime_t wait_cancel_runtime;
  auto wait_cancel_auth = context();
  wait_cancel_http.responses.push_back({method_e::post, {202, {}}});
  wait_cancel_http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"Living Room","consoleType":"XboxSeriesX","powerState":"ConnectedStandby"}]})"}});
  checks = 0;
  EXPECT_EQ(client_t(wait_cancel_http, wait_cancel_runtime, wait_cancel_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, [&checks]() {
                                                                               return ++checks == 3;
                                                                             })
              .error.code,
            error_e::cancelled);

  fake_http_t discovery_http;
  fake_runtime_t discovery_runtime;
  auto discovery_auth = context();
  discovery_http.responses.push_back({method_e::post, {202, {}}});
  discovery_http.responses.push_back({method_e::get, {403, {}}});
  EXPECT_EQ(client_t(discovery_http, discovery_runtime, discovery_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, {}).error.code, error_e::unauthorized);

  fake_http_t duplicate_http;
  fake_runtime_t duplicate_runtime;
  auto duplicate_auth = context();
  duplicate_http.responses.push_back({method_e::post, {202, {}}});
  duplicate_http.responses.push_back({method_e::get, {200, R"({"results":[{"serverId":"console-a","deviceName":"One","consoleType":"XboxSeriesX","powerState":"On"},{"serverId":"console-a","deviceName":"Two","consoleType":"XboxSeriesX","powerState":"On"}]})"}});
  EXPECT_EQ(client_t(duplicate_http, duplicate_runtime, duplicate_auth).wake_and_wait(console, {"01234567-89ab-cdef-0123-456789abcdef", 1s, 3s}, {}).error.code, error_e::duplicate_console);
}

TEST(XboxRemoteSessionTest, RetriesNetworkFailuresOnlyForIdempotentRequests) {
  using namespace xbox_remote::session;

  fake_http_t recovered_http;
  fake_runtime_t recovered_runtime;
  auto recovered_auth = context();
  recovered_http.responses.push_back({method_e::get, {0, {}, true}});
  recovered_http.responses.push_back({method_e::get, {200, R"({"results":[]})"}});
  const auto recovered = client_t(recovered_http, recovered_runtime, recovered_auth).discover({});
  ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered_http.requests.size(), 2);
  EXPECT_EQ(recovered_runtime.waits, (std::vector<std::chrono::seconds> {1s}));

  fake_http_t exhausted_http;
  fake_runtime_t exhausted_runtime;
  auto exhausted_auth = context();
  const auto exhausted = client_t(exhausted_http, exhausted_runtime, exhausted_auth).discover({});
  EXPECT_EQ(exhausted.error.code, error_e::network_error);
  EXPECT_EQ(exhausted_http.requests.size(), 8);
  EXPECT_EQ(exhausted_runtime.waits, (std::vector<std::chrono::seconds>(7, 1s)));

  fake_http_t post_http;
  fake_runtime_t post_runtime;
  auto post_auth = context();
  post_http.responses.push_back({method_e::post, {0, {}, true}});
  const auto post = client_t(post_http, post_runtime, post_auth).create_and_wait("console", {}, {});
  EXPECT_EQ(post.error.code, error_e::network_error);
  EXPECT_EQ(post_http.requests.size(), 1);
  EXPECT_TRUE(post_runtime.waits.empty());
}

TEST(XboxRemoteSessionTest, CreatesProvisionedSessionSchedulesKeepaliveAndDeletesIdempotently) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  http.responses.push_back({method_e::post, {202, R"({"sessionPath":"/v5/sessions/home/session-1"})"}});
  http.responses.push_back({method_e::get, {200, R"({"state":"Provisioning"})"}});
  http.responses.push_back({method_e::get, {200, R"({"state":"Provisioned"})"}});
  http.responses.push_back({method_e::get, {200, configuration()}});
  client_t client {http, runtime, auth};

  const auto created = client.create_and_wait("console-a", {}, {});
  ASSERT_TRUE(created);
  EXPECT_EQ(created.value.session_id, "session-1");
  EXPECT_EQ(created.value.configuration.ipv4_port, 9002);
  EXPECT_EQ(runtime.waits, (std::vector<std::chrono::seconds> {1s}));
  EXPECT_EQ(created.value.next_keepalive, std::chrono::steady_clock::time_point {106s});
  ASSERT_EQ(http.requests.size(), 4);
  EXPECT_EQ(http.requests[0].method, method_e::post);
  EXPECT_NE(http.requests[0].body.find("\"serverId\":\"console-a\""), std::string::npos);

  auto session = created.value;
  const auto early = client.keepalive_if_due(session, {});
  ASSERT_TRUE(early);
  EXPECT_FALSE(early.value);
  runtime.steady_time = session.next_keepalive;
  http.responses.push_back({method_e::post, {200, "{}"}});
  const auto sent = client.keepalive_if_due(session, {});
  ASSERT_TRUE(sent);
  EXPECT_TRUE(sent.value);
  EXPECT_EQ(session.next_keepalive, std::chrono::steady_clock::time_point {111s});

  http.responses.push_back({method_e::remove, {204, {}}});
  EXPECT_TRUE(client.delete_session(session.session_id));
  http.responses.push_back({method_e::remove, {404, {}}});
  EXPECT_TRUE(client.delete_session(session.session_id));
  EXPECT_TRUE(client.delete_session(""));
}

TEST(XboxRemoteSessionTest, EveryPostCreationFailureAttemptsDelete) {
  using namespace xbox_remote::session;

  fake_http_t malformed_http;
  fake_runtime_t malformed_runtime;
  auto malformed_auth = context();
  malformed_http.responses.push_back({method_e::post, {200, R"({"sessionId":"bad-config"})"}});
  malformed_http.responses.push_back({method_e::get, {200, R"({"state":"Provisioned"})"}});
  malformed_http.responses.push_back({method_e::get, {200, R"({"keepAlivePulseInSeconds":0})"}});
  malformed_http.responses.push_back({method_e::remove, {204, {}}});
  const auto malformed = client_t(malformed_http, malformed_runtime, malformed_auth).create_and_wait("console", {}, {});
  EXPECT_EQ(malformed.error.code, error_e::invalid_response);
  EXPECT_EQ(malformed.error.message, "Home session configuration response was invalid at $.keepAlivePulseInSeconds");
  ASSERT_EQ(malformed_http.requests.size(), 4);
  EXPECT_EQ(malformed_http.requests.back().method, method_e::remove);

  fake_http_t failed_http;
  fake_runtime_t failed_runtime;
  auto failed_auth = context();
  failed_http.responses.push_back({method_e::post, {200, R"({"sessionId":"failed-state"})"}});
  failed_http.responses.push_back({method_e::get, {200, R"({"state":"Failed","errorDetails":{"code":"ConsoleUnavailable","message":"response-secret-fixture"}})"}});
  failed_http.responses.push_back({method_e::remove, {404, {}}});
  const auto failed = client_t(failed_http, failed_runtime, failed_auth).create_and_wait("console", {}, {});
  EXPECT_EQ(failed.error.code, error_e::failed_state);
  EXPECT_EQ(failed.error.message, "Home session provisioning failed (service code ConsoleUnavailable)");
  EXPECT_EQ(error_text(failed.error).find("response-secret-fixture"), std::string::npos);
  EXPECT_EQ(failed_http.requests.back().method, method_e::remove);

  fake_http_t timeout_http;
  fake_runtime_t timeout_runtime;
  auto timeout_auth = context();
  timeout_http.responses.push_back({method_e::post, {200, R"({"sessionId":"timed-out"})"}});
  timeout_http.responses.push_back({method_e::get, {200, R"({"state":"Provisioning"})"}});
  timeout_http.responses.push_back({method_e::get, {200, R"({"state":"Provisioning"})"}});
  timeout_http.responses.push_back({method_e::remove, {204, {}}});
  options_t short_options;
  short_options.provision_timeout = 2s;
  const auto timed_out = client_t(timeout_http, timeout_runtime, timeout_auth).create_and_wait("console", short_options, {});
  EXPECT_EQ(timed_out.error.code, error_e::timeout);
  EXPECT_EQ(timeout_http.requests.back().method, method_e::remove);
}

TEST(XboxRemoteSessionTest, CancellationAndStateHttpFailureAttemptDelete) {
  using namespace xbox_remote::session;
  fake_http_t cancel_http;
  fake_runtime_t cancel_runtime;
  auto cancel_auth = context();
  cancel_http.responses.push_back({method_e::post, {200, R"({"sessionId":"cancelled"})"}});
  cancel_http.responses.push_back({method_e::get, {200, R"({"state":"Provisioning"})"}});
  cancel_http.responses.push_back({method_e::remove, {204, {}}});
  int checks = 0;
  const auto cancelled = client_t(cancel_http, cancel_runtime, cancel_auth).create_and_wait("console", {}, [&checks]() {
    return ++checks >= 4;
  });
  EXPECT_EQ(cancelled.error.code, error_e::cancelled);
  EXPECT_EQ(cancel_http.requests.back().method, method_e::remove);

  fake_http_t status_http;
  fake_runtime_t status_runtime;
  auto status_auth = context();
  status_http.responses.push_back({method_e::post, {200, R"({"sessionId":"server-error"})"}});
  status_http.responses.push_back({method_e::get, {500, "response-secret-fixture"}});
  status_http.responses.push_back({method_e::remove, {204, {}}});
  const auto status = client_t(status_http, status_runtime, status_auth).create_and_wait("console", {}, {});
  EXPECT_EQ(status.error.code, error_e::server_error);
  EXPECT_EQ(error_text(status.error).find("response-secret-fixture"), std::string::npos);
  EXPECT_EQ(status_http.requests.back().method, method_e::remove);
}

TEST(XboxRemoteSessionTest, RefreshesOnceAfterUnauthorizedAndCategorizesFailures) {
  using namespace xbox_remote::session;
  fake_runtime_t runtime;
  auto auth = context();
  fake_http_t http;
  http.responses.push_back({method_e::get, {401, "old-token-secret-fixture"}});
  http.responses.push_back({method_e::get, {200, R"({"results":[]})"}});
  int refreshes = 0;
  client_t client {http, runtime, auth, [&refreshes](xbox_remote::auth::session_context_t &value) {
                     ++refreshes;
                     value.gs_token = "replacement-gs-token-secret-fixture";
                     value.base_uri = "https://replacement.example";
                     return true;
                   }};
  const auto discovered = client.discover({});
  ASSERT_TRUE(discovered);
  EXPECT_EQ(refreshes, 1);
  ASSERT_EQ(http.requests.size(), 2);
  EXPECT_EQ(http.requests[0].headers.at("Authorization"), "Bearer gs-token-secret-fixture");
  EXPECT_EQ(http.requests[1].headers.at("Authorization"), "Bearer replacement-gs-token-secret-fixture");
  EXPECT_EQ(http.requests[1].url, "https://replacement.example/v6/servers/home?mr=50");

  fake_http_t denied_http;
  auto denied_auth = context();
  denied_http.responses.push_back({method_e::get, {401, "response-secret-fixture"}});
  const auto denied = client_t(denied_http, runtime, denied_auth, [](auto &) {
                        return false;
                      }).discover({});
  EXPECT_EQ(denied.error.code, error_e::unauthorized);
  EXPECT_EQ(error_text(denied.error).find("response-secret-fixture"), std::string::npos);

  fake_http_t rate_http;
  auto rate_auth = context();
  rate_http.responses.push_back({method_e::get, {429, "response-secret-fixture"}});
  EXPECT_EQ(client_t(rate_http, runtime, rate_auth).discover({}).error.code, error_e::rate_limited);

  fake_http_t network_http;
  auto network_auth = context();
  network_http.responses.push_back({method_e::get, {0, "transport-secret-fixture", true}});
  EXPECT_EQ(client_t(network_http, runtime, network_auth).discover({}).error.code, error_e::network_error);
}

TEST(XboxRemoteSessionTest, ExchangesSdpAndIceWithoutConnectingTransport) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  client_t client {http, runtime, auth};

  http.responses.push_back({method_e::post, {202, {}}});
  ASSERT_TRUE(client.send_sdp("session-1", {"v=0\r\n"}, {}));
  http.responses.push_back({method_e::get, {204, {}}});
  const auto pending_sdp = client.poll_sdp("session-1", {});
  ASSERT_TRUE(pending_sdp);
  EXPECT_FALSE(pending_sdp.value.has_value());
  http.responses.push_back({method_e::get, {200, R"({"exchangeResponse":"{\"sdp\":\"v=0\"}"})"}});
  const auto answer = client.poll_sdp("session-1", {});
  ASSERT_TRUE(answer);
  ASSERT_TRUE(answer.value.has_value());
  EXPECT_EQ(*answer.value, R"({"sdp":"v=0"})");

  const std::vector<xbox_remote::protocol::ice_candidate_t> candidates {
    {"candidate:1 1 udp 1 192.0.2.10 5000 typ host", "0", 0, "ufrag"},
  };
  http.responses.push_back({method_e::post, {200, {}}});
  ASSERT_TRUE(client.send_ice("session-1", candidates, {}));
  http.responses.push_back({method_e::get, {200, R"({"exchangeResponse":"[]"})"}});
  const auto remote_ice = client.poll_ice("session-1", {});
  ASSERT_TRUE(remote_ice);
  ASSERT_TRUE(remote_ice.value.has_value());
  EXPECT_EQ(*remote_ice.value, "[]");
  EXPECT_NE(http.requests[0].body.find("\"messageType\":\"offer\""), std::string::npos);
  EXPECT_NE(http.requests[3].body.find("iceCandidate"), std::string::npos);
}

TEST(XboxRemoteSessionTest, RejectsMalformedIdentifiersResponsesAndKeepaliveFailures) {
  using namespace xbox_remote::session;
  fake_http_t http;
  fake_runtime_t runtime;
  auto auth = context();
  client_t client {http, runtime, auth};
  EXPECT_EQ(client.send_sdp("../bad", {"v=0"}, {}).error.code, error_e::invalid_response);
  EXPECT_EQ(client.send_ice("bad/id", {}, {}).error.code, error_e::invalid_response);
  EXPECT_EQ(client.delete_session("bad?id").error.code, error_e::invalid_response);

  http.responses.push_back({method_e::get, {200, "{not-json"}});
  EXPECT_EQ(client.discover({}).error.code, error_e::invalid_response);

  xbox_remote::session::provisioned_t session;
  EXPECT_EQ(client.keepalive_if_due(session, {}).error.code, error_e::invalid_response);
  session.session_id = "session-1";
  session.configuration.keepalive_seconds = 10;
  session.next_keepalive = runtime.steady_time;
  http.responses.push_back({method_e::post, {404, "response-secret-fixture"}});
  const auto keepalive = client.keepalive_if_due(session, {});
  EXPECT_EQ(keepalive.error.code, error_e::not_found);
  EXPECT_EQ(error_text(keepalive.error).find("response-secret-fixture"), std::string::npos);

  http.responses.push_back({method_e::get, {200, "{}"}});
  EXPECT_EQ(client.poll_ice("session-1", {}).error.code, error_e::invalid_response);
}

TEST(XboxRemoteSessionTest, RejectsInvalidContextCancellationAndUnexpectedHttpStatuses) {
  using namespace xbox_remote::session;
  fake_runtime_t runtime;

  fake_http_t cancelled_http;
  auto cancelled_auth = context();
  EXPECT_EQ(client_t(cancelled_http, runtime, cancelled_auth).discover([]() {
                                                               return true;
                                                             })
              .error.code,
            error_e::cancelled);

  fake_http_t context_http;
  auto invalid_auth = context();
  invalid_auth.base_uri = "http://untrusted.example";
  EXPECT_EQ(client_t(context_http, runtime, invalid_auth).discover({}).error.code, error_e::invalid_response);
  invalid_auth.base_uri = "https://gssv.example?query";
  EXPECT_EQ(client_t(context_http, runtime, invalid_auth).discover({}).error.code, error_e::invalid_response);
  invalid_auth.base_uri = "https://";
  EXPECT_EQ(client_t(context_http, runtime, invalid_auth).discover({}).error.code, error_e::invalid_response);

  fake_http_t twice_http;
  auto twice_auth = context();
  twice_http.responses.push_back({method_e::get, {401, {}}});
  twice_http.responses.push_back({method_e::get, {401, {}}});
  int refreshes = 0;
  const auto twice = client_t(twice_http, runtime, twice_auth, [&refreshes](auto &) {
                       ++refreshes;
                       return true;
                     }).discover({});
  EXPECT_EQ(twice.error.code, error_e::unauthorized);
  EXPECT_EQ(refreshes, 1);

  fake_http_t forbidden_http;
  auto forbidden_auth = context();
  forbidden_http.responses.push_back({method_e::get, {403, {}}});
  EXPECT_EQ(client_t(forbidden_http, runtime, forbidden_auth).discover({}).error.code, error_e::unauthorized);

  fake_http_t unexpected_http;
  auto unexpected_auth = context();
  unexpected_http.responses.push_back({method_e::get, {418, {}}});
  EXPECT_EQ(client_t(unexpected_http, runtime, unexpected_auth).discover({}).error.code, error_e::http_error);
}

TEST(XboxRemoteSessionTest, RejectsCreateFailuresBeforeAUsableSessionExists) {
  using namespace xbox_remote::session;
  fake_runtime_t runtime;

  fake_http_t invalid_options_http;
  auto invalid_options_auth = context();
  options_t invalid_options;
  invalid_options.width = 0;
  EXPECT_EQ(client_t(invalid_options_http, runtime, invalid_options_auth).create_and_wait("console", invalid_options, {}).error.code, error_e::invalid_response);

  fake_http_t network_http;
  auto network_auth = context();
  network_http.responses.push_back({method_e::post, {0, {}, true}});
  EXPECT_EQ(client_t(network_http, runtime, network_auth).create_and_wait("console", {}, {}).error.code, error_e::network_error);

  fake_http_t missing_http;
  auto missing_auth = context();
  missing_http.responses.push_back({method_e::post, {404, {}}});
  EXPECT_EQ(client_t(missing_http, runtime, missing_auth).create_and_wait("console", {}, {}).error.code, error_e::not_found);

  fake_http_t malformed_http;
  auto malformed_auth = context();
  malformed_http.responses.push_back({method_e::post, {200, R"({"sessionId":"bad/id"})"}});
  EXPECT_EQ(client_t(malformed_http, runtime, malformed_auth).create_and_wait("console", {}, {}).error.code, error_e::invalid_response);
}

TEST(XboxRemoteSessionTest, EveryAdditionalPostCreationFailureAttemptsDelete) {
  using namespace xbox_remote::session;

  fake_http_t cancelled_http;
  fake_runtime_t cancelled_runtime;
  auto cancelled_auth = context();
  cancelled_http.responses.push_back({method_e::post, {200, R"({"sessionId":"cancel-before-state"})"}});
  cancelled_http.responses.push_back({method_e::remove, {204, {}}});
  int checks = 0;
  EXPECT_EQ(client_t(cancelled_http, cancelled_runtime, cancelled_auth).create_and_wait("console", {}, [&checks]() {
                                                                         return ++checks >= 2;
                                                                       })
              .error.code,
            error_e::cancelled);
  EXPECT_EQ(cancelled_http.requests.back().method, method_e::remove);

  fake_http_t state_network_http;
  fake_runtime_t state_network_runtime;
  auto state_network_auth = context();
  state_network_http.responses.push_back({method_e::post, {200, R"({"sessionId":"state-network"})"}});
  for (int attempt = 0; attempt < 8; ++attempt) {
    state_network_http.responses.push_back({method_e::get, {0, {}, true}});
  }
  state_network_http.responses.push_back({method_e::remove, {204, {}}});
  EXPECT_EQ(client_t(state_network_http, state_network_runtime, state_network_auth).create_and_wait("console", {}, {}).error.code, error_e::network_error);
  EXPECT_EQ(state_network_http.requests.back().method, method_e::remove);

  fake_http_t state_malformed_http;
  fake_runtime_t state_malformed_runtime;
  auto state_malformed_auth = context();
  state_malformed_http.responses.push_back({method_e::post, {200, R"({"sessionId":"state-malformed"})"}});
  state_malformed_http.responses.push_back({method_e::get, {200, "{}"}});
  state_malformed_http.responses.push_back({method_e::remove, {204, {}}});
  EXPECT_EQ(client_t(state_malformed_http, state_malformed_runtime, state_malformed_auth).create_and_wait("console", {}, {}).error.code, error_e::invalid_response);
  EXPECT_EQ(state_malformed_http.requests.back().method, method_e::remove);

  fake_http_t config_network_http;
  fake_runtime_t config_network_runtime;
  auto config_network_auth = context();
  config_network_http.responses.push_back({method_e::post, {200, R"({"sessionId":"config-network"})"}});
  config_network_http.responses.push_back({method_e::get, {200, R"({"state":"Provisioned"})"}});
  for (int attempt = 0; attempt < 8; ++attempt) {
    config_network_http.responses.push_back({method_e::get, {0, {}, true}});
  }
  config_network_http.responses.push_back({method_e::remove, {204, {}}});
  EXPECT_EQ(client_t(config_network_http, config_network_runtime, config_network_auth).create_and_wait("console", {}, {}).error.code, error_e::network_error);
  EXPECT_EQ(config_network_http.requests.back().method, method_e::remove);

  fake_http_t config_status_http;
  fake_runtime_t config_status_runtime;
  auto config_status_auth = context();
  config_status_http.responses.push_back({method_e::post, {200, R"({"sessionId":"config-status"})"}});
  config_status_http.responses.push_back({method_e::get, {200, R"({"state":"Provisioned"})"}});
  config_status_http.responses.push_back({method_e::get, {404, {}}});
  config_status_http.responses.push_back({method_e::remove, {204, {}}});
  EXPECT_EQ(client_t(config_status_http, config_status_runtime, config_status_auth).create_and_wait("console", {}, {}).error.code, error_e::not_found);
  EXPECT_EQ(config_status_http.requests.back().method, method_e::remove);
}

TEST(XboxRemoteSessionTest, CategorizesSignalingAndCleanupTransportFailures) {
  using namespace xbox_remote::session;
  fake_runtime_t runtime;

  fake_http_t sdp_network_http;
  auto sdp_network_auth = context();
  sdp_network_http.responses.push_back({method_e::post, {0, {}, true}});
  EXPECT_EQ(client_t(sdp_network_http, runtime, sdp_network_auth).send_sdp("session", {"v=0"}, {}).error.code, error_e::network_error);

  fake_http_t sdp_status_http;
  auto sdp_status_auth = context();
  sdp_status_http.responses.push_back({method_e::post, {403, {}}});
  EXPECT_EQ(client_t(sdp_status_http, runtime, sdp_status_auth).send_sdp("session", {"v=0"}, {}).error.code, error_e::unauthorized);

  fake_http_t ice_network_http;
  auto ice_network_auth = context();
  ice_network_http.responses.push_back({method_e::post, {0, {}, true}});
  EXPECT_EQ(client_t(ice_network_http, runtime, ice_network_auth).send_ice("session", {}, {}).error.code, error_e::network_error);

  fake_http_t ice_status_http;
  auto ice_status_auth = context();
  ice_status_http.responses.push_back({method_e::post, {500, {}}});
  EXPECT_EQ(client_t(ice_status_http, runtime, ice_status_auth).send_ice("session", {}, {}).error.code, error_e::server_error);

  fake_http_t poll_http;
  auto poll_auth = context();
  client_t poll_client {poll_http, runtime, poll_auth};
  EXPECT_EQ(poll_client.poll_sdp("bad/id", {}).error.code, error_e::invalid_response);
  poll_http.responses.push_back({method_e::get, {0, {}, true}});
  EXPECT_EQ(poll_client.poll_sdp("session", {}).error.code, error_e::network_error);
  poll_http.responses.push_back({method_e::get, {429, {}}});
  EXPECT_EQ(poll_client.poll_sdp("session", {}).error.code, error_e::rate_limited);

  provisioned_t keepalive_session;
  keepalive_session.session_id = "session";
  keepalive_session.configuration.keepalive_seconds = 10;
  keepalive_session.next_keepalive = runtime.steady_time;
  fake_http_t keepalive_http;
  auto keepalive_auth = context();
  keepalive_http.responses.push_back({method_e::post, {0, {}, true}});
  EXPECT_EQ(client_t(keepalive_http, runtime, keepalive_auth).keepalive_if_due(keepalive_session, {}).error.code, error_e::network_error);

  fake_http_t delete_network_http;
  auto delete_network_auth = context();
  delete_network_http.responses.push_back({method_e::remove, {0, {}, true}});
  EXPECT_EQ(client_t(delete_network_http, runtime, delete_network_auth).delete_session("session").error.code, error_e::network_error);

  fake_http_t delete_status_http;
  auto delete_status_auth = context();
  delete_status_http.responses.push_back({method_e::remove, {418, {}}});
  EXPECT_EQ(client_t(delete_status_http, runtime, delete_status_auth).delete_session("session").error.code, error_e::http_error);
}
