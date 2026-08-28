/**
 * @file src/platform/linux/audio.h
 * @brief Declarations for Linux audio capture selection.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

namespace platf::pa {
  /**
   * @brief Select the PulseAudio source used for capture.
   *
   * @param configured_source Explicit source configured by the user.
   * @param monitor_source Monitor source resolved from the selected sink.
   * @return The configured source when present; otherwise the sink monitor source.
   */
  std::string resolve_capture_source_name(std::string_view configured_source, std::string_view monitor_source);
}  // namespace platf::pa
