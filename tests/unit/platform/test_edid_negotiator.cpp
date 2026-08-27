#include <gtest/gtest.h>
#include "src/platform/linux/input_state_machine.h"
#include "src/platform/linux/edid.h"
#include "src/platform/linux/input_state_machine.h"
#include <cstring>
#include <optional>

using namespace platf::hdmirx;
using namespace platf::edid;
using namespace platf::input_sm;

class mock_ioctl_backend_t final: public ioctl_backend_t {
public:
  using get_edid_fn = std::function<edid_result_t<std::uint32_t>(
    std::uint32_t, std::uint32_t, std::uint32_t, std::span<std::uint8_t>)>;
  using set_edid_fn = std::function<edid_result_t<std::uint32_t>(
    std::uint32_t, std::uint32_t, std::uint32_t, std::span<const std::uint8_t>)>;

  get_edid_fn on_get_edid;
  set_edid_fn on_set_edid;

  edid_result_t<std::uint32_t> get_edid(
    std::uint32_t pad, std::uint32_t start, std::uint32_t blocks, std::span<std::uint8_t> buffer) override {
    if (on_get_edid) return on_get_edid(pad, start, blocks, buffer);
    return std::unexpected(edid_error_t{error_category_e::not_supported, ENOTTY, ""});
  }
  edid_result_t<std::uint32_t> set_edid(
    std::uint32_t pad, std::uint32_t start, std::uint32_t blocks, std::span<const std::uint8_t> data) override {
    if (on_set_edid) return on_set_edid(pad, start, blocks, data);
    return std::unexpected(edid_error_t{error_category_e::not_supported, ENOTTY, ""});
  }
};

TEST(EdidNegotiatorTest, HardwareWithoutEdidSupport) {
  mock_ioctl_backend_t backend;
  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);

  resolution_t target{1920, 1080};
  std::vector<hdmi_mode_t> modes;

  negotiator.start_negotiation(target, modes);
  EXPECT_EQ(sm.state(), state_e::negotiating);

  // Timing stability
  for (int i=0; i<30; ++i) {
      if (negotiator.check_lock(resolution_t{3840, 2160})) break;
  }
  EXPECT_EQ(sm.state(), state_e::streaming_rga); // Mismatch target and actual
}

TEST(EdidNegotiatorTest, UpstreamObeysDirectPath) {
  mock_ioctl_backend_t backend;

  backend.on_get_edid = [](auto, auto, auto blocks, auto buf) -> edid_result_t<std::uint32_t> {
      std::memset(buf.data(), 0, blocks * 128);
      return blocks;
  };
  backend.on_set_edid = [](auto, auto, auto blocks, auto) -> edid_result_t<std::uint32_t> {
      return blocks;
  };

  state_machine_t sm;
  session_negotiator_t negotiator(backend, sm, 0);

  resolution_t target{1920, 1080};
  std::vector<hdmi_mode_t> modes{{target, {60, 1}, true}};

  negotiator.start_negotiation(target, modes);
  EXPECT_EQ(sm.state(), state_e::negotiating);

  for (int i=0; i<30; ++i) {
      if (negotiator.check_lock(target)) break;
  }
  EXPECT_EQ(sm.state(), state_e::streaming_direct);
}

TEST(EdidNegotiatorTest, WriteFailureRestoresSavedOriginal) {
  mock_ioctl_backend_t backend;
  const auto original = make_1080p_edid();
  backend.on_get_edid = [&original](auto, auto, std::uint32_t blocks, std::span<std::uint8_t> buffer)
      -> edid_result_t<std::uint32_t> {
    if (blocks != 1) {
      return std::unexpected(edid_error_t{error_category_e::invalid_argument, EINVAL, "unexpected block count"});
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
  const resolution_t target{1920, 1080};
  negotiator.start_negotiation(target, {{target, {60, 1}, true}});

  EXPECT_EQ(writes, 2U);
  EXPECT_EQ(sm.last_reason(), "EDID write failed; original restored");
}
