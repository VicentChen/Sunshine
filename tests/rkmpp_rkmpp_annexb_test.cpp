#include <iostream>
#include <src/platform/linux/rkmpp.h>
int main() {
  const auto expect = [](bool condition, const char *name) { if (!condition) std::cerr << "rkmpp_rkmpp_annexb_test=FAIL: " << name << '\n'; return condition; };
  const auto h264 = platf::rkmpp::codec_e::h264;
  const auto h265 = platf::rkmpp::codec_e::h265;
  bool passed = true;
  passed = expect(platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,7,0,0,1,8,0,0,1,5}, h264, true), "H264 PS before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,5,0,0,1,7,0,0,1,8}, h264, true), "H264 PS after IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,7,0,0,1,8,0,0,1,19,0,0,1,5}, h264, true), "H264 non-IDR VCL before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,7,0,0,1,5}, h264, true), "H264 missing PPS") && passed;
  passed = expect(platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,64,0,0,1,66,0,0,1,68,0,0,1,38}, h265, true), "H265 PS before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,38,0,0,1,64,0,0,1,66,0,0,1,68}, h265, true), "H265 PS after IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,64,0,0,1,66,0,0,1,68,0,0,1,2,0,0,1,38}, h265, true), "H265 non-IDR VCL before IDR") && passed;
  passed = expect(!platf::rkmpp::detail::annexb_first_vcl_is_idr({0,0,1,64,0,0,1,66,0,0,1,38}, h265, true), "H265 missing PPS") && passed;
  if (!passed) return 1;
  std::cout << "rkmpp_rkmpp_annexb_test=PASS\n";
  return 0;
}
