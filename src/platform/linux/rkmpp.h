/** @file src/platform/linux/rkmpp.h */
#pragma once

#include "src/frame_profile.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mpp_frame.h>
#include <optional>
#include <ostream>
#include <vector>

namespace platf::rkmpp {
  /** @brief Video codec emitted by the RKMPP encoder. */
  enum class codec_e {
    h264,
    h265
  };

  /**
   * @brief Describes the pixels MPP receives through one DMA-BUF.
   *
   * This is input-side metadata.  It deliberately does not describe the
   * requested coded output size, which belongs to `encoder_config_t`.
   */
  struct input_layout_t {
    std::uint32_t visible_width {};  ///< Visible input width in pixels.
    std::uint32_t visible_height {};  ///< Visible input height in pixels.
    std::uint32_t horizontal_stride {};  ///< Input row stride in bytes.
    std::uint32_t vertical_stride {};  ///< Allocated luma rows represented by the DMA-BUF.
    MppFrameFormat format {};  ///< MPP pixel format of the imported DMA-BUF.

    friend constexpr bool operator==(const input_layout_t &, const input_layout_t &) = default;
  };

  /**
   * @brief Owns the lifetime pin for a DMA-BUF frame submitted to MPP.
   *
   * The type is intentionally opaque: an HDMI RX capture frame, an RGA pool
   * lease, or another producer may provide it.  `encoder_t::encode_packet()`
   * copies this holder before it submits the frame and drops its copy only
   * after synchronous MPP consumption completes.  The caller may therefore
   * return or recycle the producer object immediately after that method
   * returns, but never before.
   */
  using input_holder_t = std::shared_ptr<void>;

  /**
   * @brief Stable identity for a reusable producer DMA-BUF import.
   *
   * A generation changes whenever the producer's allocation set is rebuilt;
   * the index identifies one allocation only within that generation. File
   * descriptor values are deliberately excluded because the kernel may reuse
   * them after a source recovery.
   */
  struct input_buffer_key_t {
    std::uint64_t generation {};  ///< Producer allocation generation.
    std::uint32_t index {};  ///< Buffer index within the allocation generation.

    friend constexpr bool operator==(const input_buffer_key_t &, const input_buffer_key_t &) = default;
  };

  /**
   * @brief A single DMA-BUF input frame submitted to RKMPP.
   *
   * `dma_buf_fd` is borrowed and is never closed by RKMPP.  `allocation_size`
   * is the full allocation, not only populated bytes.  `holder` owns the
   * producer lease that keeps the descriptor and pixels valid through the
   * synchronous encode operation.
   */
  struct input_frame_t {
    input_layout_t layout;  ///< Actual producer layout, which must match encoder input layout.
    int dma_buf_fd {-1};  ///< Borrowed DMA-BUF file descriptor.
    std::uint64_t allocation_size {};  ///< Complete DMA-BUF allocation in bytes.
    std::int64_t pts {};  ///< Producer presentation timestamp passed to MPP unchanged.
    input_holder_t holder;  ///< Lifetime pin for the borrowed DMA-BUF.
    video::frame_profile_t *profile {};  ///< Borrowed profile record updated during synchronous encoding.
    std::optional<input_buffer_key_t> cache_key;  ///< Stable import-cache key, or empty to import only for this frame.

    /**
     * @brief Release the producer lease and restore an empty input frame.
     *
     * Use this when a prepared frame is replaced without being submitted to
     * MPP, as can happen to the initial synchronized-capture placeholder.
     */
    void reset() noexcept;
  };

  /** @brief Result of validating a generic RKMPP input layout or frame. */
  enum class input_status_e {
    ok,  ///< The input meets RKMPP's direct-DMA-BUF contract.
    invalid_visible_size,  ///< Visible dimensions are outside the stage-1 policy limits.
    unsupported_format,  ///< The MPP format has no supported single-buffer layout.
    stride_too_small,  ///< Horizontal or vertical stride cannot cover visible pixels.
    allocation_too_small,  ///< DMA-BUF allocation cannot cover the declared layout.
    invalid_dma_buf,  ///< The borrowed descriptor is invalid.
    missing_holder,  ///< No producer lifetime pin was supplied.
    layout_mismatch,  ///< Frame layout differs from the encoder's configured input layout.
    allocation_not_representable,  ///< Allocation cannot be passed losslessly to the local MPP ABI.
  };

