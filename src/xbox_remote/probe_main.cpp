/**
 * @file src/xbox_remote/probe_main.cpp
 * @brief Command-line authentication and Home REST probe for Xbox Remote Play.
 */

// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "src/utility.h"
#include "src/uuid.h"
#include "src/xbox_remote/auth.h"
#include "src/xbox_remote/http_runtime.h"
#include "src/xbox_remote/input_queue.h"
#include "src/xbox_remote/session.h"
#include "src/xbox_remote/startup.h"
#include "src/xbox_remote/token_store.h"
#include "src/xbox_remote/transport.h"

namespace {
  volatile std::sig_atomic_t cancelled = 0;  ///< Set by the process interrupt handler.

  /**
   * @brief Request cancellation without performing unsafe signal work.
   *
   * @param signal Received signal number.
   */
  void cancel_probe(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
      cancelled = 1;
    }
  }

  /**
   * @brief Print command usage without revealing a default credential path.
   *
   * @return Process usage-error code.
   */
  int print_usage() {
    std::cerr << "Usage:\n"
                 "  xbox-remote-probe <login|resume|consoles> <token-file>\n"
                 "  xbox-remote-probe wake-check <token-file> <console-number> [timeout-seconds]\n"
                 "  xbox-remote-probe session-check <token-file> <console-number> [repeat-count]\n"
                 "  xbox-remote-probe transport-check <token-file> <console-number> [hold-seconds]\n"
                 "  xbox-remote-probe startup-check <token-file> <console-number> [hold-seconds]\n"
                 "  xbox-remote-probe soak-check <token-file> <console-number> [hold-seconds]\n"
                 "  xbox-remote-probe input-check <token-file> <console-number> <case>\n"
                 "Input cases: neutral, a, dpad-up, left-x-negative, left-x-positive,\n"
                 "  left-y-negative, left-y-positive, right-x-negative, right-x-positive,\n"
                 "  right-y-negative, right-y-positive, lt, rt, matrix, vibration\n";
    return 2;
  }

  /**
   * @brief Print a sanitized authentication failure.
   *
   * @param failure Failure containing fixed messages and status only.
   * @return Process failure code.
   */
  int print_failure(const xbox_remote::auth::auth_failure_t &failure) {
    std::cerr << "Authentication failed at " << failure.stage;
    if (failure.http_status != 0) {
      std::cerr << " (HTTP " << failure.http_status << ')';
    }
    std::cerr << ": " << failure.message << '\n';
    return 1;
  }

  /**
   * @brief Print a sanitized Home REST failure.
   *
   * @param failure Failure containing fixed messages and status only.
   * @return Process failure code.
   */
  int print_failure(const xbox_remote::session::failure_t &failure) {
    std::cerr << "Xbox Home REST operation failed at " << failure.stage;
    if (failure.http_status != 0) {
      std::cerr << " (HTTP " << failure.http_status << ')';
    }
    std::cerr << ": " << failure.message << '\n';
    return 1;
  }

  /**
   * @brief Print a sanitized WebRTC transport failure.
   *
   * @param failure Failure containing fixed stage and message text.
   * @return Process failure code.
   */
  int print_failure(const xbox_remote::transport::failure_t &failure) {
    std::cerr << "Xbox WebRTC transport failed at " << failure.stage << ": " << failure.message << '\n';
    return 1;
  }

  /**
   * @brief Print a sanitized data-channel startup failure.
   *
   * @param failure Failure containing fixed stage and message text.
   * @return Process failure code.
   */
  int print_failure(const xbox_remote::startup::failure_t &failure) {
    std::cerr << "Xbox startup handshake failed at " << failure.stage << ": " << failure.message << '\n';
    return 1;
  }

  /**
   * @brief Adapt the live WebRTC peer to the startup coordinator sender.
   */
  class startup_sender_t final: public xbox_remote::startup::sender_t {
  public:
    /**
     * @brief Construct an adapter for one connected peer.
     *
     * @param peer Peer that outlives this adapter.
     */
    explicit startup_sender_t(xbox_remote::transport::peer_t &peer):
        peer_(peer) {
    }

    /**
     * @brief Forward a text startup message to the connected peer.
     *
     * @param channel Negotiated channel label.
     * @param payload UTF-8 protocol payload.
     * @return Whether the peer accepted the message.
     */
    bool send_text(std::string_view channel, std::string_view payload) override {
      return peer_.send_text(channel, payload);
    }

    /**
     * @brief Forward a binary startup message to the connected peer.
     *
     * @param channel Negotiated channel label.
     * @param payload Binary protocol payload.
     * @return Whether the peer accepted the message.
     */
    bool send_binary(std::string_view channel, const std::vector<std::uint8_t> &payload) override {
      return peer_.send_binary(channel, payload);
    }

  private:
    xbox_remote::transport::peer_t &peer_;  ///< Non-owning connected transport.
  };

  /**
   * @brief Print a sanitized token-store failure.
   *
   * @param operation Fixed operation name.
   * @param result Sanitized token-store result.
   * @return Process failure code.
   */
  int print_store_failure(std::string_view operation, const xbox_remote::auth::token_store_result_t &result) {
    std::cerr << operation << " failed: " << result.message << '\n';
    return 1;
  }

  /**
   * @brief Parse one bounded positive command-line integer.
   *
   * @param source Decimal input.
   * @param maximum Inclusive upper bound.
   * @return Parsed value or no value for malformed/out-of-range input.
   */
  std::optional<std::uint32_t> parse_positive(std::string_view source, std::uint32_t maximum) {
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(source.data(), source.data() + source.size(), value);
    if (parsed.ec != std::errc {} || parsed.ptr != source.data() + source.size() || value == 0 || value > maximum) {
      return std::nullopt;
    }
    return value;
  }

  /**
   * @brief Make a service-provided console label safe for terminal output.
   *
   * @param source Console name or service label.
   * @return Printable label limited to 80 bytes.
   */
  std::string printable_label(std::string_view source) {
    constexpr std::size_t maximum_label_size = 80;
    std::string result;
    result.reserve(std::min(source.size(), maximum_label_size));
    for (const unsigned char value : source.substr(0, maximum_label_size)) {
      result.push_back(value >= 0x20 && value != 0x7f ? static_cast<char>(value) : '?');
    }
    return result.empty() ? "Unnamed Xbox" : result;
  }

  /**
   * @brief Count entries in a Linux process resource directory.
   *
   * @param path Process directory such as @c /proc/self/fd or @c /proc/self/task.
   * @return Entry count, or no value when the platform does not expose the directory.
   */
  std::optional<std::size_t> count_process_resources(const std::filesystem::path &path) {
    std::error_code error;
    std::filesystem::directory_iterator iterator {path, error};
    if (error) {
      return std::nullopt;
    }
    std::size_t count = 0;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
      ++count;
      iterator.increment(error);
      if (error) {
        return std::nullopt;
      }
    }
    return count;
  }

  /**
   * @brief Read the resident set size reported by Linux procfs.
   *
   * @return Resident memory in KiB, or no value when procfs is unavailable.
   */
  std::optional<std::size_t> resident_set_kib() {
    std::ifstream status {"/proc/self/status"};
    std::string line;
    constexpr std::string_view prefix = "VmRSS:";
    while (std::getline(status, line)) {
      if (!line.starts_with(prefix)) {
        continue;
      }
      std::string_view value {line};
      value.remove_prefix(prefix.size());
      const auto first_digit = value.find_first_not_of(" \t");
      if (first_digit == std::string_view::npos) {
        return std::nullopt;
      }
      value.remove_prefix(first_digit);
      std::size_t rss = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), rss);
      return parsed.ec == std::errc {} ? std::optional<std::size_t> {rss} : std::nullopt;
    }
    return std::nullopt;
  }

  /**
   * @brief Update an optional high-water mark from a procfs sample.
   *
   * @param high_water Current high-water mark.
   * @param sample Latest sample.
   */
  void update_high_water(std::optional<std::size_t> &high_water, const std::optional<std::size_t> &sample) {
    if (sample && (!high_water || *sample > *high_water)) {
      high_water = sample;
    }
  }

  /**
   * @brief User-observable scripted input cases for the Step 06 live matrix.
   */
  enum class input_case_e {
    neutral,  ///< Send only neutral absolute states.
    a,  ///< Press and release the A button.
    dpad_up,  ///< Press and release D-pad Up.
    left_x_negative,  ///< Exercise the negative left-stick X direction.
    left_x_positive,  ///< Exercise the positive left-stick X direction.
    left_y_negative,  ///< Exercise the negative left-stick Y direction.
    left_y_positive,  ///< Exercise the positive left-stick Y direction.
    right_x_negative,  ///< Exercise the negative right-stick X direction.
    right_x_positive,  ///< Exercise the positive right-stick X direction.
    right_y_negative,  ///< Exercise the negative right-stick Y direction.
    right_y_positive,  ///< Exercise the positive right-stick Y direction.
    left_trigger,  ///< Exercise the full-range left trigger.
    right_trigger,  ///< Exercise the full-range right trigger.
    matrix,  ///< Run every mapping case with neutral gaps.
    vibration,  ///< Send neutral states and observe feedback for 60 seconds.
  };

  /**
   * @brief One repeated absolute input phase in a live scripted case.
   */
  struct input_action_t {
    std::string_view label;  ///< Fixed user-visible phase label.
    xbox_remote::protocol::gamepad_frame_t frame;  ///< Absolute state repeated during the phase.
    std::chrono::milliseconds duration;  ///< Bounded phase duration.
  };

  /**
   * @brief Parse a fixed live input case name.
   *
   * @param name Command-line case name.
   * @return Parsed case or no value for an unsupported name.
   */
  std::optional<input_case_e> parse_input_case(std::string_view name) {
    using pair_t = std::pair<std::string_view, input_case_e>;
    constexpr std::array cases {
      pair_t {"neutral", input_case_e::neutral},
      pair_t {"a", input_case_e::a},
      pair_t {"dpad-up", input_case_e::dpad_up},
      pair_t {"left-x-negative", input_case_e::left_x_negative},
      pair_t {"left-x-positive", input_case_e::left_x_positive},
      pair_t {"left-y-negative", input_case_e::left_y_negative},
      pair_t {"left-y-positive", input_case_e::left_y_positive},
      pair_t {"right-x-negative", input_case_e::right_x_negative},
      pair_t {"right-x-positive", input_case_e::right_x_positive},
      pair_t {"right-y-negative", input_case_e::right_y_negative},
      pair_t {"right-y-positive", input_case_e::right_y_positive},
      pair_t {"lt", input_case_e::left_trigger},
      pair_t {"rt", input_case_e::right_trigger},
      pair_t {"matrix", input_case_e::matrix},
      pair_t {"vibration", input_case_e::vibration},
    };
    const auto found = std::ranges::find(cases, name, &pair_t::first);
    return found == cases.end() ? std::nullopt : std::optional {found->second};
  }

  /**
   * @brief Build bounded active/neutral phases for one live input case.
   *
   * Axis names deliberately describe wire sign rather than presumed screen
   * direction; the real-console observation decides the final Y mapping.
   *
   * @param selected Selected scripted case.
   * @return Ordered absolute-state phases.
   */
  std::vector<input_action_t> make_input_actions(input_case_e selected) {
    using namespace std::chrono_literals;
    using xbox_remote::protocol::gamepad_button_e;
    using xbox_remote::protocol::gamepad_frame_t;

    const auto active_frame = [](input_case_e input_case) {
      gamepad_frame_t frame;
      constexpr std::int16_t axis = 24576;
      switch (input_case) {
        case input_case_e::a:
          frame.button_mask = static_cast<std::uint16_t>(gamepad_button_e::a);
          break;
        case input_case_e::dpad_up:
          frame.button_mask = static_cast<std::uint16_t>(gamepad_button_e::dpad_up);
          break;
        case input_case_e::left_x_negative:
          frame.left_stick_x = -axis;
          break;
        case input_case_e::left_x_positive:
          frame.left_stick_x = axis;
          break;
        case input_case_e::left_y_negative:
          frame.left_stick_y = -axis;
          break;
        case input_case_e::left_y_positive:
          frame.left_stick_y = axis;
          break;
        case input_case_e::right_x_negative:
          frame.right_stick_x = -axis;
          break;
        case input_case_e::right_x_positive:
          frame.right_stick_x = axis;
          break;
        case input_case_e::right_y_negative:
          frame.right_stick_y = -axis;
          break;
        case input_case_e::right_y_positive:
          frame.right_stick_y = axis;
          break;
        case input_case_e::left_trigger:
          frame.left_trigger = 0xffff;
          break;
        case input_case_e::right_trigger:
          frame.right_trigger = 0xffff;
          break;
        case input_case_e::neutral:
        case input_case_e::matrix:
        case input_case_e::vibration:
          break;
      }
      frame.physical_physicality = xbox_remote::input::activity_mask(frame);
      return frame;
    };

    const auto label = [](input_case_e input_case) -> std::string_view {
      switch (input_case) {
        case input_case_e::neutral:
          return "neutral";
        case input_case_e::a:
          return "A pressed";
        case input_case_e::dpad_up:
          return "D-pad Up pressed";
        case input_case_e::left_x_negative:
          return "left stick X negative";
        case input_case_e::left_x_positive:
          return "left stick X positive";
        case input_case_e::left_y_negative:
          return "left stick Y negative";
        case input_case_e::left_y_positive:
          return "left stick Y positive";
        case input_case_e::right_x_negative:
          return "right stick X negative";
        case input_case_e::right_x_positive:
          return "right stick X positive";
        case input_case_e::right_y_negative:
          return "right stick Y negative";
        case input_case_e::right_y_positive:
          return "right stick Y positive";
        case input_case_e::left_trigger:
          return "left trigger full";
        case input_case_e::right_trigger:
          return "right trigger full";
        case input_case_e::matrix:
          return "matrix";
        case input_case_e::vibration:
          return "vibration observation";
      }
      return "unknown";
    };

    std::vector<input_action_t> actions;
    if (selected == input_case_e::neutral || selected == input_case_e::vibration) {
      actions.push_back({"neutral", {}, selected == input_case_e::neutral ? 5s : 2s});
      return actions;
    }
    constexpr std::array matrix_cases {
      input_case_e::a,
      input_case_e::dpad_up,
      input_case_e::left_x_negative,
      input_case_e::left_x_positive,
      input_case_e::left_y_negative,
      input_case_e::left_y_positive,
      input_case_e::right_x_negative,
      input_case_e::right_x_positive,
      input_case_e::right_y_negative,
      input_case_e::right_y_positive,
      input_case_e::left_trigger,
      input_case_e::right_trigger,
    };
    actions.push_back({"neutral baseline", {}, 3s});
    const auto append_case = [&](input_case_e input_case) {
      actions.push_back({label(input_case), active_frame(input_case), 2s});
      actions.push_back({"neutral release", {}, 1s});
    };
    if (selected == input_case_e::matrix) {
      for (const auto input_case : matrix_cases) {
        append_case(input_case);
      }
    } else {
      append_case(selected);
      actions.back().duration = 2s;
    }
    return actions;
  }
}  // namespace

