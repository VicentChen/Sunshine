/**
 * @file src/xbox_remote/http_runtime.cpp
 * @brief Production HTTPS and clock adapters for Xbox Remote Play authentication.
 */

#include "src/xbox_remote/http_runtime.h"

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// library includes
#include <curl/curl.h>

namespace xbox_remote::auth {
  namespace {
    constexpr std::size_t maximum_response_size = 1024 * 1024;
    constexpr auto cancellation_granularity = std::chrono::milliseconds {100};

    /**
     * @brief Supported HTTP request methods.
     */
    enum class request_method_e {
      get,  ///< HTTP GET.
      post,  ///< HTTP POST.
      remove,  ///< HTTP DELETE.
    };

    /**
     * @brief Append a libcurl response chunk to bounded storage.
     *
     * @param data Response chunk.
     * @param size Element size.
     * @param count Element count.
     * @param context Destination string.
     * @return Consumed byte count, or zero when the response is too large.
     */
    std::size_t append_response(char *data, std::size_t size, std::size_t count, void *context) {
      auto &response = *static_cast<std::string *>(context);
      if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        return 0;
      }
      const auto bytes = size * count;
      if (bytes > maximum_response_size - std::min(response.size(), maximum_response_size)) {
        return 0;
      }
      response.append(data, bytes);
      return bytes;
    }

    /**
     * @brief Abort a libcurl transfer when the worker requests cancellation.
     *
     * @param context Cancellation callback.
     * @param download_total Unused expected download byte count.
     * @param download_now Unused received download byte count.
     * @param upload_total Unused expected upload byte count.
     * @param upload_now Unused transmitted upload byte count.
     * @return One to abort the transfer, otherwise zero.
     */
    int abort_if_cancelled(void *context, curl_off_t download_total, curl_off_t download_now, curl_off_t upload_total, curl_off_t upload_now) {
      static_cast<void>(download_total);
      static_cast<void>(download_now);
      static_cast<void>(upload_total);
      static_cast<void>(upload_now);
      const auto &cancelled = *static_cast<const std::function<bool()> *>(context);
      try {
        return cancelled && cancelled() ? 1 : 0;
      } catch (...) {
        return 1;
      }
    }

    /**
     * @brief Initialize libcurl process state once without racing other clients.
     *
     * @return @c true when initialization succeeded.
     */
    bool initialize_curl() {
      static std::once_flag once;
      static CURLcode result = CURLE_FAILED_INIT;
      std::call_once(once, []() {
        result = curl_global_init(CURL_GLOBAL_DEFAULT);
      });
      return result == CURLE_OK;
    }

    /**
     * @brief Perform one bounded libcurl request.
     *
     * @param method HTTP method.
     * @param url HTTPS endpoint.
     * @param body Optional POST body.
     * @param headers Request headers.
     * @param timeout Hard request deadline.
     * @param cancelled Callback sampled before and during the transfer.
     * @return HTTP response or sanitized transport failure.
     */
    http_response_t perform_request(
      request_method_e method,
      std::string_view url,
      std::string_view body,
      const std::map<std::string, std::string> &headers,
      std::chrono::seconds timeout,
      const std::function<bool()> &cancelled
    ) {
      http_response_t response;
      if ((cancelled && cancelled()) || !initialize_curl() || !url.starts_with("https://") || timeout <= std::chrono::seconds::zero()) {
        response.network_error = true;
        return response;
      }

      CURL *handle = curl_easy_init();
      if (handle == nullptr) {
        response.network_error = true;
        return response;
      }

      curl_slist *header_list = nullptr;
      bool headers_valid = true;
      for (const auto &[name, value] : headers) {
        auto *next = curl_slist_append(header_list, (name + ": " + value).c_str());
        if (next == nullptr) {
          headers_valid = false;
          break;
        }
        header_list = next;
      }

      const std::string url_storage {url};
      curl_easy_setopt(handle, CURLOPT_URL, url_storage.c_str());
      if (method == request_method_e::post) {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
      } else if (method == request_method_e::remove) {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
      } else {
        curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
      }
      curl_easy_setopt(handle, CURLOPT_HTTPHEADER, header_list);
      curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
      curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
      curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count()));
      curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_response);
      curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
      curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, abort_if_cancelled);
      curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &cancelled);
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
#else
      curl_easy_setopt(handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif

      const auto transfer = headers_valid ? curl_easy_perform(handle) : CURLE_OUT_OF_MEMORY;
      long status = 0;
      if (transfer == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        response.status_code = status > 0 && status <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(status) : 0;
      } else {
        response.network_error = true;
        response.body.clear();
      }

      curl_slist_free_all(header_list);
      curl_easy_cleanup(handle);
      return response;
    }
  }  // namespace

  void curl_http_client_t::set_cancellation_callback(std::function<bool()> cancelled) {
    cancelled_ = std::move(cancelled);
  }

  http_response_t curl_http_client_t::post(
    std::string_view url,
    std::string_view body,
    const std::map<std::string, std::string> &headers,
    std::chrono::seconds timeout
  ) {
    return perform_request(request_method_e::post, url, body, headers, timeout, cancelled_);
  }

  http_response_t curl_http_client_t::get(
    std::string_view url,
    const std::map<std::string, std::string> &headers,
    std::chrono::seconds timeout
  ) {
    return perform_request(request_method_e::get, url, {}, headers, timeout, cancelled_);
  }

  http_response_t curl_http_client_t::remove(
    std::string_view url,
    const std::map<std::string, std::string> &headers,
    std::chrono::seconds timeout
  ) {
    return perform_request(request_method_e::remove, url, {}, headers, timeout, cancelled_);
  }

  std::chrono::system_clock::time_point system_runtime_t::system_now() const {
    return std::chrono::system_clock::now();
  }

  std::chrono::steady_clock::time_point system_runtime_t::steady_now() const {
    return std::chrono::steady_clock::now();
  }

  bool system_runtime_t::wait_for(std::chrono::seconds duration, const std::function<bool()> &cancelled) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
      if (cancelled && cancelled()) {
        return false;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      std::this_thread::sleep_for(std::min(remaining, cancellation_granularity));
    }
    return !(cancelled && cancelled());
  }
}  // namespace xbox_remote::auth
