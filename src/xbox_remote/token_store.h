/**
 * @file src/xbox_remote/token_store.h
 * @brief Secure persistence for Microsoft OAuth credentials.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace xbox_remote::auth {
  /**
   * @brief OAuth credentials that may be persisted between Sunshine restarts.
   */
  struct oauth_credentials_t {
    std::string access_token;  ///< Short-lived Microsoft access token.
    std::string refresh_token;  ///< Long-lived Microsoft refresh token.
    std::chrono::system_clock::time_point expires_at {};  ///< Access-token expiry on the wall clock.
  };

  /**
   * @brief Error categories returned by the secure token store.
   */
  enum class token_store_error_e {
    none,  ///< The operation succeeded.
    not_found,  ///< The token file does not exist.
    insecure_file,  ///< Ownership, type, permissions, or symlink checks failed.
    invalid_data,  ///< Stored JSON is malformed or incomplete.
    too_large,  ///< The token file exceeds the bounded input size.
    io_error,  ///< A filesystem operation failed.
    unsupported,  ///< Secure persistence is unavailable on this platform.
  };

  /**
   * @brief Result of one token-store operation.
   */
  struct token_store_result_t {
    token_store_error_e error = token_store_error_e::none;  ///< Machine-readable outcome.
    std::string message;  ///< Sanitized diagnostic that never contains token values.

    /**
     * @brief Check whether the operation succeeded.
     *
     * @return @c true when @c error is @c token_store_error_e::none.
     */
    explicit operator bool() const;
  };

  /**
   * @brief Determine whether an access token should be refreshed.
   *
   * @param credentials Credentials containing the wall-clock expiry.
   * @param now Current injectable wall-clock time.
   * @param refresh_margin Time reserved for an early refresh.
   * @return @c true when no access token exists or it expires within the margin.
   */
  bool should_refresh(
    const oauth_credentials_t &credentials,
    std::chrono::system_clock::time_point now,
    std::chrono::seconds refresh_margin = std::chrono::minutes(5)
  );

  /**
   * @brief Persist OAuth credentials using an owner-only atomic file update.
   *
   * The Linux implementation writes a mode-0600 temporary file in the target
   * directory, flushes its contents, atomically renames it, and flushes the
   * parent directory. Microsoft access and refresh tokens are the only secrets
   * persisted by this first-version store; Xbox and GSSV tokens stay in memory.
   */
  class token_store_t {
  public:
    /**
     * @brief Create a token store for one explicit file path.
     *
     * @param path Destination token file.
     */
    explicit token_store_t(std::filesystem::path path);

    /**
     * @brief Return the configured token-file path.
     *
     * @return Immutable token-file path.
     */
    const std::filesystem::path &path() const;

    /**
     * @brief Load credentials after validating file ownership and permissions.
     *
     * @param credentials Destination modified only when loading succeeds.
     * @return Sanitized operation result.
     */
    token_store_result_t load(oauth_credentials_t &credentials) const;

    /**
     * @brief Atomically save credentials with owner read/write permissions.
     *
     * @param credentials Credentials to persist.
     * @return Sanitized operation result.
     */
    token_store_result_t save(const oauth_credentials_t &credentials) const;

  private:
    std::filesystem::path path_;  ///< Explicit token-file location.
  };
}  // namespace xbox_remote::auth
