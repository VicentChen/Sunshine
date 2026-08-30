/**
 * @file src/xbox_remote/session.cpp
 * @brief Xbox Home console discovery and REST session lifecycle.
 */

#include "src/xbox_remote/session.h"

// standard includes
#include <algorithm>
#include <cctype>
#include <utility>

// library includes
#include <nlohmann/json.hpp>

namespace xbox_remote::session {
  namespace {
    using json = nlohmann::json;
    using namespace std::chrono_literals;

    constexpr auto request_timeout = 15s;
    constexpr auto retry_delay = 1s;
    constexpr std::size_t maximum_idempotent_attempts = 8;
    constexpr auto minimum_keepalive_interval = 5s;
    constexpr std::string_view xbox_command_url = "https://xccs.xboxlive.com/commands";

    /**
     * @brief Construct a sanitized Home REST failure.
     *
     * @tparam T Result value type.
     * @param code Error category.
     * @param stage Fixed stage name.
     * @param message Fixed non-secret message.
     * @param status Optional HTTP status.
     * @return Failed result.
     */
    template<typename T>
    result_t<T> fail(error_e code, std::string stage, std::string message, std::uint32_t status = 0) {
      result_t<T> result;
      result.error = {code, std::move(stage), status, std::move(message)};
      return result;
    }

    /**
     * @brief Map an unexpected HTTP status to a sanitized category.
     *
     * @tparam T Result value type.
     * @param stage Fixed stage name.
     * @param status HTTP status.
     * @return Failed result.
     */
    template<typename T>
    result_t<T> fail_http(std::string stage, std::uint32_t status) {
      if (status == 401 || status == 403) {
        return fail<T>(error_e::unauthorized, std::move(stage), "Xbox authorization was rejected", status);
      }
      if (status == 404 || status == 410) {
        return fail<T>(error_e::not_found, std::move(stage), "Xbox resource was not found", status);
      }
      if (status == 429) {
        return fail<T>(error_e::rate_limited, std::move(stage), "Xbox service rate limit was reached", status);
      }
      if (status >= 500 && status <= 599) {
        return fail<T>(error_e::server_error, std::move(stage), "Xbox service returned a server error", status);
      }
      return fail<T>(error_e::http_error, std::move(stage), "Xbox service rejected the request", status);
    }

    /**
     * @brief Check a successful HTTP status.
     *
     * @param status HTTP status.
     * @return @c true for status 200 through 299.
     */
    bool is_success(std::uint32_t status) {
      return status >= 200 && status <= 299;
    }

    /**
     * @brief Check whether an opaque identifier is safe as one URL path component.
     *
     * @param identifier Opaque service identifier.
     * @return @c true when non-empty and composed of unreserved URI characters.
     */
    bool valid_path_component(std::string_view identifier) {
      return !identifier.empty() && std::ranges::all_of(identifier, [](unsigned char value) {
        return std::isalnum(value) || value == '-' || value == '.' || value == '_' || value == '~';
      });
    }

    /**
     * @brief Check a canonical UUID used only as an XCCS command identifier.
     *
     * @param identifier Candidate UUID text.
     * @return @c true for lowercase or uppercase 8-4-4-4-12 hexadecimal form.
     */
    bool valid_command_session_id(std::string_view identifier) {
      if (identifier.size() != 36) {
        return false;
      }
      for (std::size_t index = 0; index < identifier.size(); ++index) {
        const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
        if (separator ? identifier[index] != '-' : std::isxdigit(static_cast<unsigned char>(identifier[index])) == 0) {
          return false;
        }
      }
      return true;
    }

    /**
     * @brief Normalize a trusted GSSV base URI.
     *
     * @param source URI from the in-memory GSSV context.
     * @return HTTPS URI without trailing slash, or an empty string when invalid.
     */
    std::string normalize_base_uri(std::string_view source) {
      if (!source.starts_with("https://") || source.find_first_of("?#") != std::string_view::npos) {
        return {};
      }
      while (source.ends_with('/')) {
        source.remove_suffix(1);
      }
      return source.size() > std::string_view("https://").size() ? std::string {source} : std::string {};
    }

