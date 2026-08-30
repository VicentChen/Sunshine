/**
 * @file src/xbox_remote/token_store.cpp
 * @brief Owner-only atomic token-file implementation.
 */

#include "src/xbox_remote/token_store.h"

// standard includes
#include <cerrno>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

// library includes
#include <nlohmann/json.hpp>

#ifndef _WIN32
  // system includes
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace xbox_remote::auth {
  namespace {
    using json = nlohmann::json;

    constexpr std::size_t maximum_token_file_size = 64 * 1024;

    /**
     * @brief Construct a sanitized token-store failure.
     *
     * @param error Machine-readable error.
     * @param message Diagnostic without path contents or secrets.
     * @return Populated failure result.
     */
    token_store_result_t failure(token_store_error_e error, std::string message) {
      return {error, std::move(message)};
    }

    /**
     * @brief Parse bounded token JSON into a temporary credentials object.
     *
     * @param source Stored JSON.
     * @param credentials Parsed destination.
     * @return Sanitized parse result.
     */
    token_store_result_t parse_credentials(std::string_view source, oauth_credentials_t &credentials) {
      try {
        const auto document = json::parse(source);
        if (!document.is_object()) {
          return failure(token_store_error_e::invalid_data, "token store root must be an object");
        }
        const auto version = document.find("version");
        const auto access_token = document.find("access_token");
        const auto refresh_token = document.find("refresh_token");
        const auto expires_at = document.find("expires_at_unix_seconds");
        if (version == document.end() || !version->is_number_integer() || version->get<std::int64_t>() != 1) {
          return failure(token_store_error_e::invalid_data, "token store version is missing or unsupported");
        }
        if (access_token == document.end() || !access_token->is_string() || access_token->get_ref<const std::string &>().empty()) {
          return failure(token_store_error_e::invalid_data, "token store access token is missing");
        }
        if (refresh_token == document.end() || !refresh_token->is_string() || refresh_token->get_ref<const std::string &>().empty()) {
          return failure(token_store_error_e::invalid_data, "token store refresh token is missing");
        }
        if (expires_at == document.end() || !expires_at->is_number_integer()) {
          return failure(token_store_error_e::invalid_data, "token store expiry is missing");
        }
        const auto expiry_seconds = expires_at->get<std::int64_t>();
        if (expiry_seconds <= 0) {
          return failure(token_store_error_e::invalid_data, "token store expiry is invalid");
        }

        oauth_credentials_t parsed;
        parsed.access_token = access_token->get<std::string>();
        parsed.refresh_token = refresh_token->get<std::string>();
        parsed.expires_at = std::chrono::system_clock::time_point {std::chrono::seconds {expiry_seconds}};
        credentials = std::move(parsed);
        return {};
      } catch (const json::exception &) {
        return failure(token_store_error_e::invalid_data, "token store contains invalid JSON");
      }
    }

    /**
     * @brief Encode credentials without any derived Xbox tokens.
     *
     * @param credentials Credentials to encode.
     * @return Compact JSON document.
     */
    std::string serialize_credentials(const oauth_credentials_t &credentials) {
      const auto expiry_seconds = std::chrono::duration_cast<std::chrono::seconds>(credentials.expires_at.time_since_epoch()).count();
      return json {
        {"version", 1},
        {"access_token", credentials.access_token},
        {"refresh_token", credentials.refresh_token},
        {"expires_at_unix_seconds", expiry_seconds},
      }
        .dump();
    }

#ifndef _WIN32
    /**
     * @brief Close a descriptor while preserving the caller's errno.
     *
     * @param descriptor Open descriptor, or a negative value.
     */
    void close_preserving_errno(int descriptor) {
      if (descriptor < 0) {
        return;
      }
      const int saved_errno = errno;
      ::close(descriptor);
      errno = saved_errno;
    }

    /**
     * @brief Remove a temporary file while preserving the caller's errno.
     *
     * @param path Temporary file path.
     */
    void unlink_preserving_errno(const std::filesystem::path &path) {
      const int saved_errno = errno;
      ::unlink(path.c_str());
      errno = saved_errno;
    }

    /**
     * @brief Write all bytes, retrying interrupted system calls.
     *
     * @param descriptor Destination descriptor.
     * @param data Bytes to write.
     * @return @c true when every byte was written.
     */
    bool write_all(int descriptor, std::string_view data) {
      std::size_t offset = 0;
      while (offset < data.size()) {
        const auto written = ::write(descriptor, data.data() + offset, data.size() - offset);
        if (written > 0) {
          offset += static_cast<std::size_t>(written);
          continue;
        }
        if (written < 0 && errno == EINTR) {
          continue;
        }
        return false;
      }
      return true;
    }
#endif
  }  // namespace

  token_store_result_t::operator bool() const {
    return error == token_store_error_e::none;
  }

  bool should_refresh(const oauth_credentials_t &credentials, std::chrono::system_clock::time_point now, std::chrono::seconds refresh_margin) {
    if (credentials.access_token.empty()) {
      return true;
    }
    if (refresh_margin < std::chrono::seconds::zero()) {
      refresh_margin = std::chrono::seconds::zero();
    }
    return credentials.expires_at <= now + refresh_margin;
  }

  token_store_t::token_store_t(std::filesystem::path path):
      path_(std::move(path)) {
  }

  const std::filesystem::path &token_store_t::path() const {
    return path_;
  }

  token_store_result_t token_store_t::load(oauth_credentials_t &credentials) const {
#ifdef _WIN32
    return failure(token_store_error_e::unsupported, "secure token persistence is unavailable on this platform");
#else
    if (path_.empty() || path_.filename().empty()) {
      return failure(token_store_error_e::io_error, "token store path is invalid");
    }

    const int descriptor = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
      if (errno == ENOENT) {
        return failure(token_store_error_e::not_found, "token store does not exist");
      }
      if (errno == ELOOP) {
        return failure(token_store_error_e::insecure_file, "token store must not be a symbolic link");
      }
      return failure(token_store_error_e::io_error, "token store could not be opened");
    }

    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0) {
      close_preserving_errno(descriptor);
      return failure(token_store_error_e::io_error, "token store metadata could not be read");
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() || (metadata.st_mode & 0077) != 0) {
      close_preserving_errno(descriptor);
      return failure(token_store_error_e::insecure_file, "token store ownership, type, or permissions are insecure");
    }
    if (metadata.st_size <= 0) {
      close_preserving_errno(descriptor);
      return failure(token_store_error_e::invalid_data, "token store is empty");
    }
    if (static_cast<std::uintmax_t>(metadata.st_size) > maximum_token_file_size) {
      close_preserving_errno(descriptor);
      return failure(token_store_error_e::too_large, "token store exceeds the size limit");
    }

    std::string source(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < source.size()) {
      const auto count = ::read(descriptor, source.data() + offset, source.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      close_preserving_errno(descriptor);
      return failure(token_store_error_e::io_error, "token store could not be read completely");
    }
    if (::close(descriptor) != 0) {
      return failure(token_store_error_e::io_error, "token store could not be closed");
    }
    return parse_credentials(source, credentials);
#endif
  }

  token_store_result_t token_store_t::save(const oauth_credentials_t &credentials) const {
#ifdef _WIN32
    return failure(token_store_error_e::unsupported, "secure token persistence is unavailable on this platform");
#else
    if (path_.empty() || path_.filename().empty()) {
      return failure(token_store_error_e::io_error, "token store path is invalid");
    }
    if (credentials.access_token.empty() || credentials.refresh_token.empty() || credentials.expires_at.time_since_epoch() <= std::chrono::system_clock::duration::zero()) {
      return failure(token_store_error_e::invalid_data, "refusing to save incomplete OAuth credentials");
    }

    const auto parent = path_.parent_path().empty() ? std::filesystem::path {"."} : path_.parent_path();
    const auto temporary = parent / ("." + path_.filename().string() + ".tmp");
    const auto encoded = serialize_credentials(credentials);
    const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      return failure(errno == ELOOP ? token_store_error_e::insecure_file : token_store_error_e::io_error, "temporary token store could not be opened");
    }
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0 || !write_all(descriptor, encoded) || ::fsync(descriptor) != 0) {
      close_preserving_errno(descriptor);
      unlink_preserving_errno(temporary);
      return failure(token_store_error_e::io_error, "temporary token store could not be written safely");
    }
    if (::close(descriptor) != 0) {
      unlink_preserving_errno(temporary);
      return failure(token_store_error_e::io_error, "temporary token store could not be closed");
    }
    if (::rename(temporary.c_str(), path_.c_str()) != 0) {
      unlink_preserving_errno(temporary);
      return failure(token_store_error_e::io_error, "token store could not be replaced atomically");
    }

    const int directory = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory < 0) {
      return failure(token_store_error_e::io_error, "token store directory could not be opened for synchronization");
    }
    if (::fsync(directory) != 0) {
      close_preserving_errno(directory);
      return failure(token_store_error_e::io_error, "token store directory could not be synchronized");
    }
    if (::close(directory) != 0) {
      return failure(token_store_error_e::io_error, "token store directory could not be closed");
    }
    return {};
#endif
  }
}  // namespace xbox_remote::auth
