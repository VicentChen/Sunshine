/**
 * @file src/xbox_remote/auth.cpp
 * @brief Microsoft and Xbox Home authentication state machine.
 */

#include "src/xbox_remote/auth.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

// library includes
#include <nlohmann/json.hpp>

namespace xbox_remote::auth {
  namespace {
    using json = nlohmann::json;
    using namespace std::chrono_literals;

    constexpr std::string_view microsoft_client_id = "1f907974-e22b-4810-a9de-d9647380c97e";
    constexpr std::string_view microsoft_scope = "xboxlive.signin openid profile offline_access";
    constexpr std::string_view device_code_url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode";
    constexpr std::string_view token_url = "https://login.microsoftonline.com/consumers/oauth2/v2.0/token";
    constexpr std::string_view xbox_user_url = "https://user.auth.xboxlive.com/user/authenticate";
    constexpr std::string_view xsts_url = "https://xsts.auth.xboxlive.com/xsts/authorize";
    constexpr std::string_view gssv_url = "https://xhome.gssv-play-prod.xboxlive.com/v2/login/user";
    constexpr std::chrono::seconds request_timeout = 15s;
    constexpr std::chrono::seconds retry_delay = 1s;
    constexpr std::size_t maximum_request_attempts = 8;

    /**
     * @brief Construct a sanitized authentication failure.
     *
     * @tparam T Result value type.
     * @param code Error category.
     * @param stage Fixed stage name.
     * @param message Fixed non-secret diagnostic.
     * @param status Optional HTTP status.
     * @return Failed authentication result.
     */
    template<typename T>
    auth_result_t<T> fail(auth_error_e code, std::string stage, std::string message, std::uint32_t status = 0) {
      auth_result_t<T> result;
      result.error = {code, std::move(stage), status, std::move(message)};
      return result;
    }

    /**
     * @brief Percent-encode one form value.
     *
     * @param value Unencoded value.
     * @return RFC 3986 percent-encoded value.
     */
    std::string form_encode(std::string_view value) {
      std::ostringstream encoded;
      encoded << std::uppercase << std::hex;
      for (const unsigned char character : value) {
        if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
          encoded << static_cast<char>(character);
        } else {
          encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned>(character);
        }
      }
      return encoded.str();
    }

    /**
     * @brief Return the fixed form content-type header.
     *
     * @return Form request headers.
     */
    std::map<std::string, std::string> form_headers() {
      return {{"Content-Type", "application/x-www-form-urlencoded"}};
    }

    /**
     * @brief Return Xbox authentication headers without any token values.
     *
     * @return Xbox JSON request headers.
     */
    std::map<std::string, std::string> xbox_headers() {
      return {
        {"Content-Type", "application/json"},
        {"x-xbl-contract-version", "1"},
        {"Cache-Control", "no-cache"},
        {"Origin", "https://www.xbox.com"},
        {"Referer", "https://www.xbox.com/"},
        {"Accept", "*/*"},
        {"ms-cv", "0"},
        {"User-Agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"},
      };
    }

    /**
     * @brief Parse an OAuth success response.
     *
     * @param response HTTP response.
     * @param now Injectable receipt time.
     * @param prior_refresh_token Refresh token retained when omitted by refresh.
     * @return Parsed OAuth credentials or a sanitized failure.
     */
    auth_result_t<oauth_credentials_t> parse_oauth_response(
      const http_response_t &response,
      std::chrono::system_clock::time_point now,
      std::string_view prior_refresh_token = {}
    ) {
      try {
        const auto document = json::parse(response.body);
        if (!document.is_object()) {
          return fail<oauth_credentials_t>(auth_error_e::invalid_response, "microsoft_token", "Microsoft token response must be an object", response.status_code);
        }
        const auto access = document.find("access_token");
        const auto refresh = document.find("refresh_token");
        const auto expires = document.find("expires_in");
        if (access == document.end() || !access->is_string() || access->get_ref<const std::string &>().empty() || expires == document.end() || !expires->is_number_integer()) {
          return fail<oauth_credentials_t>(auth_error_e::invalid_response, "microsoft_token", "Microsoft token response is incomplete", response.status_code);
        }
        const auto expires_in = expires->get<std::int64_t>();
        if (expires_in <= 0) {
          return fail<oauth_credentials_t>(auth_error_e::invalid_response, "microsoft_token", "Microsoft token expiry is invalid", response.status_code);
        }
        std::string refresh_value {prior_refresh_token};
        if (refresh != document.end()) {
          if (!refresh->is_string() || refresh->get_ref<const std::string &>().empty()) {
            return fail<oauth_credentials_t>(auth_error_e::invalid_response, "microsoft_token", "Microsoft refresh token is invalid", response.status_code);
          }
          refresh_value = refresh->get<std::string>();
        }
        if (refresh_value.empty()) {
          return fail<oauth_credentials_t>(auth_error_e::invalid_response, "microsoft_token", "Microsoft token response omitted the refresh token", response.status_code);
        }
        auth_result_t<oauth_credentials_t> result;
        result.value = {access->get<std::string>(), std::move(refresh_value), now + std::chrono::seconds {expires_in}};
        return result;
      } catch (const json::exception &) {
        return fail<oauth_credentials_t>(auth_error_e::invalid_response, "microsoft_token", "Microsoft token response contains invalid JSON", response.status_code);
      }
    }

