/**
 * @file src/platform/linux/audio.h
 * @brief Declarations for Linux audio capture selection.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// local includes
#include "src/platform/common.h"

namespace platf::pa {
  /**
   * @brief Select the PulseAudio source used for capture.
   *
   * @param configured_source Explicit source configured by the user.
   * @param monitor_source Monitor source resolved from the selected sink.
   * @return The configured source when present; otherwise the sink monitor source.
   */
  std::string resolve_capture_source_name(std::string_view configured_source, std::string_view monitor_source);

  /**
   * @brief Translate a PulseAudio read result into Sunshine capture status.
   *
   * A failed PulseAudio stream can become usable again after HDMI HPD creates
   * a replacement ALSA node, so callers must request reinitialization instead
   * of permanently ending audio for the active Moonlight session.
   *
   * @param read_result Return value from pa_simple_read().
   * @return Capture success for zero, otherwise a reinitialization request.
   */
  capture_e capture_status_from_read(int read_result) noexcept;
}  // namespace platf::pa
