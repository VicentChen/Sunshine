/**
 * @file src/platform/linux/edid.h
 * @brief EDID parsing, native-mode projection, and V4L2 ioctl abstraction.
 *
 * Every production EDID written by Sunshine is projected from the receiver's
 * validated native bytes. Fixed synthetic resolution fixtures intentionally do
 * not exist in this interface.
 */
#pragma once

#include <cerrno>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <src/platform/hdmirx_policy.h>
#include <string>
#include <vector>

namespace platf::edid {

  /// @brief Size of one EDID block in bytes.
  inline constexpr std::uint32_t k_edid_block_size = 128;
  /// @brief Maximum EDID blocks supported by the Rockchip receiver path.
  inline constexpr std::uint32_t k_max_edid_blocks = 4;
  /// @brief Maximum EDID byte count supported by the receiver path.
  inline constexpr std::uint32_t k_max_edid_size = k_edid_block_size * k_max_edid_blocks;

  /**
   * @brief Classified EDID ioctl error categories.
   */
  enum class error_category_e {
    not_supported,  ///< The device does not support the ioctl.
    invalid_argument,  ///< The ioctl arguments or EDID length are invalid.
    no_data,  ///< The device currently exposes no EDID.
    too_large,  ///< The EDID exceeds the supported block count.
    permission_denied,  ///< The process lacks permission.
    device_gone,  ///< The receiver disappeared.
    io_error,  ///< An unclassified I/O failure occurred.
  };

  /**
   * @brief Classify a raw errno value.
   *
   * @param err_no errno value returned by an ioctl.
   * @return Stable error category.
   */
  error_category_e classify_errno(int err_no) noexcept;

  /**
   * @brief Return a diagnostic name for an EDID error category.
   *
   * @param category Category to name.
   * @return Static category name.
   */
  const char *error_category_name(error_category_e category) noexcept;

  /**
   * @brief Details for a failed EDID operation.
   */
  struct edid_error_t {
    error_category_e category;  ///< Stable error category.
    int raw_errno;  ///< Original errno value.
    std::string message;  ///< Human-readable details.
  };

  /**
   * @brief Result type used by EDID device operations.
   *
   * @tparam T Successful result type.
   */
  template<typename T>
  using edid_result_t = std::expected<T, edid_error_t>;

  /**
   * @brief Mockable V4L2 EDID and HDMI-audio backend.
   */
  class ioctl_backend_t {
  public:
    /// @brief Destroy the backend through its interface.
    virtual ~ioctl_backend_t() = default;

    /**
     * @brief Read EDID blocks.
     *
     * @param pad V4L2 pad or input index.
     * @param start_block First block to read.
     * @param block_count Number of blocks to read.
     * @param buffer Destination storage.
     * @return Number of blocks read, or an error.
     */
    virtual edid_result_t<std::uint32_t> get_edid(
      std::uint32_t pad,
      std::uint32_t start_block,
      std::uint32_t block_count,
      std::span<std::uint8_t> buffer
    ) = 0;

    /**
     * @brief Write EDID blocks.
     *
     * @param pad V4L2 pad or input index.
     * @param start_block First block to write.
     * @param block_count Number of blocks to write.
     * @param data Source EDID bytes.
     * @return Number of blocks written, or an error.
     */
    virtual edid_result_t<std::uint32_t> set_edid(
      std::uint32_t pad,
      std::uint32_t start_block,
      std::uint32_t block_count,
      std::span<const std::uint8_t> data
    ) = 0;

    /**
     * @brief Enable or disable the optional Rockchip HDMI-audio domain.
     *
     * @param enabled true to enable audio detection and capture.
     * @return Success, or not_supported for devices without this control.
     */
    virtual edid_result_t<void> set_audio_enabled(bool enabled) {
      (void) enabled;
      return std::unexpected(edid_error_t {error_category_e::not_supported, ENOTTY, "HDMI audio control is not supported"});
    }
  };

  /**
   * @brief Parsed EDID detailed timing fields needed for mode selection.
   */
  struct timing_descriptor_t {
    std::uint32_t pixel_clock_khz {};  ///< Pixel clock in kHz.
    std::uint16_t h_active {};  ///< Horizontal active pixels.
    std::uint16_t h_blanking {};  ///< Horizontal blanking pixels.
    std::uint16_t v_active {};  ///< Vertical active lines.
    std::uint16_t v_blanking {};  ///< Vertical blanking lines.
    bool interlaced {};  ///< Whether the descriptor is interlaced.
  };

  /**
   * @brief Native EDID encoding that advertised a mode.
   */
  enum class mode_origin_e {
    established_timing,  ///< Base-block established timing bit.
    standard_timing,  ///< Base-block standard timing pair.
    base_dtd,  ///< Base-block detailed timing descriptor.
    cta_dtd,  ///< CTA detailed timing descriptor.
    cta_vdb,  ///< CTA short video descriptor.
    cta_y420_vdb,  ///< CTA YCbCr 4:2:0-only short video descriptor.
  };

