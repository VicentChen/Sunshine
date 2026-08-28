/**
 * @file tests/unit/platform/test_linux_audio_source.cpp
 * @brief Tests for Linux audio source selection.
 */
#include "../../tests_common.h"

#if defined(__linux__)
  #include <src/platform/linux/audio.h>

TEST(LinuxAudioSourceSelection, UsesConfiguredSource) {
  EXPECT_EQ(
    platf::pa::resolve_capture_source_name("alsa_input.hdmi", "sink.monitor"),
    "alsa_input.hdmi"
  );
}

TEST(LinuxAudioSourceSelection, FallsBackToSinkMonitor) {
  EXPECT_EQ(platf::pa::resolve_capture_source_name("", "sink.monitor"), "sink.monitor");
}
#endif
