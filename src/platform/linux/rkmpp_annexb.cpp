#include "src/platform/linux/rkmpp.h"

namespace platf::rkmpp::detail {
  bool annexb_first_vcl_is_idr(const std::vector<std::uint8_t> &bytes, codec_e codec, bool require_parameter_sets) noexcept {
    return annexb_first_vcl_is_idr(bytes.data(), bytes.size(), codec, require_parameter_sets);
  }

  bool annexb_first_vcl_is_idr(const std::uint8_t *bytes, std::size_t size, codec_e codec, bool require_parameter_sets) noexcept {
    bool vps = false, sps = false, pps = false;
    for (std::size_t i = 0; i + 3 < size;) {
      std::size_t prefix = 0;
      if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
        prefix = 3;
      } else if (i + 4 < size && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
        prefix = 4;
      }
      if (!prefix) {
        ++i;
        continue;
      }
      const auto header = i + prefix;
      if (header >= size) {
        break;
      }
      if (codec == codec_e::h264) {
        switch (bytes[header] & 0x1fU) {
          case 5:
            return !require_parameter_sets || (sps && pps);
          case 1:
          case 2:
          case 3:
          case 4:
          case 19:
          case 20:
          case 21:
            return false;
          case 7:
            sps = true;
            break;
          case 8:
            pps = true;
            break;
          default:
            break;
        }
      } else {
        const auto type = (bytes[header] >> 1U) & 0x3fU;
        if (type == 19 || type == 20) {
          return !require_parameter_sets || (vps && sps && pps);
        }
        if (type <= 31) {
          return false;
        }
        if (type == 32) {
          vps = true;
        } else if (type == 33) {
          sps = true;
        } else if (type == 34) {
          pps = true;
        }
      }
      i = header + 1;
    }
    return false;
  }

  bool output_is_idr(bool output_intra, const std::uint8_t *bytes, std::size_t size, codec_e codec) noexcept {
    return output_intra && annexb_first_vcl_is_idr(bytes, size, codec, false);
  }
}  // namespace platf::rkmpp::detail
