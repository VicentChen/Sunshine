/**
 * @file src/xbox_remote/production_connection.cpp
 * @brief Production Xbox Remote Play connection assembled from validated modules.
 */

#include "src/xbox_remote/production_connection.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include "src/uuid.h"
#include "src/xbox_remote/auth.h"
#include "src/xbox_remote/http_runtime.h"
#include "src/xbox_remote/session.h"
#include "src/xbox_remote/startup.h"
#include "src/xbox_remote/token_store.h"
#include "src/xbox_remote/transport.h"

namespace xbox_remote::production {
  worker::failure_kind_e classify_authentication_failure(const auth::auth_failure_t &error) {
    return error.code == auth::auth_error_e::network_error || error.code == auth::auth_error_e::timeout || error.code == auth::auth_error_e::cancelled ? worker::failure_kind_e::retryable : worker::failure_kind_e::reauthentication_required;
  }

  worker::failure_kind_e classify_session_failure(const session::failure_t &error) {
    if (error.code == session::error_e::unauthorized) {
      return worker::failure_kind_e::reauthentication_required;
    }
    if (error.code == session::error_e::not_found || error.code == session::error_e::duplicate_console || error.code == session::error_e::ambiguous_console || error.code == session::error_e::invalid_response) {
      return worker::failure_kind_e::permanent;
    }
    return worker::failure_kind_e::retryable;
  }

  worker::failure_kind_e classify_transport_failure(const transport::failure_t &error) {
    return error.code == transport::error_e::invalid_sdp || error.code == transport::error_e::invalid_candidate ? worker::failure_kind_e::permanent : worker::failure_kind_e::retryable;
  }

  worker::failure_kind_e classify_startup_failure(const startup::failure_t &error) {
    return error.code == startup::error_e::invalid_ack || error.code == startup::error_e::invalid_state ? worker::failure_kind_e::permanent : worker::failure_kind_e::retryable;
  }

  namespace {
    using namespace std::chrono_literals;

    /**
     * @brief Adapt a connected production peer to the startup sender interface.
     */
    class startup_sender_t final: public startup::sender_t {
    public:
      /**
       * @brief Bind to a peer that outlives the adapter.
       *
       * @param peer Connected WebRTC peer.
       */
      explicit startup_sender_t(transport::peer_t &peer):
          peer_(peer) {
      }

      /**
       * @copydoc startup::sender_t::send_text
       */
      bool send_text(std::string_view channel, std::string_view payload) override {
        return peer_.send_text(channel, payload);
      }

      /**
       * @copydoc startup::sender_t::send_binary
       */
      bool send_binary(std::string_view channel, const std::vector<std::uint8_t> &payload) override {
        return peer_.send_binary(channel, payload);
      }

    private:
      transport::peer_t &peer_;  ///< Connected production peer.
    };

    /**
     * @brief Production connection owned exclusively by one worker thread.
     */
    class connection_t final: public worker::connection_t {
    public:
      /**
       * @brief Construct a closed production connection.
       *
       * @param options Credentials and console selection.
       */
      explicit connection_t(options_t options):
          options_(std::move(options)),
          auth_client_(http_, runtime_),
          store_(options_.token_file) {
      }

      /**
       * @brief Close any remaining remote resources.
       */
      ~connection_t() override {
        static_cast<void>(close());
      }

