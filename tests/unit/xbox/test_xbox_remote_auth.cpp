/**
 * @file tests/unit/xbox/test_xbox_remote_auth.cpp
 * @brief Offline tests for the Xbox Remote Play authentication state machine.
 */

// standard includes
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
#include "tests/tests_common.h"
#include "src/xbox_remote/auth.h"
#include "src/xbox_remote/http_runtime.h"

namespace {
  using namespace std::chrono_literals;
  using xbox_remote::auth::http_response_t;

  /**
   * @brief Captured fake HTTP request.
   */
  struct request_t {
    std::string url;  ///< Requested endpoint.
    std::string body;  ///< Submitted request body.
    std::map<std::string, std::string> headers;  ///< Submitted request headers.
    std::chrono::seconds timeout {};  ///< Submitted hard deadline.
  };

  /**
   * @brief FIFO fake HTTP client used without network access.
   */
  class fake_http_client_t: public xbox_remote::auth::http_client_t {
  public:
    /**
     * @brief Capture a request and return the next queued response.
     *
     * @param url Requested endpoint.
     * @param body Submitted body.
     * @param headers Submitted headers.
     * @param timeout Submitted timeout.
     * @return Next queued response, or a transport failure when empty.
     */
    http_response_t post(
      std::string_view url,
      std::string_view body,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout
    ) override {
      requests.push_back({std::string {url}, std::string {body}, headers, timeout});
      if (responses.empty()) {
        return {0, {}, true};
      }
      auto response = std::move(responses.front());
      responses.pop_front();
      return response;
    }

    std::deque<http_response_t> responses;  ///< Responses returned in FIFO order.
    std::vector<request_t> requests;  ///< Captured requests.
  };

  /**
   * @brief Deterministic clocks and cancellation-aware wait implementation.
   */
  class fake_runtime_t: public xbox_remote::auth::runtime_t {
  public:
    /**
     * @brief Return the fake wall clock.
     *
     * @return Current fake wall time.
     */
    std::chrono::system_clock::time_point system_now() const override {
      return system_time;
    }

    /**
     * @brief Return the fake monotonic clock.
     *
     * @return Current fake monotonic time.
     */
    std::chrono::steady_clock::time_point steady_now() const override {
      return steady_time;
    }

    /**
     * @brief Advance both fake clocks unless cancellation is observed.
     *
     * @param duration Requested wait.
     * @param cancelled Cancellation callback.
     * @return @c false when cancellation is observed.
     */
    bool wait_for(std::chrono::seconds duration, const std::function<bool()> &cancelled) override {
      waits.push_back(duration);
      if (cancelled && cancelled()) {
        return false;
      }
      system_time += duration;
      steady_time += duration;
      return !(cancelled && cancelled());
    }

    std::chrono::system_clock::time_point system_time {std::chrono::seconds {1700000000}};  ///< Fake wall time.
    std::chrono::steady_clock::time_point steady_time {100s};  ///< Fake monotonic time.
    std::vector<std::chrono::seconds> waits;  ///< Requested polling waits.
  };

  /**
   * @brief Queue a complete successful Xbox User Token, XSTS, and GSSV chain.
   *
   * @param http Fake HTTP client to populate.
   * @param default_region Whether the second region is explicitly default.
   */
  void queue_xbox_chain(fake_http_client_t &http, bool default_region = true) {
    http.responses.push_back({200, R"({"Token":"user-secret-fixture"})"});
    http.responses.push_back({200, R"({"Token":"gssv-xsts-secret-fixture","DisplayClaims":{"xui":[{"uhs":"fixture-user-hash"}]}})"});
    http.responses.push_back({200, R"({"Token":"web-secret-fixture"})"});
    const nlohmann::json gssv {
      {"gsToken", "gs-secret-fixture"},
      {"durationInSeconds", 7200},
      {"offeringSettings", {{"regions", {
                                          {{"baseUri", "https://fallback.example"}, {"isDefault", false}},
                                          {{"baseUri", "https://default.example"}, {"isDefault", default_region}},
                                        }}}},
    };
    http.responses.push_back({200, gssv.dump()});
  }

  /**
   * @brief Build deterministic credentials for resume tests.
   *
   * @param runtime Fake runtime defining the expiry baseline.
   * @param lifetime Remaining token lifetime.
   * @return Test OAuth credentials.
   */
  xbox_remote::auth::oauth_credentials_t credentials(const fake_runtime_t &runtime, std::chrono::seconds lifetime) {
    return {"access-secret-fixture", "refresh secret/+fixture", runtime.system_time + lifetime};
  }

