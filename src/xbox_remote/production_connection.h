/**
 * @file src/xbox_remote/production_connection.h
 * @brief Production Xbox authentication, REST, WebRTC, and input connection.
 */
#pragma once

// standard includes
#include <filesystem>
#include <memory>
#include <string>

// local includes
#include "src/xbox_remote/worker.h"

namespace xbox_remote::auth {
  struct auth_failure_t;
}

namespace xbox_remote::session {
  struct failure_t;
}

namespace xbox_remote::startup {
  struct failure_t;
}

namespace xbox_remote::transport {
  struct failure_t;
}

namespace xbox_remote::production {
  /**
   * @brief Classify an authentication failure for bounded worker recovery.
   *
   * @param error Sanitized authentication failure.
   * @return Retry or reauthentication policy.
   */
  worker::failure_kind_e classify_authentication_failure(const auth::auth_failure_t &error);

  /**
   * @brief Classify a Home REST failure for bounded worker recovery.
   *
   * @param error Sanitized Home failure.
   * @return Retry, reauthentication, or permanent policy.
   */
  worker::failure_kind_e classify_session_failure(const session::failure_t &error);

  /**
   * @brief Classify a WebRTC failure for bounded worker recovery.
   *
   * @param error Sanitized transport failure.
   * @return Retry or permanent policy.
   */
  worker::failure_kind_e classify_transport_failure(const transport::failure_t &error);

  /**
   * @brief Classify a startup-handshake failure for bounded worker recovery.
   *
   * @param error Sanitized startup failure.
   * @return Retry or permanent policy.
   */
  worker::failure_kind_e classify_startup_failure(const startup::failure_t &error);

  /**
   * @brief Application-scoped production connection configuration.
   */
  struct options_t {
    std::filesystem::path token_file;  ///< Owner-only Microsoft OAuth credential file.
    std::string console_id;  ///< Stable Xbox Home console identifier, or empty for unique auto-selection; never logged.
    bool wake_console = true;  ///< Whether to execute the XCCS WakeUp gate before provisioning.
  };

  /**
   * @brief Create a factory for production Xbox Remote Play connections.
   *
   * @param options Immutable credentials, console selection, and WakeUp controls.
   * @return Factory suitable for @ref worker::session_t.
   */
  worker::connection_factory_t make_connection_factory(options_t options);
}  // namespace xbox_remote::production
