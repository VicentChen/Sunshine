/**
 * @file src/platform/linux/edid.h
 * @brief EDID data model, parser, ioctl abstraction, and RAII restore guard.
 *
 * This header provides a testable, mock-friendly abstraction over V4L2
 * EDID read/write ioctls (VIDIOC_G_EDID / VIDIOC_S_EDID) for both video
 * nodes and sub-device nodes.  It also provides minimal EDID parsing for
 * extracting display modes, checksum validation, and a session restore guard.
 *
 * Design principles:
 * - ioctl details do not leak into HDMI capture/session state machines.
 * - All write paths have an original EDID restore strategy.
 * - Pure unit tests require no root and no /dev/video0 access.
 */
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <src/platform/hdmirx_policy.h>

namespace platf::edid {

  /// @brief Size of a single EDID block in bytes.
  inline constexpr std::uint32_t k_edid_block_size = 128;

  /// @brief Maximum number of EDID blocks supported (base + 3 extensions).
  inline constexpr std::uint32_t k_max_edid_blocks = 4;

  /// @brief Maximum total EDID data size in bytes.
  inline constexpr std::uint32_t k_max_edid_size = k_edid_block_size * k_max_edid_blocks;

  // -------------------------------------------------------------------------
  // Error classification
  // -------------------------------------------------------------------------

  /**
   * @brief Classified EDID ioctl error categories.
   *
   * Each category maps to specific errno values returned by the kernel
   * EDID ioctl calls, enabling policy decisions without errno leakage.
   */
  enum class error_category_e {
    not_supported,      ///< ENOTTY: device does not support EDID ioctls.
    invalid_argument,   ///< EINVAL: bad pad/input, block count, or offset.
    no_data,            ///< ENODATA: no EDID data is currently available.
    too_large,          ///< E2BIG: requested block count exceeds device limit.
    permission_denied,  ///< EACCES/EPERM: insufficient permissions.
    device_gone,        ///< ENODEV/ENXIO: device has disappeared.
    io_error,           ///< EIO/other: unclassified I/O error.
  };

  /**
   * @brief Classify a raw errno value into an EDID error category.
   *
   * @param err_no The errno value from a failed ioctl call.
   * @return Classified error category.
   */
  error_category_e classify_errno(int err_no) noexcept;

  /**
   * @brief Human-readable name for an error category.
   *
   * @param cat Error category to name.
   * @return Null-terminated string literal.
   */
  const char *error_category_name(error_category_e cat) noexcept;

  // -------------------------------------------------------------------------
  // EDID ioctl result
  // -------------------------------------------------------------------------

  /**
   * @brief Error details from a failed EDID ioctl.
   */
  struct edid_error_t {
    error_category_e category;  ///< Classified error.
    int raw_errno;              ///< Original errno value.
    std::string message;        ///< Human-readable description.
  };

  /**
   * @brief Result type for EDID ioctl operations.
   *
   * @tparam T Success value type.
   */
  template<typename T>
  using edid_result_t = std::expected<T, edid_error_t>;

  // -------------------------------------------------------------------------
  // EDID capability classification
  // -------------------------------------------------------------------------

  /**
   * @brief Capability flags for EDID operations on a device.
   *
   * Read and write are reported separately.  Auto-negotiation requires
   * all three: readable, writable, and restorable.
   */
  struct edid_capability_t {
    bool readable {};     ///< A complete original EDID was read successfully.
    bool writable {};     ///< Reserved: a non-mutating probe cannot establish this.
    bool restorable {};   ///< Reserved: only a guarded write/restore can establish this.

    /**
     * @brief Whether auto-negotiation is safe on this device.
     *
     * @return true only when readable, writable, and restorable are all set.
     */
    constexpr bool allows_negotiation() const noexcept {
      return readable && writable && restorable;
    }
  };

  // -------------------------------------------------------------------------
  // ioctl backend abstraction
  // -------------------------------------------------------------------------

  /**
   * @brief Abstract ioctl backend for EDID operations.
   *
   * Production code calls the real V4L2 ioctl; tests inject a mock/fake.
   * The interface deliberately uses raw parameters matching v4l2_edid so
   * that ioctl details remain contained within this abstraction layer.
   */
  class ioctl_backend_t {
  public:
    virtual ~ioctl_backend_t() = default;

    /**
     * @brief Read EDID data from the device.
     *
     * Wraps VIDIOC_G_EDID: fills the provided buffer with EDID blocks.
     *
     * @param pad V4L2 pad or input index.
     * @param start_block First block to read (0-based).
     * @param block_count Number of 128-byte blocks to read.
     * @param buffer Output buffer with at least block_count * 128 bytes.
     * @return Number of blocks actually read, or an error.
     */
    virtual edid_result_t<std::uint32_t> get_edid(
      std::uint32_t pad,
      std::uint32_t start_block,
      std::uint32_t block_count,
      std::span<std::uint8_t> buffer) = 0;