  /**
   * @brief Combine all caller-visible error text for secret checks.
   *
   * @param failure Authentication failure.
   * @return Caller-visible text.
   */
  std::string error_text(const xbox_remote::auth::auth_failure_t &failure) {
    return failure.stage + " " + failure.message;
  }
}  // namespace

TEST(XboxRemoteAuthTest, CompletesDeviceCodePendingSlowDownAndXboxTokenChain) {
  using namespace xbox_remote::auth;
  fake_http_client_t http;
  fake_runtime_t runtime;
  http.responses.push_back({200, R"({"device_code":"device-secret-fixture","user_code":"ABCD-EFGH","verification_uri":"https://microsoft.com/link","interval":5,"expires_in":60})"});
  http.responses.push_back({400, R"({"error":"authorization_pending"})"});
  http.responses.push_back({400, R"({"error":"slow_down"})"});
  http.responses.push_back({200, R"({"access_token":"access-secret-fixture","refresh_token":"refresh-secret-fixture","expires_in":3600})"});
  queue_xbox_chain(http);

  client_t client {http, runtime};
  const auto code = client.begin_device_code();
  ASSERT_TRUE(code);
  EXPECT_EQ(code.value.user_code, "ABCD-EFGH");
  EXPECT_EQ(code.value.verification_uri, "https://microsoft.com/link");
  EXPECT_EQ(code.value.polling_interval, 5s);
  EXPECT_EQ(code.value.issued_at, std::chrono::steady_clock::time_point {100s});

  const auto authenticated = client.complete_device_code(code.value, []() {
    return false;
  });
  ASSERT_TRUE(authenticated);
  EXPECT_EQ(runtime.waits, (std::vector<std::chrono::seconds> {5s, 10s}));
  EXPECT_EQ(authenticated.value.oauth.expires_at, std::chrono::system_clock::time_point {1700003615s});
  EXPECT_EQ(authenticated.value.session.user_hash, "fixture-user-hash");
  EXPECT_EQ(authenticated.value.session.web_token, "web-secret-fixture");
  EXPECT_EQ(authenticated.value.session.gs_token, "gs-secret-fixture");
  EXPECT_EQ(authenticated.value.session.base_uri, "https://default.example");
  EXPECT_EQ(authenticated.value.session.expires_at, std::chrono::system_clock::time_point {1700007215s});

  ASSERT_EQ(http.requests.size(), 8);
  EXPECT_EQ(http.requests[0].url, "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode");
  EXPECT_NE(http.requests[0].body.find("offline_access"), std::string::npos);
  EXPECT_NE(http.requests[1].body.find("device_code=device-secret-fixture"), std::string::npos);
  EXPECT_EQ(http.requests[4].url, "https://user.auth.xboxlive.com/user/authenticate");
  EXPECT_EQ(nlohmann::json::parse(http.requests[5].body).at("RelyingParty"), "http://gssv.xboxlive.com/");
  EXPECT_EQ(nlohmann::json::parse(http.requests[6].body).at("RelyingParty"), "http://xboxlive.com");
  EXPECT_EQ(nlohmann::json::parse(http.requests[7].body).at("offeringId"), "xhome");
  for (const auto &request : http.requests) {
    EXPECT_EQ(request.timeout, 15s);
  }
}

TEST(XboxRemoteAuthTest, BeginDeviceCodeReturnsSanitizedNetworkHttpAndMalformedFailures) {
  using namespace xbox_remote::auth;
  fake_runtime_t runtime;

  fake_http_client_t network_http;
  network_http.responses.push_back({0, "transport-secret-fixture", true});
  const auto network = client_t {network_http, runtime}.begin_device_code();
  EXPECT_EQ(network.error.code, auth_error_e::network_error);
  EXPECT_EQ(network_http.requests.size(), 8);
  EXPECT_EQ(runtime.waits, (std::vector<std::chrono::seconds>(7, 1s)));
  EXPECT_EQ(error_text(network.error).find("transport-secret-fixture"), std::string::npos);

  fake_http_client_t rejected_http;
  rejected_http.responses.push_back({503, "service-secret-fixture"});
  const auto rejected = client_t {rejected_http, runtime}.begin_device_code();
  EXPECT_EQ(rejected.error.code, auth_error_e::http_error);
  EXPECT_EQ(rejected.error.http_status, 503);
  EXPECT_EQ(error_text(rejected.error).find("service-secret-fixture"), std::string::npos);

  fake_http_client_t malformed_http;
  malformed_http.responses.push_back({200, "{not-json"});
  const auto malformed = client_t {malformed_http, runtime}.begin_device_code();
  EXPECT_EQ(malformed.error.code, auth_error_e::invalid_response);

  fake_http_client_t incomplete_http;
  incomplete_http.responses.push_back({200, R"({"device_code":"private"})"});
  const auto incomplete = client_t {incomplete_http, runtime}.begin_device_code();
  EXPECT_EQ(incomplete.error.code, auth_error_e::invalid_response);
}

TEST(XboxRemoteAuthTest, DeviceCodePollingReportsDeclineExpiryUnexpectedAndTimeout) {
  using namespace xbox_remote::auth;
  const auto never_cancel = []() {
    return false;
  };

  fake_runtime_t decline_runtime;
  fake_http_client_t decline_http;
  decline_http.responses.push_back({400, R"({"error":"authorization_declined","detail":"decline-secret-fixture"})"});
  const device_code_t decline_code {"private", "visible", "https://example", 5s, 60s, decline_runtime.steady_time};
  const auto declined = client_t {decline_http, decline_runtime}.complete_device_code(decline_code, never_cancel);
  EXPECT_EQ(declined.error.code, auth_error_e::authorization_declined);
  EXPECT_EQ(error_text(declined.error).find("decline-secret-fixture"), std::string::npos);

  fake_runtime_t expiry_runtime;
  fake_http_client_t expiry_http;
  expiry_http.responses.push_back({400, R"({"error":"expired_token"})"});
  const device_code_t expiry_code {"private", "visible", "https://example", 5s, 60s, expiry_runtime.steady_time};
  EXPECT_EQ(client_t(expiry_http, expiry_runtime).complete_device_code(expiry_code, never_cancel).error.code, auth_error_e::code_expired);

  fake_runtime_t unexpected_runtime;
  fake_http_client_t unexpected_http;
  unexpected_http.responses.push_back({429, R"({"error":"unexpected","token":"response-secret-fixture"})"});
  const device_code_t unexpected_code {"private", "visible", "https://example", 5s, 60s, unexpected_runtime.steady_time};
  const auto unexpected = client_t(unexpected_http, unexpected_runtime).complete_device_code(unexpected_code, never_cancel);
  EXPECT_EQ(unexpected.error.code, auth_error_e::http_error);
  EXPECT_EQ(error_text(unexpected.error).find("response-secret-fixture"), std::string::npos);

  fake_runtime_t timeout_runtime;
  fake_http_client_t timeout_http;
  const device_code_t timed_out {"private", "visible", "https://example", 5s, 0s, timeout_runtime.steady_time};
  EXPECT_EQ(client_t(timeout_http, timeout_runtime).complete_device_code(timed_out, never_cancel).error.code, auth_error_e::timeout);
  EXPECT_TRUE(timeout_http.requests.empty());
}

TEST(XboxRemoteAuthTest, DeviceCodePollingCanBeCancelledBeforeOrDuringWait) {
  using namespace xbox_remote::auth;
  fake_runtime_t immediate_runtime;
  fake_http_client_t immediate_http;
  const device_code_t code {"private", "visible", "https://example", 5s, 60s, immediate_runtime.steady_time};
  EXPECT_EQ(client_t(immediate_http, immediate_runtime).complete_device_code(code, []() {
                                                         return true;
                                                       })
              .error.code,
            auth_error_e::cancelled);
  EXPECT_TRUE(immediate_http.requests.empty());

  fake_runtime_t waiting_runtime;
  fake_http_client_t waiting_http;
  waiting_http.responses.push_back({400, R"({"error":"authorization_pending"})"});
  int checks = 0;
  const auto during_wait = client_t(waiting_http, waiting_runtime).complete_device_code(code, [&checks]() {
    return ++checks >= 2;
  });
  EXPECT_EQ(during_wait.error.code, auth_error_e::cancelled);
  EXPECT_EQ(waiting_runtime.waits, (std::vector<std::chrono::seconds> {5s}));
}

TEST(XboxRemoteAuthTest, DeviceCodePollingRetriesTransientNetworkFailures) {
  using namespace xbox_remote::auth;
  fake_runtime_t runtime;
  fake_http_client_t http;
  http.responses.push_back({0, "transport-secret-fixture", true});
  http.responses.push_back({400, R"({"error":"expired_token"})"});
  const device_code_t code {"private", "visible", "https://example", 5s, 60s, runtime.steady_time};

  const auto result = client_t(http, runtime).complete_device_code(code, []() {
    return false;
  });
  EXPECT_EQ(result.error.code, auth_error_e::code_expired);
  EXPECT_EQ(runtime.waits, (std::vector<std::chrono::seconds> {5s}));
  EXPECT_EQ(error_text(result.error).find("transport-secret-fixture"), std::string::npos);
  EXPECT_EQ(http.requests.size(), 2);
}

TEST(XboxRemoteAuthTest, ResumeUsesValidAccessTokenWithoutRefreshAndFallsBackToFirstRegion) {
  using namespace xbox_remote::auth;
  fake_runtime_t runtime;
  fake_http_client_t http;
  queue_xbox_chain(http, false);

  const auto result = client_t(http, runtime).resume(credentials(runtime, 1h), []() {
    return false;
  });
  ASSERT_TRUE(result);
  ASSERT_EQ(http.requests.size(), 4);
  EXPECT_EQ(http.requests.front().url, "https://user.auth.xboxlive.com/user/authenticate");
  EXPECT_EQ(result.value.session.base_uri, "https://fallback.example");
  EXPECT_EQ(result.value.oauth.access_token, "access-secret-fixture");
}

TEST(XboxRemoteAuthTest, ResumeRefreshesEarlyAndRetainsAnOmittedRefreshToken) {
  using namespace xbox_remote::auth;
  fake_runtime_t runtime;
  fake_http_client_t http;
  http.responses.push_back({200, R"({"access_token":"replacement-access-secret","expires_in":1800})"});
  queue_xbox_chain(http);

  const auto result = client_t(http, runtime).resume(credentials(runtime, 5min), []() {
    return false;
  });
  ASSERT_TRUE(result);
  ASSERT_EQ(http.requests.size(), 5);
  EXPECT_EQ(http.requests.front().url, "https://login.microsoftonline.com/consumers/oauth2/v2.0/token");
  EXPECT_NE(http.requests.front().body.find("refresh_token=refresh%20secret%2F%2Bfixture"), std::string::npos);
  EXPECT_EQ(http.requests.front().headers.at("Cache-Control"), "no-store, must-revalidate, no-cache");
  EXPECT_EQ(result.value.oauth.access_token, "replacement-access-secret");
  EXPECT_EQ(result.value.oauth.refresh_token, "refresh secret/+fixture");
  EXPECT_EQ(result.value.oauth.expires_at, runtime.system_time + 1800s);
}

TEST(XboxRemoteAuthTest, RefreshDecisionTracksInjectedWallClockChanges) {
  using namespace xbox_remote::auth;
  const auto baseline = std::chrono::system_clock::time_point {1000s};
  const oauth_credentials_t value {"access", "refresh", baseline + 10min};
  EXPECT_FALSE(should_refresh(value, baseline));
  EXPECT_TRUE(should_refresh(value, baseline + 5min));
  EXPECT_FALSE(should_refresh(value, baseline - 1h));
}

TEST(XboxRemoteRuntimeTest, ProductionAdaptersRejectUnsafeRequestsAndImmediateCancellation) {
  using namespace xbox_remote::auth;
  curl_http_client_t http;
  const auto response = http.post("http://unsafe.example", "body-secret-fixture", {}, 1s);
  EXPECT_TRUE(response.network_error);
  EXPECT_EQ(response.status_code, 0);
  EXPECT_TRUE(response.body.empty());

  http.set_cancellation_callback([]() {
    return true;
  });
  const auto cancelled = http.get("https://cancelled.invalid", {}, 1s);
  EXPECT_TRUE(cancelled.network_error);
  EXPECT_EQ(cancelled.status_code, 0);
  EXPECT_TRUE(cancelled.body.empty());

  system_runtime_t runtime;
  EXPECT_FALSE(runtime.wait_for(1s, []() {
    return true;
  }));
  EXPECT_LE(runtime.steady_now() - std::chrono::steady_clock::now(), 1s);
  EXPECT_LE(runtime.system_now() - std::chrono::system_clock::now(), 1s);
}

TEST(XboxRemoteAuthTest, ResumeRejectsMissingRefreshAndSanitizesRefreshFailures) {
  using namespace xbox_remote::auth;
  fake_runtime_t runtime;
  fake_http_client_t unused_http;
  auto missing_credentials = credentials(runtime, 1h);
  missing_credentials.refresh_token.clear();
  EXPECT_EQ(client_t(unused_http, runtime).resume(missing_credentials, []() {
                                            return false;
                                          })
              .error.code,
            auth_error_e::invalid_response);

  fake_http_client_t rejected_http;
  rejected_http.responses.push_back({401, R"({"refresh_token":"response-secret-fixture"})"});
  const auto rejected = client_t(rejected_http, runtime).resume(credentials(runtime, 0s), []() {
    return false;
  });
  EXPECT_EQ(rejected.error.code, auth_error_e::http_error);
  EXPECT_EQ(rejected.error.stage, "microsoft_refresh");
  EXPECT_EQ(error_text(rejected.error).find("response-secret-fixture"), std::string::npos);
}

TEST(XboxRemoteAuthTest, MalformedXboxStagesReturnStructuredSanitizedFailures) {
  using namespace xbox_remote::auth;
  fake_runtime_t runtime;
  fake_http_client_t malformed_user_http;
  malformed_user_http.responses.push_back({200, R"(["user-response-secret-fixture"])"});
  const auto malformed_user = client_t(malformed_user_http, runtime).resume(credentials(runtime, 1h), []() {
    return false;
  });
  EXPECT_EQ(malformed_user.error.code, auth_error_e::invalid_response);
  EXPECT_EQ(malformed_user.error.stage, "xbox_user_token");
  EXPECT_EQ(error_text(malformed_user.error).find("user-response-secret-fixture"), std::string::npos);

  fake_http_client_t malformed_gssv_http;
  malformed_gssv_http.responses.push_back({200, R"({"Token":"user"})"});
  malformed_gssv_http.responses.push_back({200, R"({"Token":"xsts","DisplayClaims":{"xui":[{"uhs":"hash"}]}})"});
  malformed_gssv_http.responses.push_back({200, R"({"Token":"web"})"});
  malformed_gssv_http.responses.push_back({200, R"({"gsToken":"gssv-response-secret-fixture","durationInSeconds":10,"offeringSettings":{"regions":[]}})"});
  const auto malformed_gssv = client_t(malformed_gssv_http, runtime).resume(credentials(runtime, 1h), []() {
    return false;
  });
  EXPECT_EQ(malformed_gssv.error.code, auth_error_e::invalid_response);
  EXPECT_EQ(malformed_gssv.error.stage, "gssv_xhome");
  EXPECT_EQ(error_text(malformed_gssv.error).find("gssv-response-secret-fixture"), std::string::npos);
}

TEST(XboxRemoteAuthTest, RetriesTransientXboxFailureAndPreservesOauthOnTerminalFailure) {
  using namespace xbox_remote::auth;
  fake_runtime_t retry_runtime;
  fake_http_client_t retry_http;
  retry_http.responses.push_back({200, R"({"Token":"user"})"});
  retry_http.responses.push_back({0, "transport-secret-fixture", true});
  retry_http.responses.push_back({200, R"({"Token":"xsts","DisplayClaims":{"xui":[{"uhs":"hash"}]}})"});
  retry_http.responses.push_back({200, R"({"Token":"web"})"});
  retry_http.responses.push_back({200, R"({"gsToken":"gs","durationInSeconds":10,"offeringSettings":{"regions":[{"baseUri":"https://region.example","isDefault":true}]}})"});
  const auto retried = client_t(retry_http, retry_runtime).resume(credentials(retry_runtime, 1h), []() {
    return false;
  });
  ASSERT_TRUE(retried);
  EXPECT_EQ(retry_runtime.waits, (std::vector<std::chrono::seconds> {1s}));

  fake_runtime_t failure_runtime;
  fake_http_client_t failure_http;
  failure_http.responses.push_back({503, "response-secret-fixture"});
  const auto failed = client_t(failure_http, failure_runtime).resume(credentials(failure_runtime, 1h), []() {
    return false;
  });
  EXPECT_EQ(failed.error.code, auth_error_e::http_error);
  EXPECT_EQ(failed.value.oauth.refresh_token, "refresh secret/+fixture");
  EXPECT_EQ(error_text(failed.error).find("response-secret-fixture"), std::string::npos);
}