    /**
     * @brief Build the Xbox compatibility device-info header.
     *
     * @param options Display and OS compatibility settings.
     * @return Compact JSON header value.
     */
    std::string make_device_info(const options_t &options) {
      return json {
        {"appInfo", {{"env", {
                               {"clientAppId", "www.xbox.com"},
                               {"clientAppType", "browser"},
                               {"clientAppVersion", "29.9.35"},
                               {"clientSdkVersion", "10.6.8"},
                               {"httpEnvironment", "prod"},
                               {"sdkInstallId", ""},
                             }}}},
        {"dev", {
                  {"hw", {{"make", "Microsoft"}, {"model", "unknown"}, {"platformType", "desktop"}, {"sdktype", "web"}}},
                  {"os", {{"name", options.os_name}, {"ver", "22631.2715"}, {"platform", "desktop"}}},
                  {"displayInfo", {
                                    {"dimensions", {{"widthInPixels", options.width}, {"heightInPixels", options.height}}},
                                    {"pixelDensity", {{"dpiX", 1}, {"dpiY", 1}}},
                                  }},
                  {"browser", {{"browserName", "edge"}, {"browserVersion", "140.0.3485.66"}}},
                }},
      }
        .dump();
    }
  }  // namespace

  client_t::client_t(
    auth::http_client_t &http,
    auth::runtime_t &runtime,
    auth::session_context_t &context,
    refresh_callback_t refresh
  ):
      http_(http),
      runtime_(runtime),
      context_(context),
      refresh_(std::move(refresh)) {
  }

  result_t<auth::http_response_t> client_t::request(
    method_e method,
    std::string path,
    std::string body,
    std::map<std::string, std::string> headers,
    std::string stage,
    const std::function<bool()> &cancelled
  ) {
    bool refresh_attempted = false;
    std::size_t network_attempts = 0;
    while (true) {
      if (cancelled && cancelled()) {
        return fail<auth::http_response_t>(error_e::cancelled, std::move(stage), "Xbox REST operation was cancelled");
      }
      const auto base_uri = normalize_base_uri(context_.base_uri);
      if (base_uri.empty() || context_.gs_token.empty()) {
        return fail<auth::http_response_t>(error_e::invalid_response, std::move(stage), "Xbox session context is incomplete");
      }
      headers["Authorization"] = "Bearer " + context_.gs_token;
      auth::http_response_t response;
      if (method == method_e::post) {
        response = http_.post(base_uri + path, body, headers, request_timeout);
      } else if (method == method_e::remove) {
        response = http_.remove(base_uri + path, headers, request_timeout);
      } else {
        response = http_.get(base_uri + path, headers, request_timeout);
      }
      if (response.network_error) {
        ++network_attempts;
        // A lost POST response is ambiguous and replaying it could duplicate a session or signaling action.
        if (method == method_e::post || network_attempts >= maximum_idempotent_attempts) {
          return fail<auth::http_response_t>(error_e::network_error, std::move(stage), "Xbox REST network request failed");
        }
        if (!runtime_.wait_for(retry_delay, cancelled)) {
          return fail<auth::http_response_t>(error_e::cancelled, std::move(stage), "Xbox REST operation was cancelled");
        }
        continue;
      }
      if (response.status_code != 401) {
        result_t<auth::http_response_t> result;
        result.http_status = response.status_code;
        result.value = std::move(response);
        return result;
      }
      if (refresh_attempted || !refresh_ || !refresh_(context_)) {
        return fail<auth::http_response_t>(error_e::unauthorized, std::move(stage), "Xbox authorization refresh failed", 401);
      }
      refresh_attempted = true;
      network_attempts = 0;
    }
  }

