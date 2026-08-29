/**
 * @file src/input/nxbt_client.cpp
 * @brief Reconnecting worker and Unix transport for the NXBT Bridge client.
 */

#include "src/input/nxbt_client.h"

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

// local includes
#include "src/platform/common.h"

#if defined(__linux__)
  // system includes
  #include <fcntl.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace input::nxbt {
  std::string_view client_event_name(client_event_e event) {
    switch (event) {
      case client_event_e::connected:
        return "connected";
      case client_event_e::connect_failed:
        return "connect_failed";
      case client_event_e::handshake_failed:
        return "handshake_failed";
      case client_event_e::disconnected:
        return "disconnected";
      case client_event_e::malformed_reply:
        return "malformed_reply";
      case client_event_e::bridge_error:
        return "bridge_error";
      case client_event_e::heartbeat_timeout:
        return "heartbeat_timeout";
      case client_event_e::controller_status:
        return "controller_status";
    }
    return "unknown";
  }

  std::string_view controller_status_name(controller_status_e status) {
    switch (status) {
      case controller_status_e::unavailable:
        return "unavailable";
      case controller_status_e::pairing:
        return "pairing";
      case controller_status_e::connecting:
        return "connecting";
      case controller_status_e::connected:
        return "connected";
      case controller_status_e::reconnecting:
        return "reconnecting";
      case controller_status_e::failed:
        return "failed";
    }
    return "unknown";
  }

  namespace {
    constexpr std::size_t max_control_messages = platf::MAX_GAMEPADS * 4;  ///< Bounded lifecycle queue capacity.
    constexpr std::size_t max_packet_size = 64;  ///< Largest accepted version-1 packet plus growth room.

    /**
     * @brief Convert a steady-clock time point to protocol microseconds.
     *
     * @param now Monotonic time point.
     * @return Microseconds since the steady clock epoch.
     */
    std::uint64_t monotonic_us(std::chrono::steady_clock::time_point now) {
      return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    }

#if defined(__linux__)
    /**
     * @brief Production non-blocking Unix `SOCK_SEQPACKET` transport.
     */
    class unix_transport_t final: public transport_t {
    public:
      /**
       * @brief Close any owned socket descriptor.
       */
      ~unix_transport_t() override {
        close();
      }

      /**
       * @copydoc transport_t::connect
       */
      std::pair<transport_result_e, std::error_code> connect(const std::string &endpoint, std::chrono::milliseconds timeout) override {
        close();
        if (endpoint.empty() || endpoint.size() >= sizeof(sockaddr_un::sun_path)) {
          return {transport_result_e::disconnected, std::make_error_code(std::errc::filename_too_long)};
        }
        fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (fd_ < 0) {
          return failure(errno);
        }
        const auto flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0 || ::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
          const auto error = errno;
          close();
          return failure(error);
        }
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
        if (::connect(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) {
          return {transport_result_e::success, {}};
        }
        if (errno != EINPROGRESS) {
          const auto error = errno;
          close();
          return failure(error);
        }
        pollfd descriptor {fd_, POLLOUT, 0};
        const auto poll_result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (poll_result == 0) {
          close();
          return {transport_result_e::timeout, std::make_error_code(std::errc::timed_out)};
        }
        if (poll_result < 0) {
          const auto error = errno;
          close();
          return failure(error);
        }
        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0 || socket_error != 0) {
          const auto error = socket_error != 0 ? socket_error : errno;
          close();
          return failure(error);
        }
        return {transport_result_e::success, {}};
      }

      /**
       * @copydoc transport_t::send
       */
      std::pair<transport_result_e, std::error_code> send(const std::vector<std::uint8_t> &packet, std::chrono::milliseconds timeout) override {
        if (fd_ < 0) {
          return failure(ENOTCONN);
        }
        pollfd descriptor {fd_, POLLOUT, 0};
        const auto poll_result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (poll_result == 0) {
          return {transport_result_e::timeout, std::make_error_code(std::errc::timed_out)};
        }
        if (poll_result < 0) {
          return failure(errno);
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          return failure(ECONNRESET);
        }
        const auto sent = ::send(fd_, packet.data(), packet.size(), MSG_NOSIGNAL);
        if (sent < 0) {
          return failure(errno);
        }
        if (static_cast<std::size_t>(sent) != packet.size()) {
          return failure(EIO);
        }
        return {transport_result_e::success, {}};
      }

      /**
       * @copydoc transport_t::receive
       */
      receive_result_t receive(std::chrono::milliseconds timeout) override {
        if (fd_ < 0) {
          return {transport_result_e::disconnected, {}, std::make_error_code(std::errc::not_connected)};
        }
        pollfd descriptor {fd_, POLLIN, 0};
        const auto poll_result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (poll_result == 0) {
          return {};
        }
        if (poll_result < 0) {
          return {transport_result_e::disconnected, {}, std::error_code(errno, std::generic_category())};
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
          return {transport_result_e::disconnected, {}, std::make_error_code(std::errc::connection_reset)};
        }
        std::array<std::uint8_t, max_packet_size> buffer {};
        iovec vector {buffer.data(), buffer.size()};
        msghdr message {};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const auto received = ::recvmsg(fd_, &message, MSG_TRUNC);
        if (received == 0) {
          return {transport_result_e::disconnected, {}, std::make_error_code(std::errc::connection_reset)};
        }
        if (received < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return {};
          }
          return {transport_result_e::disconnected, {}, std::error_code(errno, std::generic_category())};
        }
        if ((message.msg_flags & MSG_TRUNC) != 0 || static_cast<std::size_t>(received) > buffer.size()) {
          return {transport_result_e::success, std::vector<std::uint8_t>(buffer.begin(), buffer.end()), {}};
        }
        return {transport_result_e::success, std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + received), {}};
      }

      /**
       * @copydoc transport_t::close
       */
      void close() override {
        if (fd_ >= 0) {
          ::close(fd_);
          fd_ = -1;
        }
      }

    private:
      /**
       * @brief Build a disconnected result from an errno value.
       *
       * @param error Errno-compatible error value.
       * @return Transport failure and generic-category error code.
       */
      static std::pair<transport_result_e, std::error_code> failure(int error) {
        return {transport_result_e::disconnected, std::error_code(error, std::generic_category())};
      }

      int fd_ = -1;  ///< Owned non-blocking Unix socket descriptor.
    };