/**
 * @brief Run authentication, discovery, Home lifecycle, transport, or startup checks.
 *
 * @param argc Argument count.
 * @param argv Command, explicit token-file path, and optional session-check parameters.
 * @return Zero on success, one on operation failure, or two on invalid arguments.
 */
int main(int argc, char *argv[]) {
  using namespace xbox_remote;
  using namespace std::chrono;

  if (argc < 3) {
    return print_usage();
  }
  const std::string_view command {argv[1]};
  const bool basic_command = command == "login" || command == "resume" || command == "consoles";
  const bool wake_command = command == "wake-check";
  const bool session_command = command == "session-check";
  const bool transport_command = command == "transport-check";
  const bool startup_command = command == "startup-check";
  const bool soak_command = command == "soak-check";
  const bool input_command = command == "input-check";
  const bool live_transport_command = transport_command || startup_command || soak_command || input_command;
  if ((basic_command && argc != 3) || ((wake_command || session_command || transport_command || startup_command || soak_command) && argc != 4 && argc != 5) || (input_command && argc != 5) || (!basic_command && !wake_command && !session_command && !live_transport_command)) {
    return print_usage();
  }
  const auto input_case = input_command ? parse_input_case(argv[4]) : std::optional<input_case_e> {};
  if (input_command && !input_case) {
    std::cerr << "Unsupported input case; run the command without arguments to list fixed case names.\n";
    return 2;
  }

  std::signal(SIGINT, cancel_probe);
  std::signal(SIGTERM, cancel_probe);
  auth::curl_http_client_t http;
  auth::system_runtime_t runtime;
  auth::client_t auth_client {http, runtime};
  auth::token_store_t store {std::filesystem::path {argv[2]}};
  const auto is_cancelled = []() {
    return cancelled != 0;
  };

  auth::auth_result_t<auth::authenticated_t> authenticated;
  if (command == "login") {
    const auto code = auth_client.begin_device_code();
    if (!code) {
      return print_failure(code.error);
    }
    std::cout << "Open " << code.value.verification_uri << " and enter code " << code.value.user_code << ".\n";
    authenticated = auth_client.complete_device_code(code.value, is_cancelled);
  } else {
    auth::oauth_credentials_t saved;
    const auto loaded = store.load(saved);
    if (!loaded) {
      return print_store_failure("Loading credentials", loaded);
    }
    authenticated = auth_client.resume(std::move(saved), is_cancelled);
  }

  if (!authenticated) {
    if (!authenticated.value.oauth.refresh_token.empty()) {
      const auto persisted = store.save(authenticated.value.oauth);
      if (!persisted) {
        return print_store_failure("Saving Microsoft credentials after an Xbox service failure", persisted);
      }
    }
    return print_failure(authenticated.error);
  }
  const auto persisted = store.save(authenticated.value.oauth);
  if (!persisted) {
    return print_store_failure("Saving credentials", persisted);
  }
  if (command == "login" || command == "resume") {
    std::cout << "Xbox Home streaming authentication is ready; OAuth credentials were saved securely.\n";
    return 0;
  }

  session::client_t session_client {
    http,
    runtime,
    authenticated.value.session,
    [&](auth::session_context_t &context) {
      auto refreshed = auth_client.resume(authenticated.value.oauth, is_cancelled);
      if (!refreshed) {
        return false;
      }
      authenticated.value.oauth = std::move(refreshed.value.oauth);
      context = std::move(refreshed.value.session);
      return static_cast<bool>(store.save(authenticated.value.oauth));
    },
  };
  const auto consoles = session_client.discover(is_cancelled);
  if (!consoles) {
    return print_failure(consoles.error);
  }
  if (command == "consoles") {
    std::cout << "Found " << consoles.value.size() << " Home console(s).\n";
    for (std::size_t index = 0; index < consoles.value.size(); ++index) {
      const auto &console = consoles.value[index];
      std::cout << "  [" << index + 1 << "] " << printable_label(console.device_name) << " (" << printable_label(console.console_type) << ", " << printable_label(console.power_state)
                << ") stable-id=" << printable_label(console.server_id) << '\n';
    }
    return 0;
  }

  const auto console_number = parse_positive(argv[3], static_cast<std::uint32_t>(consoles.value.size()));
  const auto default_operation_count = wake_command ? 45U : (soak_command ? 1800U : (startup_command ? 300U : (transport_command ? 600U : 5U)));
  const auto maximum_operation_count = wake_command ? 300U : (live_transport_command ? 3600U : 20U);
  const auto operation_count = input_command ? std::optional<std::uint32_t> {*input_case == input_case_e::vibration ? 60U : 3U} : (argc == 5 ? parse_positive(argv[4], maximum_operation_count) : std::optional<std::uint32_t> {default_operation_count});
  if (!console_number || !operation_count) {
    std::cerr << "Console number or duration/repeat count is invalid; run the consoles command again.\n";
    return 2;
  }
  const auto &candidate = consoles.value[*console_number - 1];
  const auto selected = session_client.select_console(consoles.value, candidate.server_id);
  if (!selected) {
    return print_failure(selected.error);
  }

  if (wake_command || live_transport_command) {
    session::wake_options_t wake_options;
    wake_options.command_session_id = uuid_util::uuid_t::generate().string();
    if (wake_command) {
      wake_options.wake_timeout = seconds {*operation_count};
    }
    const bool was_standby = selected.value.power_state == "ConnectedStandby";
    const auto woken = session_client.wake_and_wait(selected.value, wake_options, is_cancelled);
    if (!woken) {
      return print_failure(woken.error);
    }
    std::cout << (was_standby ? "Xbox WakeUp command completed; console now reports On.\n" : "Xbox wake gate found the console already On.\n");
    if (wake_command) {
      std::cout << "Wake gate passed; verify the HDMI RX lock before relying on the local capture path.\n";
      return 0;
    }
  }

  if (live_transport_command) {
    const auto rss_before = resident_set_kib();
    const auto threads_before = count_process_resources("/proc/self/task");
    const auto descriptors_before = count_process_resources("/proc/self/fd");
    auto provisioned = session_client.create_and_wait(selected.value.server_id, {}, is_cancelled);
    if (!provisioned) {
      return print_failure(provisioned.error);
    }
    transport::peer_t peer;
    const auto cleanup = [&]() {
      peer.close();
      const auto deleted = session_client.delete_session(provisioned.value.session_id);
      transport::cleanup_runtime();
      return deleted;
    };
    auto offer = peer.create_offer(seconds {8}, is_cancelled);
    if (!offer) {
      const auto failure = offer.error;
      cleanup();
      return print_failure(failure);
    }
    std::cout << "Generated Xbox-compatible offer with video/audio/application sections, four configured data channels, and " << offer.value.candidates.size() << " local candidate(s).\n";
    const auto sent_sdp = session_client.send_sdp(provisioned.value.session_id, {offer.value.sdp}, is_cancelled);
    if (!sent_sdp) {
      cleanup();
      return print_failure(sent_sdp.error);
    }

    std::optional<std::string> remote_sdp;
    const auto sdp_deadline = steady_clock::now() + seconds {30};
    while (!remote_sdp && steady_clock::now() < sdp_deadline && !is_cancelled()) {
      const auto polled = session_client.poll_sdp(provisioned.value.session_id, is_cancelled);
      if (!polled) {
        cleanup();
        return print_failure(polled.error);
      }
      if (polled.value) {
        auto parsed = protocol::parse_sdp_exchange(*polled.value);
        if (!parsed) {
          cleanup();
          std::cerr << "Xbox SDP exchange response failed structural validation.\n";
          return 1;
        }
        remote_sdp = std::move(parsed.value);
        break;
      }
      runtime.wait_for(seconds {1}, is_cancelled);
    }
    if (!remote_sdp) {
      cleanup();
      std::cerr << "Xbox SDP answer did not arrive before the deadline.\n";
      return 1;
    }
    const auto applied_answer = peer.set_remote_answer(*remote_sdp);
    if (!applied_answer) {
      const auto failure = applied_answer.error;
      cleanup();
      return print_failure(failure);
    }
    const auto sent_ice = session_client.send_ice(provisioned.value.session_id, offer.value.candidates, is_cancelled);
    if (!sent_ice) {
      cleanup();
      return print_failure(sent_ice.error);
    }

    std::optional<std::vector<protocol::ice_candidate_t>> remote_candidates;
    const auto ice_deadline = steady_clock::now() + seconds {30};
    while (!remote_candidates && steady_clock::now() < ice_deadline && !is_cancelled()) {
      const auto polled = session_client.poll_ice(provisioned.value.session_id, is_cancelled);
      if (!polled) {
        cleanup();
        return print_failure(polled.error);
      }
      if (polled.value) {
        auto parsed = protocol::parse_ice_exchange(*polled.value);
        if (!parsed) {
          cleanup();
          std::cerr << "Xbox ICE exchange response failed structural validation.\n";
          return 1;
        }
        remote_candidates = std::move(parsed.value);
        break;
      }
      runtime.wait_for(seconds {1}, is_cancelled);
    }
    if (!remote_candidates) {
      cleanup();
      std::cerr << "Xbox ICE candidates did not arrive before the deadline.\n";
      return 1;
    }
    const auto added_ice = peer.add_remote_candidates(*remote_candidates);
    if (!added_ice) {
      const auto failure = added_ice.error;
      cleanup();
      return print_failure(failure);
    }
    const auto ready = peer.wait_ready(seconds {30}, is_cancelled);
    if (!ready) {
      const auto failure = ready.error;
      cleanup();
      return print_failure(failure);
    }
    std::size_t vibration_packet_count = 0;
    std::optional<protocol::vibration_t> last_vibration;
    const auto observe_inbound = [&](const transport::channel_message_t &message) {
      if (message.channel != "input" || !message.binary) {
        return;
      }
      const auto parsed = protocol::parse_vibration_packet(message.payload);
      if (!parsed) {
        return;
      }
      ++vibration_packet_count;
      const bool changed = !last_vibration || last_vibration->left_motor_percent != parsed.value.left_motor_percent || last_vibration->right_motor_percent != parsed.value.right_motor_percent || last_vibration->left_trigger_percent != parsed.value.left_trigger_percent || last_vibration->right_trigger_percent != parsed.value.right_trigger_percent || last_vibration->duration_ms != parsed.value.duration_ms || last_vibration->delay_ms != parsed.value.delay_ms || last_vibration->repeat != parsed.value.repeat;
      last_vibration = parsed.value;
      if (changed) {
        std::cout << "Vibration: left=" << static_cast<unsigned>(parsed.value.left_motor_percent) << "%, right=" << static_cast<unsigned>(parsed.value.right_motor_percent) << "%, left-trigger=" << static_cast<unsigned>(parsed.value.left_trigger_percent) << "%, right-trigger=" << static_cast<unsigned>(parsed.value.right_trigger_percent) << "%, duration=" << parsed.value.duration_ms << " ms, delay=" << parsed.value.delay_ms << " ms, repeat=" << static_cast<unsigned>(parsed.value.repeat) << ".\n"
                  << std::flush;
      }
    };
    if (startup_command || soak_command || input_command) {
      startup_sender_t sender {peer};
      startup::coordinator_t coordinator {sender};
      auto startup_result = coordinator.start(steady_clock::now());
      if (!startup_result) {
        const auto failure = startup_result.error;
        cleanup();
        return print_failure(failure);
      }
      while (coordinator.state() != startup::state_e::ready && !is_cancelled()) {
        while (auto message = peer.take_message()) {
          if (message->channel != "message" || message->binary) {
            observe_inbound(*message);
            continue;
          }
          const std::string payload {message->payload.begin(), message->payload.end()};
          startup_result = coordinator.on_message(payload, steady_clock::now());
          if (!startup_result) {
            const auto failure = startup_result.error;
            cleanup();
            return print_failure(failure);
          }
        }
        startup_result = coordinator.poll(steady_clock::now(), is_cancelled());
        if (!startup_result) {
          const auto failure = startup_result.error;
          cleanup();
          return print_failure(failure);
        }
        if (coordinator.state() == startup::state_e::ready) {
          break;
        }
        const auto current = peer.snapshot();
        if (!current.failure_stage.empty() || current.peer == transport::peer_state_e::failed || current.ice == transport::ice_state_e::failed) {
          cleanup();
          std::cerr << "Xbox WebRTC transport failed during the startup handshake.\n";
          return 1;
        }
        std::this_thread::sleep_for(milliseconds {25});
      }
      if (coordinator.state() != startup::state_e::ready) {
        cleanup();
        std::cerr << "Xbox startup handshake was cancelled before becoming ready.\n";
        return 1;
      }
      std::cout << "HandshakeAck accepted; authorization, gamepad remove/add, six capabilities, and ClientMetadata were sent without gamepad reports.\n";
    }
    if (input_command) {
      input::packetizer_t packetizer;
      const auto actions = make_input_actions(*input_case);
      for (const auto &action : actions) {
        std::cout << "Input phase: " << action.label << " for " << action.duration.count() << " ms.\n"
                  << std::flush;
        const auto action_deadline = steady_clock::now() + action.duration;
        while (steady_clock::now() < action_deadline && !is_cancelled()) {
          const auto timestamp_ms = duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
          if (!peer.send_binary("input", packetizer.encode(action.frame, timestamp_ms))) {
            cleanup();
            std::cerr << "Xbox input channel rejected a scripted absolute state.\n";
            return 1;
          }
          const auto kept_alive = session_client.keepalive_if_due(provisioned.value, is_cancelled);
          if (!kept_alive) {
            cleanup();
            return print_failure(kept_alive.error);
          }
          const auto current = peer.snapshot();
          if (!current.failure_stage.empty() || current.peer == transport::peer_state_e::failed || current.ice == transport::ice_state_e::failed) {
            cleanup();
            std::cerr << "Xbox WebRTC transport failed during scripted input delivery.\n";
            return 1;
          }
          while (auto message = peer.take_message()) {
            observe_inbound(*message);
          }
          std::this_thread::sleep_for(milliseconds {16});
        }
      }
      if (is_cancelled()) {
        cleanup();
        std::cerr << "Xbox scripted input check was cancelled.\n";
        return 1;
      }
      std::cout << "Scripted absolute input phases completed with a final neutral state.\n"
                << std::flush;
    }
    std::cout << "Peer, ICE, and all four Xbox data channels are ready; " << (startup_command || soak_command || input_command ? "startup complete; " : "") << "holding for " << *operation_count << " second(s).\n";
    const auto rss_hold_start = resident_set_kib();
    const auto threads_hold_start = count_process_resources("/proc/self/task");
    const auto descriptors_hold_start = count_process_resources("/proc/self/fd");
    auto rss_hold_high_water = rss_hold_start;
    auto threads_hold_high_water = threads_hold_start;
    auto descriptors_hold_high_water = descriptors_hold_start;
    const auto hold_started = steady_clock::now();
    const auto hold_deadline = hold_started + seconds {*operation_count};
    auto next_resource_sample = hold_started;
    auto next_soak_report = hold_started + minutes {5};
    input::packetizer_t soak_packetizer;
    const protocol::gamepad_frame_t neutral_frame;
    std::uint64_t soak_packet_count = 0;
    while (steady_clock::now() < hold_deadline && !is_cancelled()) {
      const auto now = steady_clock::now();
      if (soak_command) {
        const auto timestamp_ms = duration<double, std::milli>(now.time_since_epoch()).count();
        if (!peer.send_binary("input", soak_packetizer.encode(neutral_frame, timestamp_ms))) {
          cleanup();
          std::cerr << "Xbox input channel rejected a neutral soak state.\n";
          return 1;
        }
        ++soak_packet_count;
      }
      const auto kept_alive = session_client.keepalive_if_due(provisioned.value, is_cancelled);
      if (!kept_alive) {
        cleanup();
        return print_failure(kept_alive.error);
      }
      const auto current = peer.snapshot();
      if (!current.failure_stage.empty() || current.peer == transport::peer_state_e::failed || current.ice == transport::ice_state_e::failed) {
        cleanup();
        std::cerr << "Xbox WebRTC transport failed while holding the session.\n";
        return 1;
      }
      while (auto message = peer.take_message()) {
        observe_inbound(*message);
      }
      if (now >= next_resource_sample) {
        update_high_water(rss_hold_high_water, resident_set_kib());
        update_high_water(threads_hold_high_water, count_process_resources("/proc/self/task"));
        update_high_water(descriptors_hold_high_water, count_process_resources("/proc/self/fd"));
        next_resource_sample = now + seconds {1};
      }
      if (soak_command && now >= next_soak_report) {
        std::cout << "Soak checkpoint: elapsed=" << duration_cast<seconds>(now - hold_started).count() << " s, neutral packets=" << soak_packet_count << ".\n"
                  << std::flush;
        next_soak_report += minutes {5};
      }
      if (soak_command) {
        std::this_thread::sleep_for(milliseconds {16});
      } else if (!runtime.wait_for(seconds {1}, is_cancelled)) {
        break;
      }
    }
    const auto rss_hold_end = resident_set_kib();
    const auto threads_hold_end = count_process_resources("/proc/self/task");
    const auto descriptors_hold_end = count_process_resources("/proc/self/fd");
    const auto final_snapshot = peer.snapshot();
    const auto deleted = cleanup();
    if (!deleted) {
      return print_failure(deleted.error);
    }
    if (is_cancelled()) {
      std::cerr << (input_command ? "Xbox input check" : (soak_command ? "Xbox soak check" : (startup_command ? "Xbox startup check" : "Xbox WebRTC transport check"))) << " was cancelled after cleanup.\n";
      return 1;
    }
    if (final_snapshot.video_packets == 0 || final_snapshot.audio_packets == 0) {
      std::cerr << "Xbox WebRTC transport opened but did not consume both audio and video RTP.\n";
      return 1;
    }
    auto threads_after = count_process_resources("/proc/self/task");
    auto descriptors_after = count_process_resources("/proc/self/fd");
    if (threads_before && descriptors_before) {
      const auto quiescence_deadline = steady_clock::now() + seconds {2};
      while (steady_clock::now() < quiescence_deadline && threads_after && descriptors_after && (*threads_after > *threads_before || *descriptors_after > *descriptors_before)) {
        std::this_thread::sleep_for(milliseconds {50});
        threads_after = count_process_resources("/proc/self/task");
        descriptors_after = count_process_resources("/proc/self/fd");
      }
    }
    const auto rss_after = resident_set_kib();
    std::cout << "Consumed RTP without decoders: video packets=" << final_snapshot.video_packets << ", audio packets=" << final_snapshot.audio_packets << ".\n";
    if (rss_hold_start && rss_hold_end && rss_hold_high_water && threads_hold_start && threads_hold_end && threads_hold_high_water && descriptors_hold_start && descriptors_hold_end && descriptors_hold_high_water) {
      std::cout << "Hold resources: RSS KiB " << *rss_hold_start << " -> " << *rss_hold_end << " (peak " << *rss_hold_high_water << "); threads " << *threads_hold_start << " -> " << *threads_hold_end << " (peak " << *threads_hold_high_water << "); file descriptors " << *descriptors_hold_start << " -> " << *descriptors_hold_end << " (peak " << *descriptors_hold_high_water << ").\n";
    }
    if (threads_before && threads_after && descriptors_before && descriptors_after) {
      std::cout << "Resources after deterministic close: threads " << *threads_before << " -> " << *threads_after << "; file descriptors " << *descriptors_before << " -> " << *descriptors_after << ".\n";
      if (*threads_after > *threads_before || *descriptors_after > *descriptors_before) {
        std::cerr << "Process resources grew across the WebRTC lifecycle check.\n";
        return 1;
      }
    }
    if (rss_before && rss_after) {
      std::cout << "RSS after deterministic close: " << *rss_before << " -> " << *rss_after << " KiB.\n";
    }
    if (soak_command) {
      std::cout << "Neutral soak sent " << soak_packet_count << " absolute state packet(s).\n";
    }
    if (input_command) {
      std::cout << "Parsed " << vibration_packet_count << " valid vibration packet(s). Scripted input delivery passed; user-visible mapping still requires observation.\n";
    } else {
      std::cout << (soak_command ? "Xbox neutral-input soak gate passed.\n" : (startup_command ? "Xbox startup-handshake gate passed.\n" : "Xbox WebRTC compatibility gate passed.\n"));
    }
    return 0;
  }

  const auto threads_before = count_process_resources("/proc/self/task");
  const auto descriptors_before = count_process_resources("/proc/self/fd");
  std::cout << "Checking " << printable_label(selected.value.device_name) << " with " << *operation_count << " immediate create/delete cycle(s).\n";
  for (std::uint32_t run = 1; run <= *operation_count; ++run) {
    const auto create_started = steady_clock::now();
    auto provisioned = session_client.create_and_wait(selected.value.server_id, {}, is_cancelled);
    const auto create_duration = duration_cast<milliseconds>(steady_clock::now() - create_started);
    if (!provisioned) {
      return print_failure(provisioned.error);
    }
    const auto delete_started = steady_clock::now();
    const auto deleted = session_client.delete_session(provisioned.value.session_id, is_cancelled);
    const auto delete_duration = duration_cast<milliseconds>(steady_clock::now() - delete_started);
    if (!deleted) {
      return print_failure(deleted.error);
    }
    std::cout << "  Run " << run << ": Provisioned in " << create_duration.count() << " ms; deleted in " << delete_duration.count() << " ms"
              << " (HTTP create/state/config/delete " << provisioned.value.create_http_status << '/' << provisioned.value.state_http_status << '/'
              << provisioned.value.configuration_http_status << '/' << deleted.http_status << ").\n";
  }
  const auto threads_after = count_process_resources("/proc/self/task");
  const auto descriptors_after = count_process_resources("/proc/self/fd");
  if (threads_before && threads_after && descriptors_before && descriptors_after) {
    std::cout << "  Resources: threads " << *threads_before << " → " << *threads_after << "; file descriptors " << *descriptors_before << " → "
              << *descriptors_after << ".\n";
    if (*threads_after > *threads_before || *descriptors_after > *descriptors_before) {
      std::cerr << "Process resources grew across the lifecycle check.\n";
      return 1;
    }
  }
  std::cout << "Xbox Home session lifecycle check passed.\n";
  return 0;
}