  result_t<std::vector<protocol::console_t>> client_t::discover(const std::function<bool()> &cancelled) {
    auto response = request(
      method_e::get,
      "/v6/servers/home?mr=50",
      {},
      {{"Accept", "application/json"}, {"Content-Type", "application/json"}, {"X-MS-Device-Info", make_device_info(options_t {})}},
      "discover",
      cancelled
    );
    if (!response) {
      result_t<std::vector<protocol::console_t>> result;
      result.error = std::move(response.error);
      return result;
    }
    if (response.value.status_code != 200) {
      return fail_http<std::vector<protocol::console_t>>("discover", response.value.status_code);
    }
    auto consoles = protocol::parse_console_list(response.value.body);
    if (!consoles) {
      return fail<std::vector<protocol::console_t>>(error_e::invalid_response, "discover", "Xbox console list response was invalid", response.value.status_code);
    }
    result_t<std::vector<protocol::console_t>> result;
    result.http_status = response.value.status_code;
    result.value = std::move(consoles.value);
    return result;
  }

  result_t<protocol::console_t> client_t::select_console(
    const std::vector<protocol::console_t> &consoles,
    std::string_view server_id
  ) const {
    if (server_id.empty()) {
      if (consoles.empty()) {
        return fail<protocol::console_t>(error_e::not_found, "select_console", "No Xbox Home console was discovered");
      }
      if (consoles.size() != 1) {
        return fail<protocol::console_t>(error_e::ambiguous_console, "select_console", "More than one Xbox Home console was discovered");
      }
      result_t<protocol::console_t> result;
      result.value = consoles.front();
      return result;
    }
    std::optional<protocol::console_t> selected;
    for (const auto &console : consoles) {
      if (console.server_id != server_id) {
        continue;
      }
      if (selected) {
        return fail<protocol::console_t>(error_e::duplicate_console, "select_console", "Xbox server identifier is duplicated");
      }
      selected = console;
    }
    if (!selected) {
      return fail<protocol::console_t>(error_e::not_found, "select_console", "Xbox console was not found");
    }
    result_t<protocol::console_t> result;
    result.value = std::move(*selected);
    return result;
  }

  result_t<protocol::console_t> client_t::wake_and_wait(
    const protocol::console_t &console,
    const wake_options_t &options,
    const std::function<bool()> &cancelled
  ) {
    if (console.server_id.empty() || !valid_command_session_id(options.command_session_id) || options.poll_interval <= 0s || options.wake_timeout <= 0s) {
      return fail<protocol::console_t>(error_e::invalid_response, "wake_precondition", "Xbox wake request is invalid");
    }
    if (console.power_state == "On") {
      result_t<protocol::console_t> result;
      result.value = console;
      return result;
    }
    if (console.power_state != "ConnectedStandby") {
      return fail<protocol::console_t>(error_e::failed_state, "wake_precondition", "Xbox console is not in a wakeable standby state");
    }

    bool refresh_attempted = false;
    std::optional<failure_t> ambiguous_failure;
    std::uint32_t command_http_status = 0;
    while (true) {
      if (cancelled && cancelled()) {
        return fail<protocol::console_t>(error_e::cancelled, "wake_command", "Xbox wake operation was cancelled");
      }
      if (context_.user_hash.empty() || context_.web_token.empty()) {
        return fail<protocol::console_t>(error_e::invalid_response, "wake_command", "Xbox remote-management context is incomplete");
      }
      const auto body = json {
        {"destination", "Xbox"},
        {"type", "Power"},
        {"command", "WakeUp"},
        {"sessionId", options.command_session_id},
        {"sourceId", "com.microsoft.smartglass"},
        {"parameters", json::array()},
        {"linkedXboxId", console.server_id},
      }
                          .dump();
      const std::map<std::string, std::string> headers {
        {"Authorization", "XBL3.0 x=" + context_.user_hash + ';' + context_.web_token},
        {"Accept-Language", "en-US"},
        {"Content-Type", "application/json"},
        {"skillplatform", "RemoteManagement"},
        {"x-xbl-contract-version", "4"},
        {"x-xbl-client-name", "XboxApp"},
        {"x-xbl-client-type", "UWA"},
        {"x-xbl-client-version", "39.39.22001.0"},
      };
      auto response = http_.post(xbox_command_url, body, headers, request_timeout);
      if (response.network_error) {
        ambiguous_failure = failure_t {error_e::network_error, "wake_command", 0, "Xbox wake command outcome is unknown after a network failure"};
        break;
      }
      if (response.status_code == 401) {
        if (refresh_attempted || !refresh_ || !refresh_(context_)) {
          return fail<protocol::console_t>(error_e::unauthorized, "wake_command", "Xbox authorization refresh failed", 401);
        }
        refresh_attempted = true;
        continue;
      }
      if (!is_success(response.status_code)) {
        return fail_http<protocol::console_t>("wake_command", response.status_code);
      }
      command_http_status = response.status_code;
      break;
    }

    const auto deadline = runtime_.steady_now() + options.wake_timeout;
    while (runtime_.steady_now() < deadline) {
      auto consoles = discover(cancelled);
      if (!consoles) {
        result_t<protocol::console_t> result;
        result.error = std::move(consoles.error);
        return result;
      }
      auto selected = select_console(consoles.value, console.server_id);
      if (selected && selected.value.power_state == "On") {
        selected.http_status = command_http_status;
        return selected;
      }
      if (!selected && selected.error.code != error_e::not_found) {
        return selected;
      }
      if (!runtime_.wait_for(options.poll_interval, cancelled)) {
        return fail<protocol::console_t>(error_e::cancelled, "wake_state", "Xbox wake operation was cancelled");
      }
    }
    if (ambiguous_failure) {
      result_t<protocol::console_t> result;
      result.error = std::move(*ambiguous_failure);
      return result;
    }
    return fail<protocol::console_t>(error_e::timeout, "wake_state", "Xbox console did not report On before the wake deadline");
  }