    /**
     * @brief Write EDID data to the device.
     *
     * Wraps VIDIOC_S_EDID: writes EDID blocks to the device.
     *
     * @param pad V4L2 pad or input index.
     * @param start_block First block to write (0-based).
     * @param block_count Number of 128-byte blocks to write.
     * @param data EDID data to write (must be block_count * 128 bytes).
     * @return Number of blocks actually written, or an error.
     */
    virtual edid_result_t<std::uint32_t> set_edid(
      std::uint32_t pad,
      std::uint32_t start_block,
      std::uint32_t block_count,
      std::span<const std::uint8_t> data) = 0;

    /**
     * @brief Enable or disable an optional HDMI RX audio domain.
     *
     * Rockchip HDMI RX devices require this operation after their HDMI link
     * has been configured.  Other V4L2 devices do not implement it and can
     * safely report not_supported.
     *
     * @param enabled true to start HDMI audio detection and output.
     * @return Success, or not_supported when the device has no audio control.
     */
    virtual edid_result_t<void> set_audio_enabled(bool enabled) {
      (void) enabled;
      return std::unexpected(edid_error_t{error_category_e::not_supported, ENOTTY, "HDMI audio control is not supported"});
    }

  };

  // -------------------------------------------------------------------------
  // EDID data model and parsing
  // -------------------------------------------------------------------------

  /**
   * @brief A parsed timing descriptor from an EDID block.
   *
   * Contains pixel clock, active dimensions, blanking, and sync information
   * needed to construct hdmi_mode_t for the Stage 1 selector.
   */
  struct timing_descriptor_t {
    std::uint32_t pixel_clock_khz {};  ///< Pixel clock in kHz (10 kHz units in EDID * 10).
    std::uint16_t h_active {};         ///< Horizontal active pixels.
    std::uint16_t h_blanking {};       ///< Horizontal blanking pixels.
    std::uint16_t v_active {};         ///< Vertical active lines.
    std::uint16_t v_blanking {};       ///< Vertical blanking lines.
  };

  /**
   * @brief Compute the EDID block checksum.
   *
   * The sum of all 128 bytes in a valid block must be zero modulo 256.
   *
   * @param block Exactly 128 bytes of EDID data.
   * @return Computed checksum (0 means valid).
   */
  std::uint8_t compute_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept;

  /**
   * @brief Validate the checksum of an EDID block.
   *
   * @param block Exactly 128 bytes of EDID data.
   * @return true if the checksum is valid (sum of all bytes == 0 mod 256).
   */
  bool validate_block_checksum(std::span<const std::uint8_t, k_edid_block_size> block) noexcept;

  /**
   * @brief Validate checksums for all blocks in an EDID blob.
   *
   * @param edid_data Complete EDID data (must be a multiple of 128 bytes).
   * @return true if all block checksums are valid.
   */
  bool validate_edid_checksums(std::span<const std::uint8_t> edid_data) noexcept;

  /**
   * @brief Parse a detailed timing descriptor from EDID base block bytes 54-71.
   *
   * @param descriptor 18 bytes starting at the timing descriptor position.
   * @return Parsed timing, or std::nullopt if the descriptor is a monitor descriptor.
   */
  std::optional<timing_descriptor_t> parse_timing_descriptor(
    std::span<const std::uint8_t, 18> descriptor) noexcept;

  /**
   * @brief Extract all detailed timing descriptors from a base EDID block.
   *
   * The base block contains up to 4 detailed timing descriptor slots at
   * bytes 54-125.  Non-timing descriptors (pixel clock == 0) are skipped.
   *
   * @param base_block 128-byte base EDID block.
   * @return Vector of parsed timing descriptors found.
   */
  std::vector<timing_descriptor_t> parse_base_block_timings(
    std::span<const std::uint8_t, k_edid_block_size> base_block) noexcept;

  /**
   * @brief Convert a timing descriptor to an hdmi_mode_t for the Stage 1 selector.
   *
   * Computes resolution and refresh rate from pixel clock and blanking values.
   *
   * @param timing Parsed timing descriptor.
   * @return HDMI mode suitable for select_hdmi_mode(), or std::nullopt
   *         if the timing has zero dimensions or pixel clock.
   */
  std::optional<platf::hdmirx::hdmi_mode_t> timing_to_hdmi_mode(
    const timing_descriptor_t &timing) noexcept;

