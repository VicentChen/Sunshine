/** @file src/platform/linux/rkmpp.h */
#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <vector>

namespace platf::hdmirx { class captured_frame_t; struct capture_format_t; }

namespace platf::rkmpp {
  enum class codec_e { h264, h265 };
  namespace detail {
    /** Derive MPP's vertical stride from a single-plane V4L2 allocation. */
    std::uint32_t derive_vertical_stride(const hdmirx::capture_format_t &format);
    /** A non-partition packet is complete; partitions end only at EOI. */
    bool is_access_unit_complete(bool partition, bool eoi) noexcept;
    /**
     * Accept only when the first VCL NAL is an IDR and, if requested, every
     * required parameter set precedes that IDR in the same Annex-B sequence.
     */
    bool annexb_first_vcl_is_idr(const std::vector<std::uint8_t> &bytes, codec_e codec, bool require_parameter_sets) noexcept;
    bool annexb_first_vcl_is_idr(const std::uint8_t *bytes, std::size_t size, codec_e codec, bool require_parameter_sets) noexcept;
  }
  struct encoder_stats_t {
    std::uint64_t frames {};
    std::uint64_t packets {};
    std::uint64_t bytes {};
    std::uint32_t min_packet_bytes {};
    std::uint32_t max_packet_bytes {};
  };
  struct encoder_config_t {
    codec_e codec {};
    const hdmirx::capture_format_t *input_format {};
    std::uint32_t fps_num {60};
    std::uint32_t fps_den {1};
    std::uint32_t bitrate {12'000'000};
    std::uint32_t gop {60};
  };
  /**
   * Owns the MPP output allocation until Sunshine's network consumer drops the
   * corresponding video packet. It is deliberately move-only: MppPacket and
   * its backing MppBuffer have one unambiguous release point.
   */
  class encoded_packet_t {
  public:
    encoded_packet_t();
    encoded_packet_t(const encoded_packet_t &) = delete;
    encoded_packet_t &operator=(const encoded_packet_t &) = delete;
    encoded_packet_t(encoded_packet_t &&) noexcept;
    encoded_packet_t &operator=(encoded_packet_t &&) noexcept;
    ~encoded_packet_t();

    std::uint8_t *data() const noexcept;
    std::size_t size() const noexcept;
    bool output_intra() const noexcept;
    explicit operator bool() const noexcept;

  private:
    struct state_t;
    explicit encoded_packet_t(std::unique_ptr<state_t> state) noexcept;
    std::unique_ptr<state_t> state_;
    friend class encoder_t;
  };
  class encoder_t {
  public:
    static encoder_t create(const encoder_config_t &config);
    encoder_t(); encoder_t(const encoder_t &) = delete; encoder_t &operator=(const encoder_t &) = delete;
    encoder_t(encoder_t &&) noexcept; encoder_t &operator=(encoder_t &&) noexcept; ~encoder_t();
    void encode(hdmirx::captured_frame_t &frame, std::ostream &output);
    std::vector<std::uint8_t> encode_to_vector(hdmirx::captured_frame_t &frame);
    void request_idr();
    std::uint64_t encoded_frames() const noexcept;
    encoder_stats_t stats() const noexcept;
    encoded_packet_t encode_packet(hdmirx::captured_frame_t &frame);
  private:
    struct state_t; explicit encoder_t(std::unique_ptr<state_t> state) noexcept; std::unique_ptr<state_t> state_;
  };
  bool is_compiled() noexcept;
}  // namespace platf::rkmpp
