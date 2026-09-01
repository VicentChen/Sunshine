/**
 * @file tests/unit/rkmpp/test_edid_negotiator.cpp
 * @brief Tests HDMI RX session EDID negotiation and fallback behavior.
 */

#include "src/platform/linux/edid.h"
#include "src/platform/linux/input_state_machine.h"

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

using namespace platf::hdmirx;
using namespace platf::edid;
using namespace platf::input_sm;

class mock_ioctl_backend_t final: public ioctl_backend_t {
public:
  using get_edid_fn = std::function<edid_result_t<std::uint32_t>(
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::span<std::uint8_t>
  )>;
  using set_edid_fn = std::function<edid_result_t<std::uint32_t>(
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t>
  )>;
  using set_audio_enabled_fn = std::function<edid_result_t<void>(bool)>;

  get_edid_fn on_get_edid;
  set_edid_fn on_set_edid;
  set_audio_enabled_fn on_set_audio_enabled;

  edid_result_t<std::uint32_t> get_edid(
    std::uint32_t pad,
    std::uint32_t start,
    std::uint32_t blocks,
    std::span<std::uint8_t> buffer
  ) override {
    if (on_get_edid) {
      return on_get_edid(pad, start, blocks, buffer);
    }
    return std::unexpected(edid_error_t {error_category_e::not_supported, ENOTTY, ""});
  }

  edid_result_t<std::uint32_t> set_edid(
    std::uint32_t pad,
    std::uint32_t start,
    std::uint32_t blocks,
    std::span<const std::uint8_t> data
  ) override {
    if (on_set_edid) {
      return on_set_edid(pad, start, blocks, data);
    }
    return std::unexpected(edid_error_t {error_category_e::not_supported, ENOTTY, ""});
  }

  edid_result_t<void> set_audio_enabled(bool enabled) override {
    if (on_set_audio_enabled) {
      return on_set_audio_enabled(enabled);
    }
    return ioctl_backend_t::set_audio_enabled(enabled);
  }

};

TEST(EdidNegotiatorTest, HardwareWithoutEdidSupport) {
  mock_ioctl_backend_t backend;
  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);

  resolution_t target {1920, 1080};
  std::vector<hdmi_mode_t> modes;

  negotiator.start_negotiation(target, modes);
  EXPECT_EQ(sm.state(), state_e::negotiating);

  // Timing stability
  for (int i = 0; i < 30; ++i) {
    if (negotiator.check_lock(resolution_t {3840, 2160})) {
      break;
    }
  }
  EXPECT_EQ(sm.state(), state_e::streaming_rga);  // Mismatch target and actual
}

TEST(EdidNegotiatorTest, EdidWriteReliesOnDriverHpdAndReachesDirectPathWhenObserved) {
  mock_ioctl_backend_t backend;
  const auto original = with_cta_lpcm_audio_extension(make_2160p_edid(), 16);
  std::vector<std::uint32_t> write_block_counts;
  std::vector<std::uint8_t> negotiated_edid;
  std::vector<bool> audio_states;

  backend.on_get_edid = [&original](auto, auto, auto blocks, auto buf) -> edid_result_t<std::uint32_t> {
    std::memcpy(buf.data(), original.data(), blocks * k_edid_block_size);
    return blocks;
  };
  backend.on_set_edid = [&write_block_counts, &negotiated_edid](auto, auto, auto blocks, auto data) -> edid_result_t<std::uint32_t> {
    write_block_counts.push_back(blocks);
    if (blocks == 2U) {
      negotiated_edid.assign(data.begin(), data.end());
    }
    return blocks;
  };
  backend.on_set_audio_enabled = [&audio_states](bool enabled) -> edid_result_t<void> {
    audio_states.push_back(enabled);
    return {};
  };

  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);

  resolution_t target {1920, 1080};
  std::vector<hdmi_mode_t> modes {{target, {60, 1}, true}};

  negotiator.start_negotiation(target, modes);
  EXPECT_EQ(sm.state(), state_e::negotiating);

  for (int i = 0; i < 30; ++i) {
    if (negotiator.check_lock(target)) {
      break;
    }
  }
  EXPECT_EQ(sm.state(), state_e::streaming_direct);
  ASSERT_FALSE(write_block_counts.empty());
  EXPECT_EQ(write_block_counts.front(), 2U);
  ASSERT_EQ(negotiated_edid.size(), k_edid_block_size * 2U);
  EXPECT_EQ(negotiated_edid[126], 1U);
  EXPECT_EQ(negotiated_edid[k_edid_block_size], 0x02U);
  EXPECT_EQ(negotiated_edid[k_edid_block_size + 3U], 0x41U);
  EXPECT_EQ(negotiated_edid[k_edid_block_size + 5U], 0x90U);
  EXPECT_EQ(negotiated_edid[k_edid_block_size + 7U], 0x09U);
  EXPECT_EQ(audio_states, (std::vector<bool> {true}));
}

