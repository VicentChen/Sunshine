/** @file tests/rkmpp_rkmpp_layout_test.cpp */
#include <cstdint>
#include <iostream>

#include <src/platform/linux/hdmirx.h>
#include <src/platform/linux/rkmpp.h>

// The layout and Annex-B helpers are tested without opening a V4L2 device.
// rkmpp.cpp also contains the production encoder methods, so provide the
// small HDMI RX interfaces those uncalled methods reference at link time.
namespace platf::hdmirx {
bool capture_format_is_valid(const capture_format_t &) noexcept {
  return true;
}

void captured_frame_t::release() {}
bool captured_frame_t::released() const noexcept {
  return true;
}
std::chrono::steady_clock::time_point captured_frame_t::timestamp() const noexcept {
  return {};
}
const std::vector<frame_plane_t> &captured_frame_t::planes() const noexcept {
  static const std::vector<frame_plane_t> empty;
  return empty;
}
}  // namespace platf::hdmirx

namespace {
platf::hdmirx::capture_format_t layout(MppFrameFormat format, std::uint32_t height, std::uint32_t rows) {
  platf::hdmirx::capture_format_t result;
  result.width = 1920;
  result.height = height;
  result.mpp_format = format;
  result.planes.push_back({5760, 5760 * rows, 5760 * rows});
  return result;
}
}  // namespace

int main() {
  const auto expect = [](bool condition, const char *name) {
    if (!condition) std::cerr << "rkmpp_rkmpp_layout_test=FAIL: " << name << '\n';
    return condition;
  };
  bool passed = true;
  passed = expect(platf::rkmpp::detail::derive_vertical_stride(layout(MPP_FMT_BGR888, 1080, 1080)) == 1080, "BGR vertical stride") && passed;
  passed = expect(platf::rkmpp::detail::derive_vertical_stride(layout(MPP_FMT_YUV420SP, 1080, 1620)) == 1080, "NV12 vertical stride") && passed;
  passed = expect(platf::rkmpp::detail::derive_vertical_stride(layout(MPP_FMT_YUV422SP, 1080, 2160)) == 1080, "NV16 vertical stride") && passed;
  passed = expect(platf::rkmpp::detail::derive_vertical_stride(layout(MPP_FMT_YUV444SP, 1080, 3240)) == 1080, "NV24 vertical stride") && passed;
  passed = expect(platf::rkmpp::detail::is_access_unit_complete(false, false), "non-partition packet completion") && passed;
  passed = expect(!platf::rkmpp::detail::is_access_unit_complete(true, false), "incomplete partition") && passed;
  passed = expect(platf::rkmpp::detail::is_access_unit_complete(true, true), "EOI partition completion") && passed;
  const auto h264 = platf::rkmpp::codec_e::h264;
  const auto h265 = platf::rkmpp::codec_e::h265;
  passed = expect(platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 7, 0, 0, 1, 8, 0, 0, 1, 5}, h264, true), "H264 PS before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 5, 0, 0, 1, 7, 0, 0, 1, 8}, h264, true), "H264 PS after IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 7, 0, 0, 1, 8, 0, 0, 1, 19, 0, 0, 1, 5}, h264, true), "H264 non-IDR VCL before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 7, 0, 0, 1, 5}, h264, true), "H264 missing PPS") && passed;
  passed = expect(platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 64, 0, 0, 1, 66, 0, 0, 1, 68, 0, 0, 1, 38}, h265, true), "H265 PS before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 38, 0, 0, 1, 64, 0, 0, 1, 66, 0, 0, 1, 68}, h265, true), "H265 PS after IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 64, 0, 0, 1, 66, 0, 0, 1, 68, 0, 0, 1, 2, 0, 0, 1, 38}, h265, true), "H265 non-IDR VCL before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0, 0, 1, 64, 0, 0, 1, 66, 0, 0, 1, 38}, h265, true), "H265 missing PPS") && passed;
  if (!passed) return 1;
  std::cout << "rkmpp_rkmpp_layout_test=PASS\n";
  return 0;
}