#else
    /**
     * @brief Unsupported-platform transport that fails without blocking.
     */
    class unix_transport_t final: public transport_t {
    public:
      /**
       * @copydoc transport_t::connect
       */
      std::pair<transport_result_e, std::error_code> connect(const std::string &, std::chrono::milliseconds) override {
        return {transport_result_e::disconnected, std::make_error_code(std::errc::operation_not_supported)};
      }

      /**
       * @copydoc transport_t::send
       */
      std::pair<transport_result_e, std::error_code> send(const std::vector<std::uint8_t> &, std::chrono::milliseconds) override {
        return {transport_result_e::disconnected, std::make_error_code(std::errc::operation_not_supported)};
      }

      /**
       * @copydoc transport_t::receive
       */
      receive_result_t receive(std::chrono::milliseconds) override {
        return {transport_result_e::disconnected, {}, std::make_error_code(std::errc::operation_not_supported)};
      }

      /**
       * @copydoc transport_t::close
       */
      void close() override {
      }
    };
#endif
  }  // namespace

  transport_factory_t make_unix_transport_factory() {
    return []() {
      return std::make_unique<unix_transport_t>();
    };
  }

  /**
   * @brief Private synchronized implementation of the NXBT client worker.
   */
  class client_t::impl_t {
  public:
    /**
     * @brief Desired logical state for one Bridge controller slot.
     */
    struct slot_t {
      bool attached = false;  ///< Whether the slot should exist on the Bridge.
      bool neutral = true;  ///< Whether the desired output is neutral.
      std::uint8_t client_relative_id = 0;  ///< Latest Moonlight controller id.
      std::optional<controller_state_t> latest_state;  ///< Bounded latest snapshot.
      bool dirty = false;  ///< Whether the latest snapshot has not been sent on this connection.
    };

    /**
     * @brief Construct state and start the owned worker thread.
     *
     * @param options Client timing and endpoint settings.
     * @param transport_factory Factory used for connection attempts.
     * @param event_callback Optional event observer.
     */
    impl_t(client_options_t options, transport_factory_t transport_factory, client_event_callback_t event_callback):
        options_(std::move(options)),
        transport_factory_(std::move(transport_factory)),
        event_callback_(std::move(event_callback)),
        diagnostics_ {
          .endpoint = options_.endpoint,
          .controller_statuses = std::vector<controller_status_e>(platf::MAX_GAMEPADS, controller_status_e::unavailable),
        },
        worker_([this]() {
          run();
        }) {
    }

    /**
     * @brief Signal shutdown and wait for bounded worker cleanup.
     */
    ~impl_t() {
      shutdown_.store(true);
      condition_.notify_all();
      if (worker_.joinable()) {
        worker_.join();
      }
    }

    /**
     * @brief Add one desired controller attachment.
     */
    bool attach(std::uint8_t controller_id, std::uint8_t client_relative_id) {
      if (!valid(controller_id)) {
        return false;
      }
      std::lock_guard lock(mutex_);
      auto &slot = slots_[controller_id];
      if (slot.attached) {
        return false;
      }
      slot = {};
      slot.attached = true;
      slot.client_relative_id = client_relative_id;
      enqueue_locked(message_t {.type = message_type_e::attach, .controller_id = controller_id, .client_relative_id = client_relative_id});
      condition_.notify_all();
      return true;
    }

    /**
     * @brief Update one desired controller binding.
     */
    bool rebind(std::uint8_t controller_id, std::uint8_t client_relative_id) {
      if (!valid(controller_id)) {
        return false;
      }
      std::lock_guard lock(mutex_);
      auto &slot = slots_[controller_id];
      if (!slot.attached) {
        return false;
      }
      slot.client_relative_id = client_relative_id;
      enqueue_locked(message_t {.type = message_type_e::rebind, .controller_id = controller_id, .client_relative_id = client_relative_id});
      condition_.notify_all();
      return true;
    }

    /**
     * @brief Replace one bounded pending controller snapshot.
     */
    bool update(const controller_state_t &state) {
      if (!valid(state.controller_id)) {
        return false;
      }
      std::lock_guard lock(mutex_);
      auto &slot = slots_[state.controller_id];
      if (!slot.attached) {
        return false;
      }
      slot.latest_state = state;
      slot.neutral = false;
      slot.dirty = true;
      condition_.notify_all();
      return true;
    }

    /**
     * @brief Make one desired slot neutral and discard its stale snapshot.
     */
    void neutralize(std::uint8_t controller_id) {
      if (!valid(controller_id)) {
        return;
      }
      std::lock_guard lock(mutex_);
      auto &slot = slots_[controller_id];
      if (!slot.attached) {
        return;
      }
      slot.neutral = true;
      slot.latest_state.reset();
      slot.dirty = false;
      enqueue_locked(message_t {.type = message_type_e::neutralize, .controller_id = controller_id});
      condition_.notify_all();
    }

    /**
     * @brief Remove one desired slot after ordered neutralize and detach.
     */
    void detach(std::uint8_t controller_id) {
      if (!valid(controller_id)) {
        return;
      }
      std::lock_guard lock(mutex_);
      auto &slot = slots_[controller_id];
      if (!slot.attached) {
        return;
      }
      enqueue_locked(message_t {.type = message_type_e::neutralize, .controller_id = controller_id});
      enqueue_locked(message_t {.type = message_type_e::detach, .controller_id = controller_id});
      slot = {};
      condition_.notify_all();
    }

    /**
     * @brief Count currently occupied latest-state slots.
     */
    std::size_t pending_state_count() const {
      std::lock_guard lock(mutex_);
      return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [](const slot_t &slot) {
        return slot.latest_state.has_value();
      }));
    }

    /**
     * @brief Copy the synchronized diagnostic state.
     */
    client_diagnostics_t diagnostics() const {
      std::lock_guard lock(diagnostic_mutex_);
      return diagnostics_;
    }

  private:
    /**
     * @brief Run connection, protocol negotiation, replay, and heartbeat loops.
     */
    void run() {
      while (!shutdown_.load()) {
        auto transport = transport_factory_ ? transport_factory_() : nullptr;
        if (!transport) {
          report({.type = client_event_e::connect_failed, .system_error = std::make_error_code(std::errc::not_supported)}, true);
          wait_reconnect();
          continue;
        }
        const auto connection = transport->connect(options_.endpoint, options_.connect_timeout);
        if (connection.first != transport_result_e::success) {
          report({.type = client_event_e::connect_failed, .system_error = connection.second}, true);
          transport->close();
          wait_reconnect();
          continue;
        }
        if (!handshake(*transport)) {
          transport->close();
          wait_reconnect();
          continue;
        }
        report({.type = client_event_e::connected}, false);
        mark_reconnect();
        bool connected = replay(*transport);
        std::optional<std::uint64_t> pending_ping;
        auto last_ping = std::chrono::steady_clock::now();
        auto ping_started = last_ping;
        while (connected && !shutdown_.load()) {
          connected = flush(*transport);
          if (!connected) {
            break;
          }
          const auto incoming = transport->receive(options_.io_timeout);
          if (incoming.result == transport_result_e::success) {
            connected = handle_reply(incoming.packet, pending_ping);
          } else if (incoming.result == transport_result_e::disconnected) {
            connected = false;
          }
          const auto now = std::chrono::steady_clock::now();
          if (connected && pending_ping && now - ping_started >= options_.heartbeat_timeout) {
            report({.type = client_event_e::heartbeat_timeout}, true);
            connected = false;
          }
          if (connected && !pending_ping && now - last_ping >= options_.heartbeat_interval) {
            const auto timestamp = monotonic_us(now);
            message_t ping {.type = message_type_e::ping, .monotonic_timestamp_us = timestamp};
            connected = send_message(*transport, ping);
            if (connected) {
              pending_ping = timestamp;
              ping_started = now;
              last_ping = now;
            }
          }
        }
        if (shutdown_.load() && connected) {
          shutdown_connected(*transport);
        }
        transport->close();
        if (!shutdown_.load()) {
          report({.type = client_event_e::disconnected}, true);
          wait_reconnect();
        }
      }
    }

    /**
     * @brief Negotiate protocol version 1 before any controller command.
     *
     * @param transport Connected packet transport.
     * @return @c true after a valid hello acknowledgement.
     */
    bool handshake(transport_t &transport) {
      if (!send_message(transport, message_t {.type = message_type_e::hello})) {
        report({.type = client_event_e::handshake_failed}, true);
        return false;
      }
      const auto deadline = std::chrono::steady_clock::now() + options_.handshake_timeout;
      while (!shutdown_.load() && std::chrono::steady_clock::now() < deadline) {
        const auto incoming = transport.receive(options_.io_timeout);
        if (incoming.result == transport_result_e::timeout) {
          continue;
        }
        if (incoming.result == transport_result_e::disconnected) {
          report({.type = client_event_e::handshake_failed, .system_error = incoming.error}, true);
          return false;
        }
        const auto decoded = decode_message(incoming.packet);
        if (decoded.error != protocol_error_e::none) {
          report({.type = client_event_e::malformed_reply, .protocol_error = decoded.error}, true);
          return false;
        }
        if (decoded.message.type == message_type_e::hello_ack) {
          return true;
        }
        if (decoded.message.type == message_type_e::error) {
          report({.type = client_event_e::handshake_failed, .protocol_error = decoded.message.error}, true);
          return false;
        }
        report({.type = client_event_e::handshake_failed, .protocol_error = protocol_error_e::unknown_message_type}, true);
        return false;
      }
      if (!shutdown_.load()) {
        report({.type = client_event_e::handshake_failed, .system_error = std::make_error_code(std::errc::timed_out)}, true);
      }
      return false;
    }

    /**
     * @brief Rebuild Bridge slots from desired state after every handshake.
     *
     * @param transport Negotiated packet transport.
     * @return @c true when replay completed.
     */
    bool replay(transport_t &transport) {
      std::array<slot_t, platf::MAX_GAMEPADS> snapshot;
      {
        std::lock_guard lock(mutex_);
        snapshot = slots_;
        controls_.clear();
        force_reconnect_ = false;
      }
      for (std::size_t controller_id = 0; controller_id < snapshot.size(); ++controller_id) {
        const auto &slot = snapshot[controller_id];
        if (!slot.attached) {
          continue;
        }
        if (!send_message(transport, message_t {
                                       .type = message_type_e::attach,
                                       .controller_id = static_cast<std::uint8_t>(controller_id),
                                       .client_relative_id = slot.client_relative_id,
                                     }) ||
            !send_message(transport, message_t {.type = message_type_e::neutralize, .controller_id = static_cast<std::uint8_t>(controller_id)})) {
          return false;
        }
        if (!slot.neutral && slot.latest_state && !send_state(transport, *slot.latest_state)) {
          return false;
        }
        if (slot.latest_state) {
          clear_dirty_if_current(controller_id, slot.latest_state->sequence);
        }
      }
      return true;
    }

    /**
     * @brief Send queued lifecycle messages then at most one state per slot.
     *
     * @param transport Negotiated packet transport.
     * @return @c true while the connection remains usable.
     */
    bool flush(transport_t &transport) {
      std::deque<message_t> controls;
      std::array<std::optional<controller_state_t>, platf::MAX_GAMEPADS> states;
      {
        std::lock_guard lock(mutex_);
        if (force_reconnect_) {
          return false;
        }
        controls.swap(controls_);
        for (std::size_t index = 0; index < slots_.size(); ++index) {
          if (slots_[index].dirty && slots_[index].latest_state) {
            states[index] = slots_[index].latest_state;
          }
        }
      }
      for (const auto &message : controls) {
        if (!send_message(transport, message)) {
          return false;
        }
      }
      for (std::size_t index = 0; index < states.size(); ++index) {
        if (!states[index]) {
          continue;
        }
        if (!send_state(transport, *states[index])) {
          return false;
        }
        clear_dirty_if_current(index, states[index]->sequence);
      }
      return true;
    }

    /**
     * @brief Decode and process one post-handshake Bridge reply.
     *
     * @param packet Complete received packet.
     * @param pending_ping Current outstanding ping timestamp.
     * @return @c true when the connection remains protocol-valid.
     */
    bool handle_reply(const std::vector<std::uint8_t> &packet, std::optional<std::uint64_t> &pending_ping) {
      const auto decoded = decode_message(packet);
      if (decoded.error != protocol_error_e::none) {
        report({.type = client_event_e::malformed_reply, .protocol_error = decoded.error}, true);
        return false;
      }
      switch (decoded.message.type) {
        case message_type_e::pong:
          if (pending_ping && decoded.message.monotonic_timestamp_us == *pending_ping) {
            pending_ping.reset();
          }
          return true;
        case message_type_e::status:
          report({
                   .type = client_event_e::controller_status,
                   .controller_id = decoded.message.controller_id,
                   .controller_status = decoded.message.status,
                 },
                 false);
          return true;
        case message_type_e::error:
          report({.type = client_event_e::bridge_error, .protocol_error = decoded.message.error}, true);
          return false;
        default:
          report({.type = client_event_e::malformed_reply, .protocol_error = protocol_error_e::unknown_message_type}, true);
          return false;
      }
    }

    /**
     * @brief Best-effort neutralize and detach of all desired slots.
     *
     * @param transport Currently negotiated transport.
     */
    void shutdown_connected(transport_t &transport) {
      std::array<bool, platf::MAX_GAMEPADS> attached {};
      {
        std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < slots_.size(); ++index) {
          attached[index] = slots_[index].attached;
        }
      }
      for (std::size_t index = 0; index < attached.size(); ++index) {
        if (!attached[index]) {
          continue;
        }
        const auto controller_id = static_cast<std::uint8_t>(index);
        if (!send_message(transport, message_t {.type = message_type_e::neutralize, .controller_id = controller_id})) {
          return;
        }
        if (!send_message(transport, message_t {.type = message_type_e::detach, .controller_id = controller_id})) {
          return;
        }
      }
    }

    /**
     * @brief Encode and send one protocol message.
     */
    bool send_message(transport_t &transport, const message_t &message) const {
      return transport.send(encode_message(message), options_.io_timeout).first == transport_result_e::success;
    }

    /**
     * @brief Encode and send one complete controller state.
     */
    bool send_state(transport_t &transport, const controller_state_t &state) const {
      return send_message(transport, message_t {.type = message_type_e::state, .state = state});
    }

    /**
     * @brief Clear dirty only if no newer state replaced the sent snapshot.
     */
    void clear_dirty_if_current(std::size_t controller_id, std::uint32_t sequence) {
      std::lock_guard lock(mutex_);
      auto &slot = slots_[controller_id];
      if (slot.latest_state && slot.latest_state->sequence == sequence) {
        slot.dirty = false;
      }
    }

    /**
     * @brief Mark every current latest state dirty for a new connection.
     */
    void mark_reconnect() {
      std::lock_guard lock(mutex_);
      for (auto &slot : slots_) {
        slot.dirty = slot.attached && !slot.neutral && slot.latest_state.has_value();
      }
    }

    /**
     * @brief Append reliable lifecycle control or force a clean replay on overflow.
     *
     * @param message Controller lifecycle message.
     */
    void enqueue_locked(message_t message) {
      if (controls_.size() >= max_control_messages) {
        controls_.clear();
        force_reconnect_ = true;
        return;
      }
      controls_.push_back(std::move(message));
    }

    /**
     * @brief Wait interruptibly before another connection attempt.
     */
    void wait_reconnect() {
      std::unique_lock lock(mutex_);
      condition_.wait_for(lock, options_.reconnect_delay, [this]() {
        return shutdown_.load();
      });
    }

    /**
     * @brief Deliver an event while limiting repeated error categories.
     *
     * @param event Event details.
     * @param rate_limited Whether identical categories use the error interval.
     */
    void report(const client_event_t &event, bool rate_limited) {
      {
        std::lock_guard lock(diagnostic_mutex_);
        if (event.type == client_event_e::connected) {
          diagnostics_.socket_connected = true;
          diagnostics_.negotiated_protocol_version = protocol_version;
          diagnostics_.heartbeat_healthy = true;
          diagnostics_.has_last_error = false;
          diagnostics_.last_system_error.clear();
          diagnostics_.last_protocol_error = protocol_error_e::none;
        } else if (event.type == client_event_e::controller_status) {
          if (event.controller_id < diagnostics_.controller_statuses.size()) {
            diagnostics_.controller_statuses[event.controller_id] = event.controller_status;
          }
        } else {
          diagnostics_.socket_connected = false;
          diagnostics_.negotiated_protocol_version = 0;
          diagnostics_.heartbeat_healthy = false;
          diagnostics_.has_last_error = true;
          diagnostics_.last_error = event.type;
          diagnostics_.last_system_error = event.system_error;
          diagnostics_.last_protocol_error = event.protocol_error;
        }
      }
      if (!event_callback_) {
        return;
      }
      if (rate_limited) {
        const auto index = static_cast<std::size_t>(event.type);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(event_mutex_);
        if (last_event_[index] && now - *last_event_[index] < options_.error_log_interval) {
          return;
        }
        last_event_[index] = now;
      }
      event_callback_(event);
    }

    /**
     * @brief Validate a Bridge slot against Sunshine's global slot bound.
     */
    static bool valid(std::uint8_t controller_id) {
      return controller_id < platf::MAX_GAMEPADS;
    }

    client_options_t options_;  ///< Endpoint and bounded worker timings.
    transport_factory_t transport_factory_;  ///< Fresh transport provider.
    client_event_callback_t event_callback_;  ///< Optional worker event observer.
    mutable std::mutex mutex_;  ///< Protects desired slots and lifecycle controls.
    std::condition_variable condition_;  ///< Interrupts reconnect delay and announces work.
    std::array<slot_t, platf::MAX_GAMEPADS> slots_ {};  ///< Bounded desired state.
    std::deque<message_t> controls_;  ///< Bounded ordered lifecycle messages.
    bool force_reconnect_ = false;  ///< Rebuild state after control overflow.
    std::atomic<bool> shutdown_ {false};  ///< Requests bounded worker cleanup and exit.
    std::mutex event_mutex_;  ///< Protects event limiter timestamps.
    std::array<std::optional<std::chrono::steady_clock::time_point>, 8> last_event_ {};  ///< Last reported error per event.
    mutable std::mutex diagnostic_mutex_;  ///< Protects diagnostic state exposed outside the worker.
    client_diagnostics_t diagnostics_;  ///< Latest endpoint, connection, error, and controller status.
    std::thread worker_;  ///< Sole owner and user of each live transport; declared last so observed state is initialized first.
  };

  client_t::client_t(client_options_t options, transport_factory_t transport_factory, client_event_callback_t event_callback):
      impl_(std::make_unique<impl_t>(std::move(options), std::move(transport_factory), std::move(event_callback))) {
  }

  client_t::~client_t() = default;

  bool client_t::attach(std::uint8_t controller_id, std::uint8_t client_relative_id) {
    return impl_->attach(controller_id, client_relative_id);
  }

  bool client_t::rebind(std::uint8_t controller_id, std::uint8_t client_relative_id) {
    return impl_->rebind(controller_id, client_relative_id);
  }

  bool client_t::update(const controller_state_t &state) {
    return impl_->update(state);
  }

  void client_t::neutralize(std::uint8_t controller_id) {
    impl_->neutralize(controller_id);
  }

  void client_t::detach(std::uint8_t controller_id) {
    impl_->detach(controller_id);
  }

  std::size_t client_t::pending_state_count() const {
    return impl_->pending_state_count();
  }

  client_diagnostics_t client_t::diagnostics() const {
    return impl_->diagnostics();
  }
}  // namespace input::nxbt