    /**
     * @brief Parse the fixed error field of an OAuth polling response.
     *
     * @param body Response body.
     * @return Error identifier, or an empty string for malformed data.
     */
    std::string parse_oauth_error(std::string_view body) {
      try {
        const auto document = json::parse(body);
        if (document.is_object()) {
          const auto error = document.find("error");
          if (error != document.end() && error->is_string()) {
            return error->get<std::string>();
          }
        }
      } catch (const json::exception &) {
      }
      return {};
    }

    /**
     * @brief Parse an Xbox token and optional user hash.
     *
     * @param response HTTP response.
     * @param stage Fixed stage name.
     * @param require_user_hash Whether DisplayClaims.xui[0].uhs is required.
     * @return Token and user hash.
     */
    auth_result_t<std::pair<std::string, std::string>> parse_xbox_token(
      const http_response_t &response,
      std::string stage,
      bool require_user_hash
    ) {
      try {
        const auto document = json::parse(response.body);
        if (!document.is_object()) {
          return fail<std::pair<std::string, std::string>>(auth_error_e::invalid_response, std::move(stage), "Xbox token response must be an object", response.status_code);
        }
        const auto token = document.find("Token");
        if (token == document.end() || !token->is_string() || token->get_ref<const std::string &>().empty()) {
          return fail<std::pair<std::string, std::string>>(auth_error_e::invalid_response, std::move(stage), "Xbox token response is incomplete", response.status_code);
        }
        std::string user_hash;
        const auto claims = document.find("DisplayClaims");
        if (claims != document.end() && claims->is_object()) {
          const auto xui = claims->find("xui");
          if (xui != claims->end() && xui->is_array() && !xui->empty() && (*xui)[0].is_object()) {
            const auto hash = (*xui)[0].find("uhs");
            if (hash != (*xui)[0].end() && hash->is_string()) {
              user_hash = hash->get<std::string>();
            }
          }
        }
        if (require_user_hash && user_hash.empty()) {
          return fail<std::pair<std::string, std::string>>(auth_error_e::invalid_response, std::move(stage), "Xbox token response omitted the user hash", response.status_code);
        }
        auth_result_t<std::pair<std::string, std::string>> result;
        result.value = {token->get<std::string>(), std::move(user_hash)};
        return result;
      } catch (const json::exception &) {
        return fail<std::pair<std::string, std::string>>(auth_error_e::invalid_response, std::move(stage), "Xbox token response contains invalid JSON", response.status_code);
      }
    }

    /**
     * @brief Convert transport and HTTP failures into sanitized results.
     *
     * @tparam T Result value type.
     * @param response HTTP response.
     * @param stage Fixed stage name.
     * @return Failed result.
     */
    template<typename T>
    auth_result_t<T> response_failure(const http_response_t &response, std::string stage) {
      if (response.network_error) {
        return fail<T>(auth_error_e::network_error, std::move(stage), "authentication network request failed");
      }
      return fail<T>(auth_error_e::http_error, std::move(stage), "authentication service rejected the request", response.status_code);
    }

