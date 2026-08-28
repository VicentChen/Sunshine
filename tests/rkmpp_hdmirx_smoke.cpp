/**
 * @file tests/rkmpp_hdmirx_smoke.cpp
 * @brief Hardware smoke test for rk_hdmirx capture and DMA-BUF lifetime.
 */
#include <dirent.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <src/platform/linux/hdmirx.h>

boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> error(1);
boost::log::sources::severity_logger<int> info(1);
namespace config {
  sunshine_t sunshine {};
  video_t video {};
}

namespace {
  struct options_t {
    std::string device {"/dev/video0"};
    std::uint32_t frames {300};
    std::uint32_t rounds {3};
    bool shutdown_race {};
  };

  std::uint32_t parse_positive(const char *value, const char *name) {
    const auto parsed = std::strtoul(value, nullptr, 10);
    if (parsed == 0 || parsed > UINT32_MAX) {
      throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return static_cast<std::uint32_t>(parsed);
  }

  options_t parse_options(int argc, char *argv[]) {
    options_t options;
    for (int index = 1; index < argc; ++index) {
      const std::string argument(argv[index]);
      if (argument == "--device" && index + 1 < argc) {
        options.device = argv[++index];
      } else if (argument == "--frames" && index + 1 < argc) {
        options.frames = parse_positive(argv[++index], "--frames");
      } else if (argument == "--rounds" && index + 1 < argc) {
        options.rounds = parse_positive(argv[++index], "--rounds");
      } else if (argument == "--shutdown-race") {
        options.shutdown_race = true;
      } else {
        throw std::invalid_argument("usage: rkmpp_hdmirx_smoke [--device /dev/video0] [--frames 300] [--rounds 3] [--shutdown-race]");
      }
    }
    return options;
  }

  std::size_t own_fd_count() {
    DIR *directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
      throw std::runtime_error("cannot open /proc/self/fd");
    }
    std::size_t count = 0;
    while (const auto *entry = ::readdir(directory)) {
      if (entry->d_name[0] != '.') {
        ++count;
      }
    }
    ::closedir(directory);
    return count;
  }

  std::string fourcc(std::uint32_t value) {
    std::string text(4, ' ');
    for (std::size_t index = 0; index < text.size(); ++index) {
      text[index] = static_cast<char>((value >> (index * 8U)) & 0xffU);
    }
    return text;
  }

  void log_format(const platf::hdmirx::hdmirx_capture_t &capture) {
    const auto &format = capture.format();
    std::cout << "driver=" << capture.device_info().driver << " card=" << capture.device_info().card
              << " format=" << fourcc(format.fourcc) << " width=" << format.width << " height=" << format.height
              << " planes=" << format.planes.size() << " buffers=" << capture.buffer_count() << '\n';
    for (std::size_t plane = 0; plane < format.planes.size(); ++plane) {
      std::cout << "  plane=" << plane << " bytesperline=" << format.planes[plane].bytesperline
                << " sizeimage=" << format.planes[plane].sizeimage << '\n';
    }
    if (const auto &timings = capture.timings()) {
      const auto &bt = timings->bt;
      const auto horizontal_total = bt.width + bt.hfrontporch + bt.hsync + bt.hbackporch;
      const auto vertical_total = bt.height + bt.vfrontporch + bt.vsync + bt.vbackporch;
      const auto fps = horizontal_total != 0 && vertical_total != 0 ? static_cast<double>(bt.pixelclock) / (static_cast<double>(horizontal_total) * vertical_total) : 0.0;
      std::cout << std::fixed << std::setprecision(3) << "timing_width=" << bt.width << " timing_height=" << bt.height
                << " pixelclock=" << bt.pixelclock << " htotal=" << horizontal_total << " vtotal=" << vertical_total
                << " fps_estimate=" << fps << '\n';
    } else {
      std::cout << "timing=unavailable\n";
    }
  }

  void check_frame(const platf::hdmirx::captured_frame_t &frame, const platf::hdmirx::capture_format_t &format, std::optional<std::uint32_t> previous_sequence) {
    if (previous_sequence && frame.sequence() <= *previous_sequence) {
      throw std::runtime_error("V4L2 sequence is not strictly monotonic");
    }
    if (frame.planes().size() != format.planes.size()) {
      throw std::runtime_error("dequeued plane count differs from G_FMT");
    }
    for (std::size_t plane = 0; plane < frame.planes().size(); ++plane) {
      const auto &metadata = frame.planes()[plane];
      const auto &layout = format.planes[plane];
      if (metadata.dma_buf_fd < 0 || metadata.bytesperline != layout.bytesperline || metadata.sizeimage != layout.sizeimage || metadata.data_offset > metadata.bytesused || metadata.payload_bytes != metadata.bytesused - metadata.data_offset || metadata.bytesused > metadata.sizeimage) {
        throw std::runtime_error("dequeued V4L2/DMA-BUF metadata is invalid");
      }
    }
  }

