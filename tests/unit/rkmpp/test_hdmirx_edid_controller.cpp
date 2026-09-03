/**
 * @file tests/unit/rkmpp/test_hdmirx_edid_controller.cpp
 * @brief Tests the process-level idempotent HDMI RX EDID control plane.
 */

#include "src/platform/linux/hdmirx_edid_controller.h"
#include "tests/unit/rkmpp/edid_test_fixtures.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {
  using namespace platf::edid;
  using namespace platf::hdmirx;

  /** @brief In-memory receiver that models HPD events caused by S_EDID. */
  class event_receiver_t final: public ioctl_backend_t {
  public:
    std::vector<std::uint8_t> bytes {edid_test::make_native_edid()};  ///< Installed EDID.
    std::uint32_t reads {};  ///< Number of EDID reads.
    std::uint32_t writes {};  ///< Number of EDID writes.
    std::uint32_t hpd_events {};  ///< Synthetic plugout/hotplug/source-change events.
    std::uint32_t audio_changes {};  ///< Audio-state calls.
    bool fail_reads {};  ///< Whether reads fail.
    bool fail_first_write {};  ///< Whether the first write is partial.
    bool corrupt_first_readback {};  ///< Whether the first post-write EDID is corrupted.
    bool wrote_since_read {};  ///< Tracks readback behavior.

    edid_result_t<std::uint32_t> get_edid(
      std::uint32_t,
      std::uint32_t,
      std::uint32_t blocks,
      std::span<std::uint8_t> output
    ) override {
      ++reads;
      if (fail_reads) {
        return std::unexpected(edid_error_t {error_category_e::io_error, EIO, "read failed"});
      }
      std::copy_n(bytes.begin(), static_cast<std::size_t>(blocks) * k_edid_block_size, output.begin());
      if (corrupt_first_readback && wrote_since_read) {
        output[20] ^= 1U;
        if (blocks > 1U) {
          corrupt_first_readback = false;
        }
      }
      if (!corrupt_first_readback) {
        wrote_since_read = false;
      }
      return blocks;
    }

    edid_result_t<std::uint32_t> set_edid(
      std::uint32_t,
      std::uint32_t,
      std::uint32_t blocks,
      std::span<const std::uint8_t> input
    ) override {
      ++writes;
      hpd_events += 3U;
      if (fail_first_write && writes == 1U) {
        return blocks - 1U;
      }
      bytes.assign(input.begin(), input.end());
      wrote_since_read = true;
      return blocks;
    }

    edid_result_t<void> set_audio_enabled(bool) override {
      ++audio_changes;
      return {};
    }
  };

  TEST(HdmirxEdidController, MatchingSelectedLiveModeReadsBaselineButDoesNotWrite) {
    event_receiver_t receiver;
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1920, 1080}, {{60, 1}}, resolution_t {1920, 1080});
    EXPECT_EQ(result.status, edid_apply_status_e::live_match);
    ASSERT_TRUE(result.selected_mode);
    EXPECT_EQ(result.selected_mode->resolution, (resolution_t {1920, 1080}));
    EXPECT_EQ(receiver.reads, 2U);
    EXPECT_EQ(receiver.writes, 0U);
    EXPECT_EQ(receiver.hpd_events, 0U);
    EXPECT_EQ(receiver.audio_changes, 1U);
  }

  TEST(HdmirxEdidController, SelectsClosestNativeModeAndWritesOnce) {
    event_receiver_t receiver;
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1680, 1050}, {{60, 1}});
    ASSERT_EQ(result.status, edid_apply_status_e::advertised);
    ASSERT_TRUE(result.selected_mode);
    EXPECT_EQ(result.selected_mode->resolution, (resolution_t {1600, 900}));
    EXPECT_EQ(receiver.writes, 1U);
    EXPECT_EQ(receiver.hpd_events, 3U);
    const auto modes = parse_edid_modes(receiver.bytes);
    ASSERT_FALSE(modes.empty());
    EXPECT_TRUE(std::all_of(modes.begin(), modes.end(), [](const auto &mode) {
      return mode.resolution.width <= 1600U && mode.resolution.height <= 900U;
    }));
    EXPECT_TRUE(std::any_of(modes.begin(), modes.end(), [](const auto &mode) {
      return mode.resolution == resolution_t {640, 480};
    }));
  }

  TEST(HdmirxEdidController, FourKInputIsRenegotiatedForA1080pMoonlightTarget) {
    event_receiver_t receiver;
    receiver.bytes = edid_test::make_rockchip_340mhz_edid();
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1920, 1080}, {{60, 1}}, resolution_t {3840, 2160});
    ASSERT_EQ(result.status, edid_apply_status_e::advertised);
    ASSERT_TRUE(result.selected_mode);
    EXPECT_EQ(result.selected_mode->resolution, (resolution_t {1920, 1080}));
    EXPECT_EQ(receiver.writes, 1U);
    const auto modes = parse_edid_modes(receiver.bytes);
    EXPECT_FALSE(std::any_of(modes.begin(), modes.end(), [](const auto &mode) {
      return mode.resolution == resolution_t {3840, 2160};
    }));
  }

  TEST(HdmirxEdidController, Rockchip340ProfileAdvertises4kWithoutA600MHzTemplate) {
    event_receiver_t receiver;
    receiver.bytes = edid_test::make_rockchip_340mhz_edid();
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {3840, 2160}, {{60, 1}}, resolution_t {640, 480});
    ASSERT_EQ(result.status, edid_apply_status_e::advertised);
    ASSERT_TRUE(result.selected_mode);
    EXPECT_EQ(result.selected_mode->resolution, (resolution_t {3840, 2160}));
    EXPECT_EQ(result.selected_mode->refresh_rate, (refresh_rate_t {60, 1}));
    EXPECT_EQ(receiver.writes, 1U);
    const auto catalog = parse_mode_catalog(receiver.bytes);
    EXPECT_TRUE(std::any_of(catalog.begin(), catalog.end(), [](const auto &record) {
      return record.mode.resolution == resolution_t {3840, 2160} && record.origin == mode_origin_e::cta_y420_vdb;
    }));
    EXPECT_TRUE(std::any_of(catalog.begin(), catalog.end(), [](const auto &record) {
      return record.mode.resolution == resolution_t {1920, 1080} && record.origin == mode_origin_e::base_dtd;
    }));
  }

  TEST(HdmirxEdidController, ExistingTargetBytesAreReassertedUntilLiveTimingMatches) {
    event_receiver_t receiver;
    edid_controller_t controller;
    ASSERT_EQ(controller.apply_target(receiver, 0, {1920, 1080}, {{60, 1}}, resolution_t {3840, 2160}).status, edid_apply_status_e::advertised);
    const auto first_profile = receiver.bytes;
    const auto repeated = controller.apply_target(receiver, 0, {1920, 1080}, {{60, 1}}, resolution_t {640, 480});
    EXPECT_EQ(repeated.status, edid_apply_status_e::advertised);
    EXPECT_EQ(receiver.bytes, first_profile);
    EXPECT_EQ(receiver.writes, 2U);
  }

  TEST(HdmirxEdidController, MismatchedLiveModeReassertsTargetForANewTransaction) {
    event_receiver_t receiver;
    edid_controller_t controller;
    ASSERT_EQ(controller.apply_target(receiver, 0, {1280, 720}).status, edid_apply_status_e::advertised);
    const auto events_after_write = receiver.hpd_events;

    const auto repeated = controller.apply_target(receiver, 0, {1280, 720}, std::nullopt, resolution_t {1920, 1080});
    EXPECT_EQ(repeated.status, edid_apply_status_e::advertised);
    EXPECT_EQ(receiver.writes, 2U);
    EXPECT_GT(receiver.hpd_events, events_after_write);
    EXPECT_EQ(receiver.audio_changes, 1U);
  }

  TEST(HdmirxEdidController, NewMoonlightTargetUsesCachedNativeBaseline) {
    event_receiver_t receiver;
    edid_controller_t controller;
    ASSERT_EQ(controller.apply_target(receiver, 0, {1280, 720}).status, edid_apply_status_e::advertised);
    const auto second = controller.apply_target(receiver, 0, {3840, 2160});
    ASSERT_EQ(second.status, edid_apply_status_e::advertised);
    ASSERT_TRUE(second.selected_mode);
    EXPECT_EQ(second.selected_mode->resolution, (resolution_t {3840, 2160}));
    EXPECT_EQ(receiver.writes, 2U);
  }

  TEST(HdmirxEdidController, PartialWriteGetsOneBoundedNativeRestore) {
    event_receiver_t receiver;
    const auto native = receiver.bytes;
    receiver.fail_first_write = true;
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1280, 720});
    EXPECT_EQ(result.status, edid_apply_status_e::write_failed);
    EXPECT_EQ(receiver.writes, 2U);
    EXPECT_EQ(receiver.bytes, native);
  }

  TEST(HdmirxEdidController, MismatchedReadbackGetsOneBoundedNativeRestore) {
    event_receiver_t receiver;
    const auto native = receiver.bytes;
    receiver.corrupt_first_readback = true;
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1280, 720});
    EXPECT_EQ(result.status, edid_apply_status_e::verify_failed);
    EXPECT_EQ(receiver.writes, 2U);
    EXPECT_EQ(receiver.bytes, native);
  }

  TEST(HdmirxEdidController, DestructionNeverRestoresOrCreatesAnotherHpdCycle) {
    event_receiver_t receiver;
    {
      edid_controller_t controller;
      ASSERT_EQ(controller.apply_target(receiver, 0, {1280, 720}).status, edid_apply_status_e::advertised);
      EXPECT_EQ(receiver.writes, 1U);
    }
    EXPECT_EQ(receiver.writes, 1U);
    EXPECT_EQ(receiver.hpd_events, 3U);
  }

  TEST(HdmirxEdidController, RestartRecognizesLastProjectionAndRetainsNativeCatalog) {
    const auto state_path = std::filesystem::temp_directory_path() /
                            ("sunshine-hdmirx-edid-" + std::to_string(::getpid()) + ".bin");
    std::filesystem::remove(state_path);
    event_receiver_t receiver;
    {
      edid_controller_t first_process {state_path};
      ASSERT_EQ(first_process.apply_target(receiver, 0, {1280, 720}).status, edid_apply_status_e::advertised);
    }
    {
      edid_controller_t restarted_process {state_path};
      const auto result = restarted_process.apply_target(receiver, 0, {3840, 2160});
      ASSERT_EQ(result.status, edid_apply_status_e::advertised);
      ASSERT_TRUE(result.selected_mode);
      EXPECT_EQ(result.selected_mode->resolution, (resolution_t {3840, 2160}));
    }
    EXPECT_EQ(receiver.writes, 2U);
    std::filesystem::remove(state_path);
  }

  TEST(HdmirxEdidController, ReadFailureDoesNotWrite) {
    event_receiver_t receiver;
    receiver.fail_reads = true;
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1920, 1080});
    EXPECT_EQ(result.status, edid_apply_status_e::read_failed);
    EXPECT_EQ(receiver.writes, 0U);
  }

  TEST(HdmirxEdidController, ExactLiveTargetSurvivesAnEdidReadFailure) {
    event_receiver_t receiver;
    receiver.fail_reads = true;
    edid_controller_t controller;
    const auto result = controller.apply_target(receiver, 0, {1920, 1080}, {{60, 1}}, resolution_t {1920, 1080});
    EXPECT_EQ(result.status, edid_apply_status_e::live_match);
    EXPECT_FALSE(result.selected_mode);
    EXPECT_EQ(receiver.writes, 0U);
    EXPECT_EQ(receiver.audio_changes, 1U);
  }

  TEST(HdmirxInputModeVerifier, ReportsMatchExactlyOnce) {
    input_mode_verifier_t verifier;
    const auto start = std::chrono::steady_clock::time_point {};
    verifier.begin({1920, 1080}, start, std::chrono::seconds(5));
    EXPECT_FALSE(verifier.observe(resolution_t {640, 480}, start + std::chrono::seconds(1)));
    const auto matched = verifier.observe(resolution_t {1920, 1080}, start + std::chrono::seconds(2));
    ASSERT_TRUE(matched);
    EXPECT_EQ(matched->status, input_mode_status_e::matched);
    EXPECT_EQ(matched->expected, (resolution_t {1920, 1080}));
    EXPECT_EQ(matched->actual, (resolution_t {1920, 1080}));
    EXPECT_FALSE(verifier.observe(resolution_t {1920, 1080}, start + std::chrono::seconds(3)));
  }

  TEST(HdmirxInputModeVerifier, ReportsLastMismatchedTimingAtDeadline) {
    input_mode_verifier_t verifier;
    const auto start = std::chrono::steady_clock::time_point {};
    verifier.begin({3840, 2160}, start, std::chrono::seconds(5));
    EXPECT_FALSE(verifier.observe(std::nullopt, start + std::chrono::seconds(3)));
    EXPECT_FALSE(verifier.observe(resolution_t {640, 480}, start + std::chrono::seconds(4)));
    const auto timed_out = verifier.observe(std::nullopt, start + std::chrono::seconds(5));
    ASSERT_TRUE(timed_out);
    EXPECT_EQ(timed_out->status, input_mode_status_e::timed_out);
    EXPECT_EQ(timed_out->actual, (resolution_t {640, 480}));
    EXPECT_FALSE(verifier.observe(std::nullopt, start + std::chrono::seconds(6)));
  }

  TEST(HdmirxInputModeVerifier, ReportsMissingTimingAtDeadline) {
    input_mode_verifier_t verifier;
    const auto start = std::chrono::steady_clock::time_point {};
    verifier.begin({3840, 2160}, start, std::chrono::seconds(5));
    const auto timed_out = verifier.observe(std::nullopt, start + std::chrono::seconds(5));
    ASSERT_TRUE(timed_out);
    EXPECT_EQ(timed_out->status, input_mode_status_e::timed_out);
    EXPECT_FALSE(timed_out->actual);
  }
}  // namespace