  /**
   * @brief Parse a complete EDID blob and extract all HDMI modes.
   *
   * Validates every declared block, parses detailed timing descriptors from the
   * base block, and adds recognized progressive CTA-861 video modes. Duplicate
   * resolution/refresh pairs are collapsed.
   *
   * @param edid_data Complete EDID data.
   * @return Vector of HDMI modes, empty if EDID is invalid or has no timings.
   */
  std::vector<platf::hdmirx::hdmi_mode_t> parse_edid_modes(
    std::span<const std::uint8_t> edid_data) noexcept;

  /**
   * @brief Restrict a valid source EDID to one advertised resolution.
   *
   * Preserves the receiver identity and non-video CTA capability blocks such
   * as audio, HDMI VSDB, and HDMI Forum VSDB. Video descriptors are rewritten
   * so the selected resolution is preferred and other resolutions are no
   * longer advertised. This is required for HDMI 2.0 sources that reject a
   * synthetic minimal EDID lacking their negotiated link capabilities.
   *
   * @param source_edid Complete EDID currently advertised by the receiver.
   * @param target Resolution selected from parse_edid_modes(source_edid).
   * @return Restricted EDID with valid checksums, or an empty vector when the
   * source is invalid, the target was not advertised, or cannot be represented.
   */
  std::vector<std::uint8_t> restrict_edid_to_resolution(
    std::span<const std::uint8_t> source_edid,
    const platf::hdmirx::resolution_t &target) noexcept;

  // -------------------------------------------------------------------------
  // EDID fixture generation
  // -------------------------------------------------------------------------

  /**
   * @brief Generate a minimal valid EDID base block for a single preferred mode.
   *
   * Creates a 128-byte EDID with a single detailed timing descriptor as the
   * preferred mode.  The checksum is computed and placed at byte 127.
   *
   * @param h_active Horizontal active pixels.
   * @param v_active Vertical active lines.
   * @param h_blanking Horizontal blanking pixels.
   * @param v_blanking Vertical blanking lines.
   * @param pixel_clock_10khz Pixel clock in 10 kHz units.
   * @return A 128-byte EDID base block with valid checksum.
   */
  std::vector<std::uint8_t> generate_edid_base_block(
    std::uint16_t h_active,
    std::uint16_t v_active,
    std::uint16_t h_blanking,
    std::uint16_t v_blanking,
    std::uint16_t pixel_clock_10khz) noexcept;

  /**
   * @brief Add a CTA-861 extension advertising HDMI stereo LPCM audio.
   *
   * The generated extension advertises a native CTA video mode, basic audio,
   * two-channel LPCM, and HDMI vendor data. This lets HDMI sources such as
   * Nintendo Switch identify the virtual receiver as HDMI and audio capable.
   *
   * @param base_edid A single valid 128-byte EDID base block.
   * @param native_cta_vic CTA video identification code for the native mode,
   * or zero to omit the CTA video data block.
   * @return A two-block EDID, or an empty vector when the input is not a
   * single valid base block.
   */
  std::vector<std::uint8_t> with_cta_lpcm_audio_extension(
    std::span<const std::uint8_t> base_edid,
    std::uint8_t native_cta_vic);

  /**
   * @brief Generate an EDID fixture for 720p (1280x720@60Hz).
   * @return 128-byte EDID with valid checksum.
   */
  std::vector<std::uint8_t> make_720p_edid() noexcept;

  /**
   * @brief Generate an EDID fixture for 1080p (1920x1080@60Hz).
   * @return 128-byte EDID with valid checksum.
   */
  std::vector<std::uint8_t> make_1080p_edid() noexcept;

  /**
   * @brief Generate an EDID fixture for 1440p (2560x1440@60Hz).
   * @return 128-byte EDID with valid checksum.
   */
  std::vector<std::uint8_t> make_1440p_edid() noexcept;

  /**
   * @brief Generate an EDID fixture for 2160p (3840x2160@30Hz).
   * @return 128-byte EDID with valid checksum.
   */
  std::vector<std::uint8_t> make_2160p_edid() noexcept;

  /**
   * @brief Generate a 128-byte EDID with an intentionally bad checksum.
   * @return EDID data where the checksum byte is corrupted.
   */
  std::vector<std::uint8_t> make_bad_checksum_edid() noexcept;

  /**
   * @brief Generate a truncated EDID (less than 128 bytes).
   * @return 64 bytes of EDID-like data (not a complete block).
   */
  std::vector<std::uint8_t> make_truncated_edid() noexcept;

  // -------------------------------------------------------------------------
  // High-level EDID device operations
  // -------------------------------------------------------------------------

