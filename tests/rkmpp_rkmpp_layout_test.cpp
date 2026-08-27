/** @file tests/rkmpp_rkmpp_layout_test.cpp */
#include <cstdint>
#include <iostream>

#include <src/platform/linux/rkmpp.h>

namespace {
platf::rkmpp::input_layout_t layout(MppFrameFormat format, std::uint32_t height, std::uint32_t rows) {
  return {1920, height, format == MPP_FMT_BGR888 ? 5760U : 1920U, rows, format};
}
}  // namespace

int main() {
  const auto expect = [](bool condition, const char *name) {
    if (!condition) std::cerr << "rkmpp_rkmpp_layout_test=FAIL: " << name << '\n';
    return condition;
  };
  bool passed = true;
  passed = expect(platf::rkmpp::detail::minimum_allocation_size(layout(MPP_FMT_BGR888, 1080, 1080)) == 5'760U * 1'080U, "BGR allocation") && passed;
  passed = expect(platf::rkmpp::detail::minimum_allocation_size(layout(MPP_FMT_YUV420SP, 1080, 1080)) == 1'920U * 1'620U, "NV12 allocation") && passed;
  passed = expect(platf::rkmpp::detail::minimum_allocation_size(layout(MPP_FMT_YUV422SP, 1080, 1080)) == 1'920U * 2'160U, "NV16 allocation") && passed;
  passed = expect(platf::rkmpp::detail::minimum_allocation_size(layout(MPP_FMT_YUV444SP, 1080, 1080)) == 1'920U * 3'240U, "NV24 allocation") && passed;
  passed = expect(platf::rkmpp::validate_input_layout(layout(MPP_FMT_YUV420SP, 1080, 1080)) == platf::rkmpp::input_status_e::ok, "NV12 layout validation") && passed;
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