      /**
       * @copydoc worker::connection_t::open
       */
      worker::result_t open(const std::function<bool()> &cancelled) override {
        http_.set_cancellation_callback(cancelled);
        if (options_.token_file.empty()) {
          return failure("configuration", worker::failure_kind_e::permanent);
        }
        report("authentication");
        auth::oauth_credentials_t saved;
        if (!store_.load(saved)) {
          return failure("token_store_load", worker::failure_kind_e::reauthentication_required);
        }
        authenticated_ = auth_client_.resume(std::move(saved), cancelled);
        if (!authenticated_) {
          persist_oauth();
          return failure(authenticated_.error.stage.empty() ? "authentication" : authenticated_.error.stage, classify_authentication_failure(authenticated_.error));
        }
        if (!persist_oauth()) {
          return failure("token_store_save", worker::failure_kind_e::permanent);
        }
        session_client_ = std::make_unique<session::client_t>(
          http_,
          runtime_,
          authenticated_.value.session,
          [this, cancelled](auth::session_context_t &context) {
            auto refreshed = auth_client_.resume(authenticated_.value.oauth, cancelled);
            if (!refreshed) {
              return false;
            }
            authenticated_.value.oauth = std::move(refreshed.value.oauth);
            context = std::move(refreshed.value.session);
            return persist_oauth();
          }
        );
        report("discovery");
        const auto consoles = session_client_->discover(cancelled);
        if (!consoles) {
          return failure(consoles.error.stage, classify_session_failure(consoles.error));
        }
        auto selected = session_client_->select_console(consoles.value, options_.console_id);
        if (!selected) {
          return failure(selected.error.stage, classify_session_failure(selected.error));
        }
        if (options_.wake_console) {
          report("wake");
          session::wake_options_t wake_options;
          wake_options.command_session_id = uuid_util::uuid_t::generate().string();
          auto woken = session_client_->wake_and_wait(selected.value, wake_options, cancelled);
          if (!woken) {
            return failure(woken.error.stage, classify_session_failure(woken.error));
          }
        }
        report("provisioning");
        auto created = session_client_->create_and_wait(selected.value.server_id, {}, cancelled);
        if (!created) {
          return failure(created.error.stage, classify_session_failure(created.error));
        }
        provisioned_ = std::move(created.value);
        peer_ = std::make_unique<transport::peer_t>();
        transport_initialized_ = true;
        report("signaling_sdp");
        const auto offer = peer_->create_offer(8s, cancelled);
        if (!offer) {
          return failure(offer.error.stage, classify_transport_failure(offer.error));
        }
        if (offer.value.used_host_candidate_fallback) {
          report("signaling_sdp_host_fallback");
        }
        const auto sent_sdp = session_client_->send_sdp(provisioned_->session_id, {offer.value.sdp}, cancelled);
        if (!sent_sdp) {
          return failure(sent_sdp.error.stage, classify_session_failure(sent_sdp.error));
        }
        auto remote_sdp = poll_exchange<std::string>(
          cancelled,
          [this, &cancelled]() {
            return session_client_->poll_sdp(provisioned_->session_id, cancelled);
          },
          [](const std::string &value) {
            return protocol::parse_sdp_exchange(value);
          },
          "sdp_poll"
        );
        if (!remote_sdp) {
          return failure(remote_sdp.stage, remote_sdp.failure_kind);
        }
        const auto answer = peer_->set_remote_answer(remote_sdp.value);
        if (!answer) {
          return failure(answer.error.stage, classify_transport_failure(answer.error));
        }
        report("signaling_ice");
        const auto sent_ice = session_client_->send_ice(provisioned_->session_id, offer.value.candidates, cancelled);
        if (!sent_ice) {
          return failure(sent_ice.error.stage, classify_session_failure(sent_ice.error));
        }
        auto remote_ice = poll_exchange<std::vector<protocol::ice_candidate_t>>(
          cancelled,
          [this, &cancelled]() {
            return session_client_->poll_ice(provisioned_->session_id, cancelled);
          },
          [](const std::string &value) {
            return protocol::parse_ice_exchange(value);
          },
          "ice_poll"
        );
        if (!remote_ice) {
          return failure(remote_ice.stage, remote_ice.failure_kind);
        }
        const auto added = peer_->add_remote_candidates(remote_ice.value);
        if (!added) {
          return failure(added.error.stage, classify_transport_failure(added.error));
        }
        report("transport");
        const auto ready = peer_->wait_ready(30s, cancelled);
        if (!ready) {
          return failure(ready.error.stage, classify_transport_failure(ready.error));
        }
        report("handshake");
        startup_sender_t sender {*peer_};
        startup::coordinator_t coordinator {sender};
        auto started = coordinator.start(std::chrono::steady_clock::now());
        while (started && coordinator.state() != startup::state_e::ready && !cancelled()) {
          while (auto message = peer_->take_message()) {
            if (message->channel != "message" || message->binary) {
              continue;
            }
            started = coordinator.on_message(std::string {message->payload.begin(), message->payload.end()}, std::chrono::steady_clock::now());
            if (!started) {
              break;
            }
          }
          if (started) {
            started = coordinator.poll(std::chrono::steady_clock::now(), cancelled());
          }
          if (started && coordinator.state() != startup::state_e::ready) {
            std::this_thread::sleep_for(25ms);
          }
        }
        if (!started || coordinator.state() != startup::state_e::ready) {
          return failure(started.error.stage.empty() ? "startup" : started.error.stage, classify_startup_failure(started.error));
        }
        return {};
      }