  result_t<provisioned_t> client_t::create_and_wait(
    std::string_view server_id,
    const options_t &options,
    const std::function<bool()> &cancelled
  ) {
    if (server_id.empty() || options.width == 0 || options.height == 0 || options.poll_interval <= 0s || options.provision_timeout <= 0s) {
      return fail<provisioned_t>(error_e::invalid_response, "create_session", "Home session options are invalid");
    }
    protocol::home_play_request_t play;
    play.server_id = std::string {server_id};
    play.settings.locale = options.locale;
    play.settings.os_name = options.os_name;
    auto created_response = request(
      method_e::post,
      "/v5/sessions/home/play",
      protocol::serialize_home_play_request(play),
      {{"Accept", "application/json"}, {"Content-Type", "application/json"}, {"X-MS-Device-Info", make_device_info(options)}},
      "create_session",
      cancelled
    );
    if (!created_response) {
      result_t<provisioned_t> result;
      result.error = std::move(created_response.error);
      return result;
    }
    if (!is_success(created_response.value.status_code)) {
      return fail_http<provisioned_t>("create_session", created_response.value.status_code);
    }
    auto created = protocol::parse_session_created(created_response.value.body);
    if (!created || !valid_path_component(created.value.session_id)) {
      return fail<provisioned_t>(error_e::invalid_response, "create_session", "Home session creation response was invalid", created_response.value.status_code);
    }
    const auto session_id = created.value.session_id;
    const auto fail_after_creation = [&](failure_t error) {
      static_cast<void>(delete_session(session_id, {}));
      result_t<provisioned_t> result;
      result.error = std::move(error);
      return result;
    };

    const auto deadline = runtime_.steady_now() + options.provision_timeout;
    while (runtime_.steady_now() < deadline) {
      if (cancelled && cancelled()) {
        return fail_after_creation({error_e::cancelled, "session_state", 0, "Home session provisioning was cancelled"});
      }
      auto state_response = request(
        method_e::get,
        "/v5/sessions/home/" + session_id + "/state",
        {},
        {{"Content-Type", "application/json"}},
        "session_state",
        cancelled
      );
      if (!state_response) {
        return fail_after_creation(std::move(state_response.error));
      }
      if (state_response.value.status_code != 200) {
        return fail_after_creation(fail_http<provisioned_t>("session_state", state_response.value.status_code).error);
      }
      auto state = protocol::parse_session_state(state_response.value.body);
      if (!state) {
        return fail_after_creation({error_e::invalid_response, "session_state", state_response.value.status_code, "Home session state response was invalid"});
      }
      if (protocol::is_home_session_provisioned(state.value)) {
        auto config_response = request(
          method_e::get,
          "/v5/sessions/home/" + session_id + "/configuration",
          {},
          {{"Content-Type", "application/json"}},
          "session_configuration",
          cancelled
        );
        if (!config_response) {
          return fail_after_creation(std::move(config_response.error));
        }
        if (config_response.value.status_code != 200) {
          return fail_after_creation(fail_http<provisioned_t>("session_configuration", config_response.value.status_code).error);
        }
        auto configuration = protocol::parse_session_configuration(config_response.value.body);
        if (!configuration) {
          return fail_after_creation({
            error_e::invalid_response,
            "session_configuration",
            config_response.value.status_code,
            "Home session configuration response was invalid at " + configuration.error.field,
          });
        }
        result_t<provisioned_t> result;
        result.value.session_id = session_id;
        result.value.configuration = std::move(configuration.value);
        result.value.next_keepalive = runtime_.steady_now() + std::max(minimum_keepalive_interval, std::chrono::seconds {result.value.configuration.keepalive_seconds});
        result.value.create_http_status = created_response.value.status_code;
        result.value.state_http_status = state_response.value.status_code;
        result.value.configuration_http_status = config_response.value.status_code;
        result.http_status = config_response.value.status_code;
        return result;
      }
      if (state.value.state == "Failed" || state.value.state == "Error") {
        auto message = std::string {"Home session provisioning failed"};
        if (!state.value.failure_code.empty()) {
          message += " (service code " + state.value.failure_code + ')';
        }
        return fail_after_creation({error_e::failed_state, "session_state", 0, std::move(message)});
      }
      if (!runtime_.wait_for(options.poll_interval, cancelled)) {
        return fail_after_creation({error_e::cancelled, "session_state", 0, "Home session provisioning was cancelled"});
      }
    }
    return fail_after_creation({error_e::timeout, "session_state", 0, "Home session provisioning timed out"});
  }