TEST(EdidNegotiatorTest, MatchingLiveTimingSkipsEdidWrite) {
  mock_ioctl_backend_t backend;
  std::uint32_t reads = 0;
  std::uint32_t writes = 0;
  backend.on_get_edid = [&reads](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
    ++reads;
    return std::unexpected(edid_error_t {error_category_e::not_supported, ENOTTY, "unexpected EDID read"});
  };
  backend.on_set_edid = [&writes](auto, auto, auto blocks, auto) -> edid_result_t<std::uint32_t> {
    ++writes;
    return blocks;
  };

  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);
  const resolution_t target {3840, 2160};
  negotiator.start_negotiation(target, {{target, {60, 1}, true}}, target);

  EXPECT_EQ(sm.state(), state_e::streaming_direct);
  EXPECT_EQ(reads, 0U);
  EXPECT_EQ(writes, 0U);
  EXPECT_EQ(sm.last_reason(), "timing matches");
}

TEST(EdidNegotiatorTest, HigherLiveTimingNegotiatesRequestedModeAndReachesDirectPath) {
  mock_ioctl_backend_t backend;
  const auto original = with_cta_lpcm_audio_extension(make_2160p_edid(), 16);
  std::uint32_t writes = 0;
  backend.on_get_edid = [&original](auto, auto, auto blocks, auto buffer) -> edid_result_t<std::uint32_t> {
    std::memcpy(buffer.data(), original.data(), blocks * k_edid_block_size);
    return blocks;
  };
  backend.on_set_edid = [&writes](auto, auto, auto blocks, auto) -> edid_result_t<std::uint32_t> {
    ++writes;
    return blocks;
  };

  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);
  const resolution_t target {1920, 1080};
  const resolution_t live {3840, 2160};
  negotiator.start_negotiation(target, {{target, {60, 1}, true}}, live);

  EXPECT_EQ(sm.state(), state_e::negotiating);
  EXPECT_GT(writes, 0U);
  for (int i = 0; i < 30; ++i) {
    if (negotiator.check_lock(target)) {
      break;
    }
  }
  EXPECT_EQ(sm.state(), state_e::streaming_direct);
  EXPECT_EQ(sm.last_reason(), "timing matches");
}

TEST(EdidNegotiatorTest, WriteFailureRestoresSavedOriginal) {
  mock_ioctl_backend_t backend;
  const auto original = make_1080p_edid();
  backend.on_get_edid = [&original](auto, auto, std::uint32_t blocks, std::span<std::uint8_t> buffer)
    -> edid_result_t<std::uint32_t> {
    if (blocks != 1) {
      return std::unexpected(edid_error_t {error_category_e::invalid_argument, EINVAL, "unexpected block count"});
    }
    std::memcpy(buffer.data(), original.data(), k_edid_block_size);
    return 1U;
  };

  std::uint32_t writes = 0;
  backend.on_set_edid = [&writes](auto, auto, std::uint32_t blocks, auto) -> edid_result_t<std::uint32_t> {
    ++writes;
    // The first request is the new fixture and simulates a partial write;
    // the guarded restore request must still be attempted and checked.
    return writes == 1 ? blocks - 1U : blocks;
  };

  state_machine_t sm;
  ASSERT_TRUE(sm.enter_no_signal());
  session_negotiator_t negotiator(backend, sm, 0);
  const resolution_t target {1920, 1080};
  negotiator.start_negotiation(target, {{target, {60, 1}, true}});

  EXPECT_EQ(writes, 2U);
  EXPECT_EQ(sm.last_reason(), "EDID write failed; original restored");
}

TEST(EdidNegotiatorTest, SessionEndRestoresOriginalThroughDriverHpdCycle) {
  mock_ioctl_backend_t backend;
  const auto original = with_cta_lpcm_audio_extension(make_2160p_edid(), 16);
  std::uint32_t writes = 0;
  backend.on_get_edid = [&original](auto, auto, std::uint32_t blocks, std::span<std::uint8_t> buffer)
    -> edid_result_t<std::uint32_t> {
    if (blocks != 1U && blocks != 2U) {
      return std::unexpected(edid_error_t {error_category_e::invalid_argument, EINVAL, "unexpected block count"});
    }
    std::memcpy(buffer.data(), original.data(), blocks * k_edid_block_size);
    return blocks;
  };
  backend.on_set_edid = [&writes](auto, auto, std::uint32_t blocks, auto) -> edid_result_t<std::uint32_t> {
    ++writes;
    return blocks;
  };

  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);
  const resolution_t target {1920, 1080};
  negotiator.start_negotiation(target, {{target, {60, 1}, true}});
  ASSERT_EQ(writes, 1U);

  negotiator.end_session();
  EXPECT_EQ(writes, 2U);
  EXPECT_EQ(sm.state(), state_e::shutdown);
}

TEST(EdidNegotiatorTest, UnsupportedModeIsNotSilentlyWrittenAs1080p) {
  mock_ioctl_backend_t backend;
  std::uint32_t reads = 0;
  std::uint32_t writes = 0;
  backend.on_get_edid = [&reads](auto, auto, auto, auto) -> edid_result_t<std::uint32_t> {
    ++reads;
    return std::unexpected(edid_error_t {error_category_e::not_supported, ENOTTY, "unexpected EDID read"});
  };
  backend.on_set_edid = [&writes](auto, auto, std::uint32_t blocks, auto) -> edid_result_t<std::uint32_t> {
    ++writes;
    return blocks;
  };

  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);
  negotiator.start_negotiation({4096, 2160}, {{{4096, 2160}, {60, 1}, true}});

  EXPECT_EQ(sm.state(), state_e::negotiating);
  EXPECT_EQ(reads, 1U);
  EXPECT_EQ(writes, 0U);
}
