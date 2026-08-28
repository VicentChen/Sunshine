/**
 * @file src/platform/hdmirx_policy.h
 * @brief Hardware-independent HDMI RX sizing and mode-selection policy.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace platf::hdmirx {
  /** Maximum dimension accepted by the policy arithmetic. */
  inline constexpr std::uint32_t k_max_policy_dimension = 32'768;

  /**
   * @brief A visible image size in pixels.
   */
  struct resolution_t {
    std::uint32_t width {};  ///< Visible width in pixels.
    std::uint32_t height {};  ///< Visible height in pixels.

    friend constexpr bool operator==(const resolution_t &, const resolution_t &) = default;
  };

  /**
   * @brief A refresh rate represented as a positive rational number.
   */
  struct refresh_rate_t {
    std::uint32_t numerator {};  ///< Refresh-rate numerator.
    std::uint32_t denominator {1};  ///< Refresh-rate denominator.

    friend constexpr bool operator==(const refresh_rate_t &, const refresh_rate_t &) = default;
  };

  /**
   * @brief An HDMI mode advertised by a verified EDID mode set.
   *
   * The policy never reads or writes EDID.  Callers must set `verified` only
   * after validating the mode came from the EDID they intend to write.
   */
  struct hdmi_mode_t {
    resolution_t resolution;  ///< Active image dimensions.
    refresh_rate_t refresh_rate;  ///< Mode refresh rate.
    bool verified {};  ///< Whether the mode passed EDID validation.

    friend constexpr bool operator==(const hdmi_mode_t &, const hdmi_mode_t &) = default;
  };

  /**
   * @brief A rectangle expressed in pixels.
   */
  struct rectangle_t {
    std::uint32_t left {};  ///< Horizontal offset from the containing image.
    std::uint32_t top {};  ///< Vertical offset from the containing image.
    std::uint32_t width {};  ///< Rectangle width.
    std::uint32_t height {};  ///< Rectangle height.

    friend constexpr bool operator==(const rectangle_t &, const rectangle_t &) = default;
  };

  /**
   * @brief Source and destination rectangles for a hardware conversion.
   */
  struct viewport_t {
    rectangle_t source;  ///< Source crop rectangle, normally the full input.
    rectangle_t destination;  ///< Destination rectangle inside the output canvas.

    friend constexpr bool operator==(const viewport_t &, const viewport_t &) = default;
  };

  /** Pixel format constraints relevant to the sizing policy. */
  enum class pixel_format_e {
    generic,  ///< No subsampling alignment constraints.
    nv12,  ///< 4:2:0 semi-planar format requiring even dimensions and offsets.
  };

  /**
   * @brief Layout details for an NV12 allocation.
   */
  struct nv12_layout_t {
    resolution_t visible;  ///< Visible dimensions.
    std::uint32_t stride {};  ///< Bytes per line for both planes.
    std::uint64_t allocation_size {};  ///< Total Y plus UV allocation size.
  };

  /**
   * @brief Check whether a resolution is safe for policy arithmetic.
   *
   * @param resolution Resolution to validate.
   * @return true when both dimensions are nonzero and within policy limits.
   */
  bool is_valid_resolution(const resolution_t &resolution) noexcept;

  /**
   * @brief Determine whether input dimensions need a conversion before encode.
   *
   * @param input Actual HDMI RX input dimensions I.
   * @param target Moonlight requested coded dimensions T.
   * @return true when a conversion path is required.
   */
  bool needs_conversion(const resolution_t &input, const resolution_t &target) noexcept;

  /**
   * @brief Calculate a centered, aspect-preserving viewport.
   *
   * The source rectangle covers the complete input.  Generic output sizes may
   * be odd.  NV12 requires even source and output dimensions and additionally
   * makes the centered destination offsets even for chroma alignment.
   *
   * @param input Actual input dimensions I.
   * @param target Output dimensions T.
   * @param format Chroma-alignment constraints to apply.
   * @return A viewport, or `std::nullopt` for invalid or unrepresentable sizes.
   */
  std::optional<viewport_t> make_viewport(const resolution_t &input, const resolution_t &target, pixel_format_e format = pixel_format_e::generic) noexcept;

  /**
   * @brief Determine whether a viewport overwrites the complete output canvas.
   *
   * @param viewport Source and destination rectangles selected for conversion.
   * @param target Output canvas dimensions.
   * @return true when the destination rectangle is exactly the output canvas.
   */
  bool viewport_covers_target(const viewport_t &viewport, const resolution_t &target) noexcept;

  /**
   * @brief Validate or derive an NV12 stride and allocation layout.
   *
   * NV12 visible width and height must be even.  A zero stride requests the
   * smallest stride aligned to `stride_alignment`; a supplied stride must be
   * at least the visible width and use the same alignment.
   *
   * @param resolution Visible NV12 dimensions.
   * @param stride Bytes per line, or zero to derive it.
   * @param stride_alignment Required power-of-two stride alignment in bytes.
   * @return Validated layout, or `std::nullopt` when any rule is violated.
   */
  std::optional<nv12_layout_t> make_nv12_layout(const resolution_t &resolution, std::uint32_t stride = 0, std::uint32_t stride_alignment = 2) noexcept;

  /**
   * @brief Select one deterministic mode from a verified EDID candidate set.
   *
   * Modes meeting both target dimensions are preferred.  Within that group
   * the smallest area wins; if none meet both dimensions, the largest area
   * wins.  Dimension delta, aspect-ratio delta, refresh-rate delta, and finally
   * lexical mode fields provide stable tie-breakers independent of input order.
   *
   * @param candidates Modes prepared from and verified against the EDID.
   * @param target Moonlight requested dimensions T.
   * @param requested_refresh Optional Moonlight requested refresh rate.
   * @return The selected mode, or `std::nullopt` for no valid candidate.
   */
  std::optional<hdmi_mode_t> select_hdmi_mode(const std::vector<hdmi_mode_t> &candidates, const resolution_t &target, std::optional<refresh_rate_t> requested_refresh = std::nullopt) noexcept;
}  // namespace platf::hdmirx