      /**
       * @copydoc worker::connection_t::set_progress_handler
       */
      void set_progress_handler(progress_handler_t handler) override {
        progress_handler_ = std::move(handler);
      }

      /**
       * @copydoc worker::connection_t::send
       */
      bool send(const input::item_t &item) override {
        if (!peer_) {
          return false;
        }
        if (item.kind == input::item_kind_e::detach) {
          return peer_->send_text("control", protocol::make_gamepad_changed(0, false));
        }
        const auto timestamp = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto frame = item.kind == input::item_kind_e::neutralize ? protocol::gamepad_frame_t {} : item.frame;
        return peer_->send_binary("input", packetizer_.encode(frame, timestamp));
      }

      /**
       * @copydoc worker::connection_t::poll
       */
      worker::result_t poll(const std::function<bool()> &cancelled, std::optional<protocol::vibration_t> &vibration) override {
        if (!peer_ || !session_client_ || !provisioned_) {
          return failure("not_connected");
        }
        const auto kept_alive = session_client_->keepalive_if_due(*provisioned_, cancelled);
        if (!kept_alive) {
          return failure(kept_alive.error.stage, classify_session_failure(kept_alive.error));
        }
        const auto snapshot = peer_->snapshot();
        if (!snapshot.failure_stage.empty() || snapshot.peer == transport::peer_state_e::failed || snapshot.ice == transport::ice_state_e::failed || !snapshot.critical_channels_open()) {
          return failure(snapshot.failure_stage.empty() ? "transport_health" : snapshot.failure_stage);
        }
        while (auto message = peer_->take_message()) {
          if (message->channel != "input" || !message->binary) {
            continue;
          }
          const auto parsed = protocol::parse_vibration_packet(message->payload);
          if (parsed) {
            vibration = parsed.value;
            break;
          }
        }
        return {};
      }

      /**
       * @copydoc worker::connection_t::close
       */
      worker::result_t close() override {
        worker::result_t cleanup;
        if (peer_) {
          peer_->close();
          peer_.reset();
        }
        if (session_client_ && provisioned_) {
          const auto cleanup_deadline = std::chrono::steady_clock::now() + 2s;
          http_.set_cancellation_callback([cleanup_deadline]() {
            return std::chrono::steady_clock::now() >= cleanup_deadline;
          });
          const auto deleted = session_client_->delete_session(provisioned_->session_id);
          if (!deleted) {
            cleanup = failure("delete_unconfirmed", classify_session_failure(deleted.error));
          }
        }
        provisioned_.reset();
        session_client_.reset();
        if (transport_initialized_) {
          transport::cleanup_runtime();
          transport_initialized_ = false;
        }
        http_.set_cancellation_callback({});
        return cleanup;
      }

    private:
      /**
       * @brief Publish one fixed non-sensitive connection stage.
       *
       * @param stage Fixed stage name.
       */
      void report(std::string_view stage) const {
        if (progress_handler_) {
          progress_handler_(stage);
        }
      }