  result_t<bool> client_t::send_sdp(
    std::string_view session_id,
    const protocol::sdp_offer_t &offer,
    const std::function<bool()> &cancelled
  ) {
    if (!valid_path_component(session_id) || offer.sdp.empty()) {
      return fail<bool>(error_e::invalid_response, "send_sdp", "SDP request is invalid");
    }
    auto response = request(
      method_e::post,
      "/v5/sessions/home/" + std::string {session_id} + "/sdp",
      protocol::serialize_sdp_offer(offer),
      {{"Accept", "application/json"}, {"Content-Type", "application/json"}},
      "send_sdp",
      cancelled
    );
    if (!response) {
      result_t<bool> result;
      result.error = std::move(response.error);
      return result;
    }
    if (!is_success(response.value.status_code)) {
      return fail_http<bool>("send_sdp", response.value.status_code);
    }
    result_t<bool> result;
    result.value = true;
    result.http_status = response.value.status_code;
    return result;
  }

  result_t<std::optional<std::string>> client_t::poll_sdp(
    std::string_view session_id,
    const std::function<bool()> &cancelled
  ) {
    return poll_exchange(session_id, "sdp", "poll_sdp", cancelled);
  }

  result_t<bool> client_t::send_ice(
    std::string_view session_id,
    const std::vector<protocol::ice_candidate_t> &candidates,
    const std::function<bool()> &cancelled
  ) {
    if (!valid_path_component(session_id)) {
      return fail<bool>(error_e::invalid_response, "send_ice", "ICE request has an invalid session identifier");
    }
    auto response = request(
      method_e::post,
      "/v5/sessions/home/" + std::string {session_id} + "/ice",
      protocol::serialize_ice_candidates(candidates),
      {{"Accept", "application/json"}, {"Content-Type", "application/json"}},
      "send_ice",
      cancelled
    );
    if (!response) {
      result_t<bool> result;
      result.error = std::move(response.error);
      return result;
    }
    if (!is_success(response.value.status_code)) {
      return fail_http<bool>("send_ice", response.value.status_code);
    }
    result_t<bool> result;
    result.value = true;
    result.http_status = response.value.status_code;
    return result;
  }

