/**
 * @file src/platform/hdmirx_policy.cpp
 * @brief Hardware-independent HDMI RX sizing and mode-selection policy.
 */
#include "src/platform/hdmirx_policy.h"

#include <algorithm>
#include <limits>

namespace platf::hdmirx {
  namespace {
    using wide_uint_t = unsigned __int128;

    /**
     * @brief Exact non-negative rational distance used by mode tie-breakers.
     */
    struct rational_distance_t {
      wide_uint_t numerator {};
      wide_uint_t denominator {1};
    };

    /**
     * @brief Compare two exact rational distances without floating-point rounding.
     */
    bool rational_distance_less(const rational_distance_t &left, const rational_distance_t &right) noexcept {
      return left.numerator * right.denominator < right.numerator * left.denominator;
    }

    /**
     * @brief Check whether two exact rational distances are equal.
     */
    bool rational_distance_equal(const rational_distance_t &left, const rational_distance_t &right) noexcept {
      return left.numerator * right.denominator == right.numerator * left.denominator;
    }

    /**
     * @brief Check whether a dimension is even.
     */
    bool is_even(std::uint32_t value) noexcept {
      return (value & 1U) == 0;
    }

    /**
     * @brief Check whether an alignment is a nonzero power of two.
     */
    bool is_power_of_two(std::uint32_t value) noexcept {
      return value != 0 && (value & (value - 1U)) == 0;
    }

    /**
     * @brief Compute a validated resolution's pixel area.
     */
    std::uint64_t area(const resolution_t &resolution) noexcept {
      return static_cast<std::uint64_t>(resolution.width) * resolution.height;
    }

    /**
     * @brief Compute the sum of absolute width and height differences.
     */
    std::uint64_t dimension_delta(const resolution_t &left, const resolution_t &right) noexcept {
      const auto width_delta = left.width >= right.width ? left.width - right.width : right.width - left.width;
      const auto height_delta = left.height >= right.height ? left.height - right.height : right.height - left.height;
      return static_cast<std::uint64_t>(width_delta) + height_delta;
    }

    /** Return the exact difference between two image aspect ratios. */
    rational_distance_t aspect_delta(const resolution_t &left, const resolution_t &right) noexcept {
      const auto lhs = static_cast<wide_uint_t>(left.width) * right.height;
      const auto rhs = static_cast<wide_uint_t>(right.width) * left.height;
      return {
        lhs >= rhs ? lhs - rhs : rhs - lhs,
        static_cast<wide_uint_t>(left.height) * right.height,
      };
    }

    /**
     * @brief Compute the greatest common divisor of two positive integers.
     */
    std::uint32_t gcd(std::uint32_t left, std::uint32_t right) noexcept {
      while (right != 0) {
        const auto remainder = left % right;
        left = right;
        right = remainder;
      }
      return left;
    }

    /**
     * @brief Check whether a refresh rate is a positive rational.
     */
    bool valid_refresh(const refresh_rate_t &rate) noexcept {
      return rate.numerator != 0 && rate.denominator != 0;
    }

    /**
     * @brief Reduce a valid refresh rate to canonical numerator and denominator.
     */
    refresh_rate_t normalized(refresh_rate_t rate) noexcept {
      const auto divisor = gcd(rate.numerator, rate.denominator);
      rate.numerator /= divisor;
      rate.denominator /= divisor;
      return rate;
    }

    /** Return the exact difference between two refresh-rate rationals. */
    rational_distance_t refresh_delta(const refresh_rate_t &left, const refresh_rate_t &right) noexcept {
      const auto lhs = static_cast<wide_uint_t>(left.numerator) * right.denominator;
      const auto rhs = static_cast<wide_uint_t>(right.numerator) * left.denominator;
      return {
        lhs >= rhs ? lhs - rhs : rhs - lhs,
        static_cast<wide_uint_t>(left.denominator) * right.denominator,
      };
    }

    /** Compare two valid refresh-rate rationals without overflow. */
    bool refresh_less(const refresh_rate_t &left, const refresh_rate_t &right) noexcept {
      return static_cast<wide_uint_t>(left.numerator) * right.denominator < static_cast<wide_uint_t>(right.numerator) * left.denominator;
    }

    /**
     * @brief Round a width up to a power-of-two alignment without overflow.
     */
    std::optional<std::uint32_t> aligned_stride(std::uint32_t width, std::uint32_t alignment) noexcept {
      if (!is_power_of_two(alignment)) {
        return std::nullopt;
      }
      const auto remainder = width & (alignment - 1U);
      const auto padding = remainder == 0 ? 0U : alignment - remainder;
      if (width > std::numeric_limits<std::uint32_t>::max() - padding) {
        return std::nullopt;
      }
      return width + padding;
    }
  }  // namespace

  bool is_valid_resolution(const resolution_t &resolution) noexcept {
    return resolution.width != 0 && resolution.height != 0 && resolution.width <= k_max_policy_dimension && resolution.height <= k_max_policy_dimension;
  }

  bool needs_conversion(const resolution_t &input, const resolution_t &target) noexcept {
    return input != target;
  }