      /**
       * @brief Parsed exchange result with a fixed failure stage.
       *
       * @tparam T Parsed exchange value.
       */
      template<typename T>
      struct exchange_t {
        T value {};  ///< Parsed value.
        std::string stage;  ///< Fixed failure stage, empty on success.
        worker::failure_kind_e failure_kind = worker::failure_kind_e::retryable;  ///< Recovery policy on failure.

        /**
         * @brief Check whether parsing and polling succeeded.
         *
         * @return @c true when no failure stage is present.
         */
        explicit operator bool() const {
          return stage.empty();
        }
      };

      /**
       * @brief Poll and strictly parse one SDP or ICE exchange.
       *
       * @tparam T Parsed value type.
       * @tparam Poll Polling callable.
       * @tparam Parse Parsing callable.
       * @param cancelled Cancellation callback.
       * @param poll Polling callable returning an optional encoded exchange.
       * @param parse Strict exchange parser.
       * @param timeout_stage Fixed timeout or parse stage.
       * @return Parsed exchange or fixed failure stage.
       */
      template<typename T, typename Poll, typename Parse>
      exchange_t<T> poll_exchange(const std::function<bool()> &cancelled, Poll poll, Parse parse, std::string_view timeout_stage) {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline && !cancelled()) {
          const auto polled = poll();
          if (!polled) {
            return {{}, polled.error.stage, classify_session_failure(polled.error)};
          }
          if (polled.value) {
            auto parsed = parse(*polled.value);
            return parsed ? exchange_t<T> {std::move(parsed.value), {}, worker::failure_kind_e::retryable} : exchange_t<T> {{}, std::string {timeout_stage}, worker::failure_kind_e::permanent};
          }
          if (!runtime_.wait_for(1s, cancelled)) {
            break;
          }
        }
        return {{}, std::string {timeout_stage}, worker::failure_kind_e::retryable};
      }

      /**
       * @brief Create a fixed sanitized failure.
       *
       * @param stage Non-sensitive lifecycle stage.
       * @param failure_kind Recovery policy.
       * @return Failed worker result.
       */
      static worker::result_t failure(std::string_view stage, worker::failure_kind_e failure_kind = worker::failure_kind_e::retryable) {
        return {false, stage.empty() ? "unknown" : std::string {stage}, failure_kind};
      }

      /**
       * @brief Persist only the refreshable OAuth credentials.
       *
       * @return @c true when the owner-only token store accepted the update.
       */
      bool persist_oauth() {
        return authenticated_.value.oauth.refresh_token.empty() || static_cast<bool>(store_.save(authenticated_.value.oauth));
      }

      options_t options_;  ///< Credentials and console selection.
      auth::curl_http_client_t http_;  ///< Production deadline-aware HTTPS client.
      auth::system_runtime_t runtime_;  ///< Production clock and cancellable wait provider.
      auth::client_t auth_client_;  ///< Microsoft and Xbox authentication chain.
      auth::token_store_t store_;  ///< Owner-only OAuth credential store.
      auth::auth_result_t<auth::authenticated_t> authenticated_;  ///< Current in-memory authorization.
      std::unique_ptr<session::client_t> session_client_;  ///< Home REST lifecycle client.
      std::optional<session::provisioned_t> provisioned_;  ///< Active Home session.
      std::unique_ptr<transport::peer_t> peer_;  ///< Active WebRTC transport.
      input::packetizer_t packetizer_;  ///< Sender-thread-owned input sequence.
      progress_handler_t progress_handler_;  ///< Sanitized worker-stage observer.
      bool transport_initialized_ = false;  ///< Whether this connection owes one global WebRTC cleanup.
    };
  }  // namespace

  worker::connection_factory_t make_connection_factory(options_t options) {
    return [options = std::move(options)]() mutable {
      return std::make_unique<connection_t>(options);
    };
  }
}  // namespace xbox_remote::production