  void log_frame(const platf::hdmirx::captured_frame_t &frame, std::uint32_t round, std::uint32_t number) {
    std::cout << "round=" << round << " frame=" << number << " sequence=" << frame.sequence()
              << " timestamp_ns=" << frame.timestamp().time_since_epoch().count() << " flags=" << frame.timestamp_flags()
              << " clock=" << (platf::hdmirx::timestamp_is_monotonic(frame.timestamp_flags()) ? "monotonic" : "unsupported")
              << " source=" << platf::hdmirx::timestamp_source_name(platf::hdmirx::timestamp_source(frame.timestamp_flags())) << '\n';
    for (std::size_t plane = 0; plane < frame.planes().size(); ++plane) {
      const auto &metadata = frame.planes()[plane];
      std::cout << "  plane=" << plane << " dma_buf_fd=" << metadata.dma_buf_fd
                << " bytesused=" << metadata.bytesused << " payload_bytes=" << metadata.payload_bytes
                << " bytesperline=" << metadata.bytesperline << " sizeimage=" << metadata.sizeimage << '\n';
    }
  }

  void run_shutdown_race(const std::string &device, std::size_t fd_baseline) {
    enum class waiter_result_t {
      pending,
      dequeued_and_released,
      stopped_by_shutdown,
      unexpected_exception,
    };

    const auto start = std::chrono::steady_clock::now();
    waiter_result_t result = waiter_result_t::pending;
    std::string unexpected_error;
    {
      auto capture = platf::hdmirx::hdmirx_capture_t::open(device);
      std::thread waiter([&] {
        try {
          auto frame = capture.dequeue(std::chrono::seconds(2));
          frame.release();
          result = waiter_result_t::dequeued_and_released;
        } catch (const std::runtime_error &error) {
          const std::string message(error.what());
          if (message == "HDMI RX stream is not active" || message == "HDMI RX stream stopped while waiting for a frame") {
            result = waiter_result_t::stopped_by_shutdown;
          } else {
            unexpected_error = message;
            result = waiter_result_t::unexpected_exception;
          }
        } catch (const std::exception &error) {
          unexpected_error = error.what();
          result = waiter_result_t::unexpected_exception;
        } catch (...) {
          unexpected_error = "non-standard exception";
          result = waiter_result_t::unexpected_exception;
        }
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      capture.shutdown();
      waiter.join();
    }
    // join establishes a happens-before edge for result and unexpected_error.
    if (result == waiter_result_t::pending) {
      throw std::runtime_error("shutdown race waiter did not report a result");
    }
    if (result == waiter_result_t::unexpected_exception) {
      throw std::runtime_error("shutdown race received unexpected dequeue error: " + unexpected_error);
    }
    if (own_fd_count() != fd_baseline) {
      throw std::runtime_error("file descriptor count grew after shutdown race");
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (elapsed > 3.0) {
      throw std::runtime_error("shutdown race exceeded its bounded dequeue timeout");
    }
    const char *outcome = result == waiter_result_t::dequeued_and_released ? "dequeued_and_released" : "stopped_by_shutdown";
    std::cout << "shutdown_race_seconds=" << elapsed << " outcome=" << outcome << " fd_count=" << fd_baseline << '\n';
  }
}  // namespace

int main(int argc, char *argv[]) {
  std::optional<std::size_t> fd_baseline;
  try {
    const auto options = parse_options(argc, argv);
    fd_baseline = own_fd_count();
    std::cout << "device=" << options.device << " frames=" << options.frames << " rounds=" << options.rounds << " fd_baseline=" << *fd_baseline << '\n';
    if (options.shutdown_race) {
      run_shutdown_race(options.device, *fd_baseline);
      std::cout << "rkmpp_hdmirx_shutdown_race=PASS\n";
      return 0;
    }
    for (std::uint32_t round = 0; round < options.rounds; ++round) {
      const auto start = std::chrono::steady_clock::now();
      {
        auto capture = platf::hdmirx::hdmirx_capture_t::open(options.device);
        log_format(capture);
        std::optional<std::uint32_t> previous_sequence;
        for (std::uint32_t number = 0; number < options.frames; ++number) {
          auto frame = capture.dequeue(std::chrono::seconds(2));
          check_frame(frame, capture.format(), previous_sequence);
          previous_sequence = frame.sequence();
          if (number == 0 || number + 1 == options.frames) {
            log_frame(frame, round + 1, number + 1);
          }
          frame.release();
        }
      }
      const auto fd_after = own_fd_count();
      if (fd_after != *fd_baseline) {
        throw std::runtime_error("file descriptor count grew after HDMI RX reopen cycle");
      }
      const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      std::cout << "round=" << round + 1 << " complete_seconds=" << elapsed << " fd_count=" << fd_after << '\n';
    }
    std::cout << "rkmpp_hdmirx_smoke=PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "rkmpp_hdmirx_smoke=FAIL: " << error.what();
    if (fd_baseline) {
      try {
        const auto fd_after = own_fd_count();
        std::cerr << " fd_baseline=" << *fd_baseline << " fd_after=" << fd_after;
      } catch (...) {
        std::cerr << " fd_after=unavailable";
      }
    }
    std::cerr << '\n';
    return 1;
  }
}