  result_t<std::optional<std::string>> client_t::poll_ice(
    std::string_view session_id,
    const std::function<bool()> &cancelled
  ) {
    return poll_exchange(session_id, "ice", "poll_ice", cancelled);
  }

  result_t<std::optional<std::string>> client_t::poll_exchange(
    std::string_view session_id,
    std::string_view endpoint,
    std::string stage,
    const std::function<bool()> &cancelled
  ) {
    if (!valid_path_component(session_id)) {
      return fail<std::optional<std::string>>(error_e::invalid_response, std::move(stage), "Exchange request has an invalid session identifier");
    }
    auto response = request(
      method_e::get,
      "/v5/sessions/home/" + std::string {session_id} + "/" + std::string {endpoint},
      {},
      {{"Content-Type", "application/json"}},
      stage,
      cancelled
    );
    if (!response) {
      result_t<std::optional<std::string>> result;
      result.error = std::move(response.error);
      return result;
    }
    if (response.value.status_code == 204) {
      result_t<std::optional<std::string>> result;
      result.http_status = response.value.status_code;
      return result;
    }
    if (response.value.status_code != 200) {
      return fail_http<std::optional<std::string>>(std::move(stage), response.value.status_code);
    }
    auto exchange = protocol::parse_exchange_response(response.value.body);
    if (!exchange) {
      return fail<std::optional<std::string>>(error_e::invalid_response, std::move(stage), "Signaling exchange response was invalid", response.value.status_code);
    }
    result_t<std::optional<std::string>> result;
    result.value = std::move(exchange.value.exchange_response);
    result.http_status = response.value.status_code;
    return result;
  }

  result_t<bool> client_t::keepalive_if_due(
    provisioned_t &session,
    const std::function<bool()> &cancelled
  ) {
    if (!valid_path_component(session.session_id) || session.configuration.keepalive_seconds == 0) {
      return fail<bool>(error_e::invalid_response, "keepalive", "Keepalive session state is invalid");
    }
    if (runtime_.steady_now() < session.next_keepalive) {
      return {};
    }
    auto response = request(
      method_e::post,
      "/v5/sessions/home/" + session.session_id + "/keepalive",
      "{}",
      {{"Accept", "application/json"}, {"Content-Type", "application/json"}},
      "keepalive",
      cancelled
    );
    if (!response) {
      result_t<bool> result;
      result.error = std::move(response.error);
      return result;
    }
    if (!is_success(response.value.status_code)) {
      return fail_http<bool>("keepalive", response.value.status_code);
    }
    session.next_keepalive = runtime_.steady_now() + std::max(minimum_keepalive_interval, std::chrono::seconds {session.configuration.keepalive_seconds});
    result_t<bool> result;
    result.value = true;
    result.http_status = response.value.status_code;
    return result;
  }

  result_t<bool> client_t::delete_session(
    std::string_view session_id,
    const std::function<bool()> &cancelled
  ) {
    if (session_id.empty()) {
      result_t<bool> result;
      result.value = true;
      return result;
    }
    if (!valid_path_component(session_id)) {
      return fail<bool>(error_e::invalid_response, "delete_session", "Session identifier is invalid");
    }
    auto response = request(
      method_e::remove,
      "/v5/sessions/home/" + std::string {session_id},
      {},
      {},
      "delete_session",
      cancelled
    );
    if (!response) {
      result_t<bool> result;
      result.error = std::move(response.error);
      return result;
    }
    if (!is_success(response.value.status_code) && response.value.status_code != 404 && response.value.status_code != 410) {
      return fail_http<bool>("delete_session", response.value.status_code);
    }
    result_t<bool> result;
    result.value = true;
    result.http_status = response.value.status_code;
    return result;
  }
}  // namespace xbox_remote::session