  /** @brief Result of validating RKMPP encoder configuration. */
  enum class encoder_config_status_e {
    ok,  ///< The encoder can be created without conversion.
    invalid_input,  ///< The configured input layout is invalid.
    invalid_coded_size,  ///< The requested coded output size is invalid.
    converter_required,  ///< Input and coded dimensions differ; stage 4 must supply a converted frame.
    invalid_rate_control,  ///< Frame rate, bitrate, or GOP is invalid.
    invalid_codec,  ///< Codec enum is not H.264 or H.265.
  };

  namespace detail {
    /**
     * @brief Return the minimum allocation for an input layout, or zero when invalid.
     *
     * @param layout Layout whose format-specific plane extent is calculated.
     * @return Required allocation in bytes, or zero when it is unrepresentable.
     */
    std::uint64_t minimum_allocation_size(const input_layout_t &layout) noexcept;
    /** A non-partition packet is complete; partitions end only at EOI. */
    bool is_access_unit_complete(bool partition, bool eoi) noexcept;
    /**
     * Accept only when the first VCL NAL is an IDR and, if requested, every
     * required parameter set precedes that IDR in the same Annex-B sequence.
     */
    bool annexb_first_vcl_is_idr(const std::vector<std::uint8_t> &bytes, codec_e codec, bool require_parameter_sets) noexcept;
    bool annexb_first_vcl_is_idr(const std::uint8_t *bytes, std::size_t size, codec_e codec, bool require_parameter_sets) noexcept;
    /**
     * @brief Confirm an MPP intra-packet indication with its Annex-B payload.
     *
     * Non-intra packets return false without scanning their bitstream. An
     * intra indication is still verified so malformed metadata cannot mark a
     * non-IDR packet as an IDR frame.
     *
     * @param output_intra MPP's `KEY_OUTPUT_INTRA` value for the packet.
     * @param bytes Complete Annex-B access unit.
     * @param codec Codec used to parse the access unit.
     * @return True only when MPP marked the packet intra and the first VCL NAL is an IDR.
     */
    bool output_is_idr(bool output_intra, const std::uint8_t *bytes, std::size_t size, codec_e codec) noexcept;
  }  // namespace detail

  /**
   * @brief Derive a generic layout from one producer's single-plane extent.
   *
   * The helper validates format-specific plane ratios and chroma alignment
   * without knowing whether the producer is HDMI RX, RGA, or another DMA-BUF
   * source. `plane_extent` is the populated format extent, normally V4L2
   * `sizeimage`, rather than the complete allocation.
   *
   * @param visible_width Visible width in pixels.
   * @param visible_height Visible height in pixels.
   * @param format MPP format of the producer plane.
   * @param horizontal_stride Producer byte stride.
   * @param plane_extent Bytes occupied by all planes in the single DMA-BUF.
   * @return Layout with derived vertical stride, or `std::nullopt` when invalid.
   */
  std::optional<input_layout_t> make_input_layout_from_plane(std::uint32_t visible_width, std::uint32_t visible_height, MppFrameFormat format, std::uint32_t horizontal_stride, std::uint64_t plane_extent) noexcept;
  /**
   * @brief Validate generic input layout metadata before creating an encoder.
   *
   * @param layout Input layout to validate.
   * @return Status explaining whether MPP can import this single DMA-BUF layout.
   */
  input_status_e validate_input_layout(const input_layout_t &layout) noexcept;
  /**
   * @brief Validate a producer frame against the layout configured for an encoder.
   *
   * @param frame Producer frame with DMA-BUF lifetime pin.
   * @param expected_layout Layout configured when the encoder was created.
   * @return Status explaining whether MPP may consume the frame.
   */
  input_status_e validate_input_frame(const input_frame_t &frame, const input_layout_t &expected_layout) noexcept;
  /**
   * @brief Validate that direct input can produce the requested coded size in this stage.
   *
   * @param config Encoder configuration to validate.
   * @return `converter_required` when input I and coded T differ.
   */
  encoder_config_status_e validate_encoder_config(const struct encoder_config_t &config) noexcept;