    /**
     * @brief Check cancellation before a network stage.
     *
     * @param cancelled Cancellation callback.
     * @return @c true when cancelled.
     */
    bool is_cancelled(const std::function<bool()> &cancelled) {
      return cancelled && cancelled();
    }
  }  // namespace

  client_t::client_t(http_client_t &http, runtime_t &runtime):
      http_(http),
      runtime_(runtime) {
  }

  auth_result_t<http_response_t> client_t::request_with_retry(
    std::string_view url,
    std::string_view body,
    const std::map<std::string, std::string> &headers,
    std::string stage,
    const std::function<bool()> &cancelled
  ) {
    for (std::size_t attempt = 0; attempt < maximum_request_attempts; ++attempt) {
      if (is_cancelled(cancelled)) {
        return fail<http_response_t>(auth_error_e::cancelled, std::move(stage), "authentication was cancelled");
      }
      auto response = http_.post(url, body, headers, request_timeout);
      if (!response.network_error) {
        auth_result_t<http_response_t> result;
        result.value = std::move(response);
        return result;
      }
      if (attempt + 1 < maximum_request_attempts && !runtime_.wait_for(retry_delay, cancelled)) {
        return fail<http_response_t>(auth_error_e::cancelled, std::move(stage), "authentication was cancelled");
      }
    }
    return fail<http_response_t>(auth_error_e::network_error, std::move(stage), "authentication network request failed");
  }

  auth_result_t<device_code_t> client_t::begin_device_code() {
    const std::string body = "client_id=" + form_encode(microsoft_client_id) + "&scope=" + form_encode(microsoft_scope);
    auto requested = request_with_retry(device_code_url, body, form_headers(), "device_code", {});
    if (!requested) {
      auth_result_t<device_code_t> result;
      result.error = std::move(requested.error);
      return result;
    }
    const auto &response = requested.value;
    if (response.status_code != 200) {
      return response_failure<device_code_t>(response, "device_code");
    }
    try {
      const auto document = json::parse(response.body);
      if (!document.is_object()) {
        return fail<device_code_t>(auth_error_e::invalid_response, "device_code", "Microsoft Device Code response must be an object", response.status_code);
      }
      const auto device = document.find("device_code");
      const auto user = document.find("user_code");
      const auto uri = document.find("verification_uri");
      const auto interval = document.find("interval");
      const auto expires = document.find("expires_in");
      if (device == document.end() || !device->is_string() || device->get_ref<const std::string &>().empty() || user == document.end() || !user->is_string() || user->get_ref<const std::string &>().empty() || uri == document.end() || !uri->is_string() || uri->get_ref<const std::string &>().empty() || interval == document.end() || !interval->is_number_integer() || expires == document.end() || !expires->is_number_integer()) {
        return fail<device_code_t>(auth_error_e::invalid_response, "device_code", "Microsoft Device Code response is incomplete", response.status_code);
      }
      const auto interval_seconds = interval->get<std::int64_t>();
      const auto expiry_seconds = expires->get<std::int64_t>();
      if (interval_seconds <= 0 || expiry_seconds <= 0) {
        return fail<device_code_t>(auth_error_e::invalid_response, "device_code", "Microsoft Device Code timing is invalid", response.status_code);
      }
      auth_result_t<device_code_t> result;
      result.value = {
        device->get<std::string>(),
        user->get<std::string>(),
        uri->get<std::string>(),
        std::chrono::seconds {interval_seconds},
        std::chrono::seconds {expiry_seconds},
        runtime_.steady_now(),
      };
      return result;
    } catch (const json::exception &) {
      return fail<device_code_t>(auth_error_e::invalid_response, "device_code", "Microsoft Device Code response contains invalid JSON", response.status_code);
    }
  }

  auth_result_t<authenticated_t> client_t::complete_device_code(const device_code_t &device_code, const std::function<bool()> &cancelled) {
    auto interval = device_code.polling_interval;
    const auto deadline = device_code.issued_at + device_code.expires_in;
    while (true) {
      if (is_cancelled(cancelled)) {
        return fail<authenticated_t>(auth_error_e::cancelled, "microsoft_token", "authentication was cancelled");
      }
      if (runtime_.steady_now() >= deadline) {
        return fail<authenticated_t>(auth_error_e::timeout, "microsoft_token", "Microsoft Device Code polling timed out");
      }
      const std::string body = "grant_type=" + form_encode("urn:ietf:params:oauth:grant-type:device_code") +
                               "&client_id=" + form_encode(microsoft_client_id) +
                               "&device_code=" + form_encode(device_code.device_code);
      const auto response = http_.post(token_url, body, form_headers(), request_timeout);
      if (response.network_error) {
        if (!runtime_.wait_for(interval, cancelled)) {
          return fail<authenticated_t>(auth_error_e::cancelled, "microsoft_token", "authentication was cancelled");
        }
        continue;
      }
      if (response.status_code == 200) {
        auto oauth = parse_oauth_response(response, runtime_.system_now());
        if (!oauth) {
          auth_result_t<authenticated_t> result;
          result.error = std::move(oauth.error);
          return result;
        }
        auto derived = derive_xbox_tokens(oauth.value, cancelled);
        if (!derived) {
          derived.value.oauth = std::move(oauth.value);
        }
        return derived;
      }

      const auto error = parse_oauth_error(response.body);
      if (error == "authorization_declined") {
        return fail<authenticated_t>(auth_error_e::authorization_declined, "microsoft_token", "Microsoft sign-in was declined", response.status_code);
      }
      if (error == "expired_token") {
        return fail<authenticated_t>(auth_error_e::code_expired, "microsoft_token", "Microsoft Device Code expired", response.status_code);
      }
      if (error != "authorization_pending" && error != "slow_down") {
        return response_failure<authenticated_t>(response, "microsoft_token");
      }
      if (error == "slow_down") {
        interval += 5s;
      }
      if (!runtime_.wait_for(interval, cancelled)) {
        return fail<authenticated_t>(auth_error_e::cancelled, "microsoft_token", "authentication was cancelled");
      }
    }
  }

  auth_result_t<authenticated_t> client_t::resume(oauth_credentials_t credentials, const std::function<bool()> &cancelled) {
    if (is_cancelled(cancelled)) {
      return fail<authenticated_t>(auth_error_e::cancelled, "microsoft_refresh", "authentication was cancelled");
    }
    if (credentials.refresh_token.empty()) {
      return fail<authenticated_t>(auth_error_e::invalid_response, "microsoft_refresh", "saved credentials do not contain a refresh token");
    }
    if (should_refresh(credentials, runtime_.system_now())) {
      const std::string body = "grant_type=refresh_token&client_id=" + form_encode(microsoft_client_id) +
                               "&refresh_token=" + form_encode(credentials.refresh_token) +
                               "&scope=" + form_encode(microsoft_scope);
      auto headers = form_headers();
      headers["Cache-Control"] = "no-store, must-revalidate, no-cache";
      auto requested = request_with_retry(token_url, body, headers, "microsoft_refresh", cancelled);
      if (!requested) {
        auth_result_t<authenticated_t> result;
        result.error = std::move(requested.error);
        result.value.oauth = std::move(credentials);
        return result;
      }
      const auto &response = requested.value;
      if (response.status_code != 200) {
        return response_failure<authenticated_t>(response, "microsoft_refresh");
      }
      auto refreshed = parse_oauth_response(response, runtime_.system_now(), credentials.refresh_token);
      if (!refreshed) {
        auth_result_t<authenticated_t> result;
        result.error = std::move(refreshed.error);
        result.error.stage = "microsoft_refresh";
        return result;
      }
      credentials = std::move(refreshed.value);
    }
    auto derived = derive_xbox_tokens(credentials, cancelled);
    if (!derived) {
      derived.value.oauth = std::move(credentials);
    }
    return derived;
  }

  auth_result_t<authenticated_t> client_t::derive_xbox_tokens(oauth_credentials_t credentials, const std::function<bool()> &cancelled) {
    const auto send_json = [&](std::string_view url, const json &body, std::string stage) -> auth_result_t<std::pair<std::string, std::string>> {
      if (is_cancelled(cancelled)) {
        return fail<std::pair<std::string, std::string>>(auth_error_e::cancelled, std::move(stage), "authentication was cancelled");
      }
      auto requested = request_with_retry(url, body.dump(), xbox_headers(), stage, cancelled);
      if (!requested) {
        auth_result_t<std::pair<std::string, std::string>> result;
        result.error = std::move(requested.error);
        return result;
      }
      const auto &response = requested.value;
      if (response.status_code != 200) {
        return response_failure<std::pair<std::string, std::string>>(response, std::move(stage));
      }
      return parse_xbox_token(response, std::move(stage), false);
    };

    const json user_body {
      {"Properties", {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"}, {"RpsTicket", "d=" + credentials.access_token}}},
      {"RelyingParty", "http://auth.xboxlive.com"},
      {"TokenType", "JWT"},
    };
    auto user = send_json(xbox_user_url, user_body, "xbox_user_token");
    if (!user) {
      auth_result_t<authenticated_t> result;
      result.error = std::move(user.error);
      return result;
    }

    const auto request_xsts = [&](std::string_view relying_party, std::string stage, bool require_hash) {
      const json body {
        {"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", {user.value.first}}}},
        {"RelyingParty", relying_party},
        {"TokenType", "JWT"},
      };
      if (is_cancelled(cancelled)) {
        return fail<std::pair<std::string, std::string>>(auth_error_e::cancelled, std::move(stage), "authentication was cancelled");
      }
      auto requested = request_with_retry(xsts_url, body.dump(), xbox_headers(), stage, cancelled);
      if (!requested) {
        auth_result_t<std::pair<std::string, std::string>> result;
        result.error = std::move(requested.error);
        return result;
      }
      const auto &response = requested.value;
      if (response.status_code != 200) {
        return response_failure<std::pair<std::string, std::string>>(response, std::move(stage));
      }
      return parse_xbox_token(response, std::move(stage), require_hash);
    };

    auto gssv_xsts = request_xsts("http://gssv.xboxlive.com/", "xsts_gssv", true);
    if (!gssv_xsts) {
      auth_result_t<authenticated_t> result;
      result.error = std::move(gssv_xsts.error);
      return result;
    }
    auto web_xsts = request_xsts("http://xboxlive.com", "xsts_web", false);
    if (!web_xsts) {
      auth_result_t<authenticated_t> result;
      result.error = std::move(web_xsts.error);
      return result;
    }
    if (is_cancelled(cancelled)) {
      return fail<authenticated_t>(auth_error_e::cancelled, "gssv_xhome", "authentication was cancelled");
    }

    const json gssv_body {{"token", gssv_xsts.value.first}, {"offeringId", "xhome"}};
    auto gssv_headers = xbox_headers();
    gssv_headers["Cache-Control"] = "no-store, must-revalidate, no-cache";
    gssv_headers["x-gssv-client"] = "XboxComBrowser";
    auto requested = request_with_retry(gssv_url, gssv_body.dump(), gssv_headers, "gssv_xhome", cancelled);
    if (!requested) {
      auth_result_t<authenticated_t> result;
      result.error = std::move(requested.error);
      return result;
    }
    const auto &response = requested.value;
    if (response.status_code != 200) {
      return response_failure<authenticated_t>(response, "gssv_xhome");
    }
    try {
      const auto document = json::parse(response.body);
      if (!document.is_object()) {
        return fail<authenticated_t>(auth_error_e::invalid_response, "gssv_xhome", "xHome GSSV response must be an object", response.status_code);
      }
      const auto gs_token = document.find("gsToken");
      const auto duration = document.find("durationInSeconds");
      const auto settings = document.find("offeringSettings");
      if (gs_token == document.end() || !gs_token->is_string() || gs_token->get_ref<const std::string &>().empty() || duration == document.end() || !duration->is_number_integer() || duration->get<std::int64_t>() <= 0 || settings == document.end() || !settings->is_object()) {
        return fail<authenticated_t>(auth_error_e::invalid_response, "gssv_xhome", "xHome GSSV response is incomplete", response.status_code);
      }
      const auto regions = settings->find("regions");
      if (regions == settings->end() || !regions->is_array() || regions->empty()) {
        return fail<authenticated_t>(auth_error_e::invalid_response, "gssv_xhome", "xHome GSSV response omitted regions", response.status_code);
      }
      std::string first_uri;
      std::string default_uri;
      for (const auto &region : *regions) {
        if (!region.is_object()) {
          continue;
        }
        const auto base_uri = region.find("baseUri");
        if (base_uri == region.end() || !base_uri->is_string() || base_uri->get_ref<const std::string &>().empty()) {
          continue;
        }
        if (first_uri.empty()) {
          first_uri = base_uri->get<std::string>();
        }
        const auto is_default = region.find("isDefault");
        if (is_default != region.end() && is_default->is_boolean() && is_default->get<bool>()) {
          default_uri = base_uri->get<std::string>();
          break;
        }
      }
      const auto base_uri = default_uri.empty() ? first_uri : default_uri;
      if (base_uri.empty()) {
        return fail<authenticated_t>(auth_error_e::invalid_response, "gssv_xhome", "xHome GSSV response contains no usable region", response.status_code);
      }

      auth_result_t<authenticated_t> result;
      result.value.oauth = std::move(credentials);
      result.value.session = {
        std::move(gssv_xsts.value.second),
        std::move(web_xsts.value.first),
        gs_token->get<std::string>(),
        base_uri,
        runtime_.system_now() + std::chrono::seconds {duration->get<std::int64_t>()},
      };
      return result;
    } catch (const json::exception &) {
      return fail<authenticated_t>(auth_error_e::invalid_response, "gssv_xhome", "xHome GSSV response contains invalid JSON", response.status_code);
    }
  }
}  // namespace xbox_remote::auth