  std::optional<viewport_t> make_viewport(const resolution_t &input, const resolution_t &target, pixel_format_e format) noexcept {
    if (!is_valid_resolution(input) || !is_valid_resolution(target)) {
      return std::nullopt;
    }
    if (format == pixel_format_e::nv12 && (!is_even(input.width) || !is_even(input.height) || !is_even(target.width) || !is_even(target.height))) {
      return std::nullopt;
    }

    std::uint32_t destination_width;
    std::uint32_t destination_height;
    if (static_cast<std::uint64_t>(target.width) * input.height <= static_cast<std::uint64_t>(target.height) * input.width) {
      destination_width = target.width;
      destination_height = static_cast<std::uint32_t>((static_cast<std::uint64_t>(target.width) * input.height) / input.width);
    } else {
      destination_height = target.height;
      destination_width = static_cast<std::uint32_t>((static_cast<std::uint64_t>(target.height) * input.width) / input.height);
    }
    destination_width = std::max(destination_width, 1U);
    destination_height = std::max(destination_height, 1U);

    if (format == pixel_format_e::nv12) {
      destination_width &= ~1U;
      destination_height &= ~1U;
      if (destination_width < 2 || destination_height < 2) {
        return std::nullopt;
      }
      // Keep the centered offsets even, as required by 4:2:0 chroma samples.
      if (((target.width - destination_width) & 3U) != 0) {
        destination_width -= 2;
      }
      if (((target.height - destination_height) & 3U) != 0) {
        destination_height -= 2;
      }
      if (destination_width < 2 || destination_height < 2) {
        return std::nullopt;
      }
    }

    return viewport_t {
      .source = rectangle_t {0, 0, input.width, input.height},
      .destination = rectangle_t {(target.width - destination_width) / 2, (target.height - destination_height) / 2, destination_width, destination_height},
    };
  }

  std::optional<nv12_layout_t> make_nv12_layout(const resolution_t &resolution, std::uint32_t stride, std::uint32_t stride_alignment) noexcept {
    if (!is_valid_resolution(resolution) || !is_even(resolution.width) || !is_even(resolution.height)) {
      return std::nullopt;
    }
    const auto derived_stride = aligned_stride(resolution.width, stride_alignment);
    if (!derived_stride.has_value()) {
      return std::nullopt;
    }
    if (stride == 0) {
      stride = *derived_stride;
    }
    if (stride < resolution.width || stride % stride_alignment != 0) {
      return std::nullopt;
    }
    const auto y_size = static_cast<std::uint64_t>(stride) * resolution.height;
    const auto uv_size = static_cast<std::uint64_t>(stride) * (resolution.height / 2);
    if (y_size > std::numeric_limits<std::uint64_t>::max() - uv_size) {
      return std::nullopt;
    }
    return nv12_layout_t {resolution, stride, y_size + uv_size};
  }

  std::optional<hdmi_mode_t> select_hdmi_mode(const std::vector<hdmi_mode_t> &candidates, const resolution_t &target, std::optional<refresh_rate_t> requested_refresh) noexcept {
    if (!is_valid_resolution(target) || (requested_refresh.has_value() && !valid_refresh(*requested_refresh))) {
      return std::nullopt;
    }

    std::vector<const hdmi_mode_t *> valid;
    valid.reserve(candidates.size());
    for (const auto &candidate : candidates) {
      if (candidate.verified && is_valid_resolution(candidate.resolution) && valid_refresh(candidate.refresh_rate)) {
        valid.push_back(&candidate);
      }
    }
    if (valid.empty()) {
      return std::nullopt;
    }

    const auto has_sufficient_mode = std::any_of(valid.begin(), valid.end(), [&target](const hdmi_mode_t *mode) {
      return mode->resolution.width >= target.width && mode->resolution.height >= target.height;
    });
    const auto target_refresh = requested_refresh.value_or(refresh_rate_t {});
    const auto better = [=](const hdmi_mode_t *left, const hdmi_mode_t *right) {
      const auto left_sufficient = left->resolution.width >= target.width && left->resolution.height >= target.height;
      const auto right_sufficient = right->resolution.width >= target.width && right->resolution.height >= target.height;
      if (has_sufficient_mode) {
        if (left_sufficient != right_sufficient) {
          return left_sufficient;
        }
        if (area(left->resolution) != area(right->resolution)) {
          return left_sufficient ? area(left->resolution) < area(right->resolution) : false;
        }
        if (left_sufficient && dimension_delta(left->resolution, target) != dimension_delta(right->resolution, target)) {
          return dimension_delta(left->resolution, target) < dimension_delta(right->resolution, target);
        }
      } else if (area(left->resolution) != area(right->resolution)) {
        return area(left->resolution) > area(right->resolution);
      }
      const auto left_aspect_delta = aspect_delta(left->resolution, target);
      const auto right_aspect_delta = aspect_delta(right->resolution, target);
      if (!rational_distance_equal(left_aspect_delta, right_aspect_delta)) {
        return rational_distance_less(left_aspect_delta, right_aspect_delta);
      }
      if (requested_refresh.has_value()) {
        const auto left_refresh_delta = refresh_delta(left->refresh_rate, target_refresh);
        const auto right_refresh_delta = refresh_delta(right->refresh_rate, target_refresh);
        if (!rational_distance_equal(left_refresh_delta, right_refresh_delta)) {
          return rational_distance_less(left_refresh_delta, right_refresh_delta);
        }
      }
      if (left->resolution.width != right->resolution.width) {
        return left->resolution.width < right->resolution.width;
      }
      if (left->resolution.height != right->resolution.height) {
        return left->resolution.height < right->resolution.height;
      }
      const auto left_rate = normalized(left->refresh_rate);
      const auto right_rate = normalized(right->refresh_rate);
      if (left_rate.numerator != right_rate.numerator || left_rate.denominator != right_rate.denominator) {
        return refresh_less(left_rate, right_rate);
      }
      if (left->refresh_rate.numerator != right->refresh_rate.numerator) {
        return left->refresh_rate.numerator < right->refresh_rate.numerator;
      }
      if (left->refresh_rate.denominator != right->refresh_rate.denominator) {
        return left->refresh_rate.denominator < right->refresh_rate.denominator;
      }
      return left->verified < right->verified;
    };

    return **std::min_element(valid.begin(), valid.end(), better);
  }
}  // namespace platf::hdmirx