  /**
   * @brief Cumulative statistics for one RKMPP encoder instance.
   */
  struct encoder_stats_t {
    std::uint64_t frames {};  ///< Number of input frames consumed by MPP.
    std::uint64_t packets {};  ///< Number of complete coded access units produced.
    std::uint64_t bytes {};  ///< Total coded bytes retained by output packets.
    std::uint32_t min_packet_bytes {};  ///< Smallest complete coded packet, or zero before output.
    std::uint32_t max_packet_bytes {};  ///< Largest complete coded packet, or zero before output.
  };

  /**
   * @brief Immutable RKMPP encoder setup for one stream.
   *
   * Input layout I and coded output T are separate fields. Until stage 4
   * connects RGA, their dimensions must be equal for direct input.
   */
  struct encoder_config_t {
    codec_e codec {};  ///< Requested H.264 or H.265 output codec.
    input_layout_t input_layout;  ///< Exact direct or converted input layout accepted by MPP.
    std::uint32_t coded_width {};  ///< Moonlight-requested coded output width T.
    std::uint32_t coded_height {};  ///< Moonlight-requested coded output height T.
    std::uint32_t fps_num {60};  ///< Input and output frame-rate numerator.
    std::uint32_t fps_den {1};  ///< Input and output frame-rate denominator.
    std::uint32_t bitrate {12'000'000};  ///< Target CBR bitrate in bits per second.
    std::uint32_t gop {60};  ///< GOP length in frames.
    bool low_delay {false};  ///< Request MPP low-delay mode; disabled by the baseline configuration.
    bool disable_reencode {false};  ///< Limit MPP rate control to zero re-encode attempts; disabled by the baseline configuration.
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
    /**
     * @brief Create an encoder using a validated direct or converted layout.
     *
     * @param config Input layout, coded size, and rate-control configuration.
     * @return Newly initialized RKMPP encoder.
     * @throws std::runtime_error When a converter is required in this stage.
     */
    static encoder_t create(const encoder_config_t &config);
    encoder_t();
    encoder_t(const encoder_t &) = delete;
    encoder_t &operator=(const encoder_t &) = delete;
    encoder_t(encoder_t &&) noexcept;
    encoder_t &operator=(encoder_t &&) noexcept;
    ~encoder_t();
    /**
     * @brief Synchronously encode one pinned DMA-BUF frame into a stream.
     *
     * @param frame Input frame whose holder remains pinned through MPP consumption.
     * @param output Destination stream that receives a temporary byte copy.
     */
    void encode(const input_frame_t &frame, std::ostream &output);
    /**
     * @brief Synchronously encode one pinned DMA-BUF frame into a byte vector.
     *
     * @param frame Input frame whose holder remains pinned through MPP consumption.
     * @return Encoded bytes copied from the zero-copy MPP output packet.
     */
    std::vector<std::uint8_t> encode_to_vector(const input_frame_t &frame);
    void request_idr();
    /**
     * @brief Release every cached producer DMA-BUF import.
     *
     * Call before a producer allocation generation is retired. In-flight work
     * is not supported because this encoder uses the synchronous MPP API.
     */
    void clear_input_cache() noexcept;
    std::uint64_t encoded_frames() const noexcept;
    encoder_stats_t stats() const noexcept;
    /**
     * @brief Synchronously encode one pinned DMA-BUF without copying its packet payload.
     *
     * @param frame Input frame whose holder remains pinned through MPP consumption.
     * @return Move-only packet that keeps MPP-owned coded bytes alive for networking.
     */
    encoded_packet_t encode_packet(const input_frame_t &frame);

  private:
    struct state_t;
    explicit encoder_t(std::unique_ptr<state_t> state) noexcept;
    std::unique_ptr<state_t> state_;
  };

  bool is_compiled() noexcept;
}  // namespace platf::rkmpp
