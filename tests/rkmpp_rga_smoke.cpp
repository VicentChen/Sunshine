/**
 * @file tests/rkmpp_rga_smoke.cpp
 * @brief Hardware smoke test for synchronous librga DMA-BUF operations.
 */
#include <src/platform/linux/rga.h>

#include <dirent.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
  /**
   * @brief Command-line options for the RGA smoke test.
   */
  struct options_t {
    std::uint32_t width {128};  ///< Even visible test width.
    std::uint32_t height {64};  ///< Even visible test height.
    std::uint32_t rounds {3};  ///< Repeated allocation and destruction count.
  };

  /**
   * @brief Count currently open file descriptors.
   *
   * @return Descriptor count, or zero if `/proc/self/fd` cannot be opened.
   */
  std::size_t fd_count() noexcept {
    auto *directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
      return 0;
    }
    std::size_t count {};
    while (const auto *entry = ::readdir(directory)) {
      if (entry->d_name[0] != '.') {
        ++count;
      }
    }
    (void) ::closedir(directory);
    return count;
  }

  /**
   * @brief Parse one positive unsigned command-line value.
   *
   * @param value Text value to parse.
   * @param option Option name used in error output.
   * @return Parsed positive value.
   */
  std::uint32_t parse_positive(const char *value, const char *option) {
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument(std::string(option) + " must be a positive uint32 value");
    }
    return static_cast<std::uint32_t>(parsed);
  }

  /**
   * @brief Parse smoke-test arguments.
   *
   * @param argc Argument count.
   * @param argv Argument vector.
   * @return Parsed options.
   */
  options_t parse_options(int argc, char *argv[]) {
    options_t options;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if ((argument == "--width" || argument == "--height" || argument == "--rounds") && index + 1 < argc) {
        const auto value = parse_positive(argv[++index], argument.c_str());
        if (argument == "--width") {
          options.width = value;
        } else if (argument == "--height") {
          options.height = value;
        } else {
          options.rounds = value;
        }
      } else {
        throw std::invalid_argument("usage: rkmpp_rga_smoke [--width even] [--height even] [--rounds positive]");
      }
    }
    if ((options.width & 1U) != 0 || (options.height & 1U) != 0) {
      throw std::invalid_argument("RGA NV12 smoke dimensions must be even");
    }
    return options;
  }
}  // namespace

/**
 * @brief Run RGA CMA DMA-BUF fill, resize, and lifetime smoke checks.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return Zero on success or one on a failed check.
 */
int main(int argc, char *argv[]) {
  try {
    const auto options = parse_options(argc, argv);
    auto backend = platf::rga::make_backend();
    auto allocator = platf::rga::make_cma_dma_allocator();

    // librga lazily opens a process-global /dev/rga descriptor when the first
    // job runs, and keeps it for the process lifetime. Complete one whole
    // target-buffer lifecycle before sampling so this stable SDK resource is
    // not confused with a per-buffer ownership leak.
    {
      auto source = platf::rga::target_buffer_t::allocate_nv12(*backend, *allocator, options.width, options.height);
      auto destination = platf::rga::target_buffer_t::allocate_nv12(*backend, *allocator, options.width, options.height);
      const auto &layout = destination.layout();
      if (layout.stride < options.width || layout.allocation_size != static_cast<std::uint64_t>(layout.stride) * (options.height + options.height / 2U)) {
        throw std::runtime_error("RGA target layout is inconsistent");
      }
      platf::rga::fill(source.rga_buffer(), {0, 0, options.width, options.height}, 0x00800080U);
      platf::rga::fill(destination.rga_buffer(), {0, 0, options.width, options.height}, 0x00800080U);
      platf::rga::resize(source.rga_buffer(), destination.rga_buffer());
    }
    const auto descriptors_before = fd_count();
    for (std::uint32_t round = 0; round < options.rounds; ++round) {
      auto source = platf::rga::target_buffer_t::allocate_nv12(*backend, *allocator, options.width, options.height);
      auto destination = platf::rga::target_buffer_t::allocate_nv12(*backend, *allocator, options.width, options.height);
      const auto &layout = destination.layout();
      if (layout.stride < options.width || layout.allocation_size != static_cast<std::uint64_t>(layout.stride) * (options.height + options.height / 2U)) {
        throw std::runtime_error("RGA target layout is inconsistent");
      }
      platf::rga::fill(source.rga_buffer(), {0, 0, options.width, options.height}, 0x00800080U);
      platf::rga::fill(destination.rga_buffer(), {0, 0, options.width, options.height}, 0x00800080U);
      platf::rga::resize(source.rga_buffer(), destination.rga_buffer());
    }
    const auto descriptors_after = fd_count();
    if (descriptors_before != 0 && descriptors_after != descriptors_before) {
      throw std::runtime_error("RGA repeated creation/destruction leaked file descriptors");
    }
    std::cout << "rkmpp_rga_smoke=PASS width=" << options.width << " height=" << options.height << " rounds=" << options.rounds << " fd_before=" << descriptors_before << " fd_after=" << descriptors_after << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "rkmpp_rga_smoke=FAIL: " << error.what() << '\n';
    return 1;
  }
}