  /**
   * @brief Read the complete EDID from a device.
   *
   * Reads all blocks starting from block 0.  Validates the returned size
   * is a multiple of 128 bytes.
   *
   * @param backend ioctl backend to use.
   * @param pad V4L2 pad or input index.
   * @return Complete EDID data, or an error.
   */
  edid_result_t<std::vector<std::uint8_t>> read_edid(
    ioctl_backend_t &backend,
    std::uint32_t pad = 0);

  /**
   * @brief Write a complete EDID to a device.
   *
   * Validates that data size is a multiple of 128 bytes and does not
   * exceed the maximum.  Writes all blocks starting from block 0.
   *
   * @param backend ioctl backend to use.
   * @param pad V4L2 pad or input index.
   * @param data Complete EDID data to write.
   * @return Number of blocks written, or an error.
   */
  edid_result_t<std::uint32_t> write_edid(
    ioctl_backend_t &backend,
    std::uint32_t pad,
    std::span<const std::uint8_t> data);

  /**
   * @brief Probe the EDID capabilities of a device.
   *
   * This is deliberately a read-only probe: testing VIDIOC_S_EDID would
   * mutate the device before its complete original EDID is protected.  Thus
   * writable and restorable remain false; callers must construct and verify
   * an edid_restore_guard_t before attempting a real, checked write.
   *
   * @param backend ioctl backend to use.
   * @param pad V4L2 pad or input index.
   * @return Capability flags for this device/pad combination.
   */
  edid_capability_t probe_capabilities(
    ioctl_backend_t &backend,
    std::uint32_t pad = 0);

  // -------------------------------------------------------------------------
  // RAII session restore guard
  // -------------------------------------------------------------------------

  /**
   * @brief Logger callback type for restore guard diagnostics.
   *
   * @param message Human-readable log message.
   */
  using log_callback_t = std::function<void(const std::string &message)>;

  /**
   * @brief RAII guard that saves and restores the original EDID on scope exit.
   *
   * On construction, reads the current EDID from the device and stores it.
   * On destruction (or explicit restore), writes the original EDID back.
   *
   * Key properties:
   * - Destructor never throws; restore failures are logged via the callback.
   * - Supports explicit restore() for early restoration.
   * - Idempotent: repeated restore() calls after the first are no-ops.
   * - Construction failure (cannot read original EDID) leaves the guard
   *   in a disarmed state where restore() is a no-op.
   */
  class edid_restore_guard_t {
  public:
    /**
     * @brief Construct and arm the restore guard.
     *
     * Reads the current EDID from the device.  If the read fails,
     * the guard is constructed in a disarmed state and logs the error.
     *
     * @param backend ioctl backend for read/write operations.
     * @param pad V4L2 pad or input index.
     * @param logger Callback for diagnostic messages (may be empty).
     */
    edid_restore_guard_t(
      ioctl_backend_t &backend,
      std::uint32_t pad,
      log_callback_t logger = {});

    /// @brief Guards are not copyable.
    edid_restore_guard_t(const edid_restore_guard_t &) = delete;
    /// @brief Guards are not copyable.
    edid_restore_guard_t &operator=(const edid_restore_guard_t &) = delete;

    /// @brief Move construction transfers ownership.
    edid_restore_guard_t(edid_restore_guard_t &&other) noexcept;
    /// @brief Move assignment transfers ownership.
    edid_restore_guard_t &operator=(edid_restore_guard_t &&other) noexcept;

    /**
     * @brief Restore the original EDID on destruction.
     *
     * Never throws.  Logs failures via the logger callback.
     */
    ~edid_restore_guard_t();

    /**
     * @brief Explicitly restore the original EDID.
     *
     * After a successful restore, the guard is disarmed.
     * Calling restore() on a disarmed guard is a no-op returning true.
     *
     * @return true if the restore succeeded or was already done.
     */
    bool restore() noexcept;

    /**
     * @brief Check whether the guard holds a saved EDID.
     *
     * @return true if the guard is armed and will restore on destruction.
     */
    bool is_armed() const noexcept;

    /**
     * @brief Access the saved original EDID data.
     *
     * @return The saved EDID, empty if the guard is disarmed.
     */
    const std::vector<std::uint8_t> &saved_edid() const noexcept;

  private:
    ioctl_backend_t *backend_ {};           ///< Backend for restore write.
    std::uint32_t pad_ {};                  ///< Pad index for restore.
    std::vector<std::uint8_t> saved_edid_;  ///< Original EDID data.
    bool armed_ {};                         ///< Whether restore is pending.
    log_callback_t logger_;                 ///< Diagnostic logger.

    /**
     * @brief Log a message if a logger is configured.
     *
     * @param msg Message to log.
     */
    void log(const std::string &msg) const;
  };

}  // namespace platf::edid