  /**
   * @brief One native mode together with its precise EDID provenance.
   */
  struct mode_record_t {
    platf::hdmirx::hdmi_mode_t mode;  ///< Mode used by hardware-independent selection.
    mode_origin_e origin {};  ///< Encoding source.
    std::uint32_t block_index {};  ///< Containing EDID block.
    std::uint32_t byte_offset {};  ///< Offset inside the containing block.
    bool native {};  ///< Native or preferred marker.
    bool y420_only {};  ///< Mode is advertised through the Y420-only block.
    bool interlaced {};  ///< Mode uses interlaced scanning.
    std::vector<std::uint8_t> raw_encoding;  ///< Original descriptor bytes.
  };

  /**
   * @brief Compute an EDID block checksum.
   *
   * @param block Exactly one EDID block.
   * @return Byte sum; zero denotes a valid checksum.
   */
  std::uint8_t compute_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept;

  /**
   * @brief Validate one EDID block checksum.
   *
   * @param block Exactly one EDID block.
   * @return true when the byte sum is zero.
   */
  bool validate_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept;

  /**
   * @brief Validate the size, declared extension count, and every checksum.
   *
   * @param edid_data Complete EDID data.
   * @return true only for an exact, fully valid EDID.
   */
  bool validate_edid_checksums(std::span<const std::uint8_t> edid_data) noexcept;

  /**
   * @brief Parse one 18-byte detailed timing descriptor.
   *
   * @param descriptor Native descriptor bytes.
   * @return Parsed timing, or nullopt for a monitor descriptor.
   */
  std::optional<timing_descriptor_t> parse_timing_descriptor(std::span<const std::uint8_t, 18> descriptor) noexcept;

  /**
   * @brief Parse detailed timings from the four base-block descriptor slots.
   *
   * @param base_block Valid base EDID block.
   * @return Parsed timing descriptors.
   */
  std::vector<timing_descriptor_t> parse_base_block_timings(std::span<const std::uint8_t, k_edid_block_size> base_block) noexcept;

  /**
   * @brief Convert a detailed timing into the mode-selection representation.
   *
   * @param timing Parsed detailed timing.
   * @return Verified HDMI mode, or nullopt for invalid totals.
   */
  std::optional<platf::hdmirx::hdmi_mode_t> timing_to_hdmi_mode(const timing_descriptor_t &timing) noexcept;

  /**
   * @brief Parse all recognized native video encodings with provenance.
   *
   * @param edid_data Complete native EDID data.
   * @return Ordered catalog, or an empty vector for invalid EDID.
   */
  std::vector<mode_record_t> parse_mode_catalog(std::span<const std::uint8_t> edid_data) noexcept;

  /**
   * @brief Parse a deduplicated mode list for selection policy.
   *
   * @param edid_data Complete native EDID data.
   * @return Verified, deduplicated modes.
   */
  std::vector<platf::hdmirx::hdmi_mode_t> parse_edid_modes(std::span<const std::uint8_t> edid_data) noexcept;

  /**
   * @brief Build a source-compatible EDID profile capped at one native mode.
   *
   * The selected mode is promoted ahead of other native encodings. Lower and
   * equal resolutions remain available as compatibility fallbacks, while any
   * mode exceeding the selected dimensions is removed. Video timings are
   * copied from the validated receiver EDID; this function never synthesizes
   * timings. Non-video CTA capabilities remain unchanged. Unknown extension
   * formats are rejected because their video contents cannot be filtered
   * safely.
   *
   * @param source_edid Complete native EDID.
   * @param target Verified native mode selected for the Moonlight request.
   * @return Valid target EDID, or an empty vector when projection is unsafe.
   */
  std::vector<std::uint8_t> project_edid_for_mode(
    std::span<const std::uint8_t> source_edid,
    const platf::hdmirx::hdmi_mode_t &target
  ) noexcept;

  /**
   * @brief Read the complete EDID declared by a receiver.
   *
   * @param backend Device backend.
   * @param pad V4L2 pad or input index.
   * @return Exact complete EDID, or an error.
   */
  edid_result_t<std::vector<std::uint8_t>> read_edid(ioctl_backend_t &backend, std::uint32_t pad = 0);

  /**
   * @brief Write one complete validated-size EDID transaction.
   *
   * @param backend Device backend.
   * @param pad V4L2 pad or input index.
   * @param data Complete EDID bytes.
   * @return Number of blocks written, or an error.
   */
  edid_result_t<std::uint32_t> write_edid(
    ioctl_backend_t &backend,
    std::uint32_t pad,
    std::span<const std::uint8_t> data
  );

}  // namespace platf::edid
