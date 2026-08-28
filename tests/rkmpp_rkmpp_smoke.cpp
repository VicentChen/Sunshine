/** @file tests/rkmpp_rkmpp_smoke.cpp */
#include <dirent.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <src/platform/linux/hdmirx.h>
#include <src/platform/linux/rkmpp.h>

// The standalone smoke only needs the logging globals referenced by the HDMI
// capture implementation. Keep it independent of Sunshine's full application
// configuration and its unrelated runtime dependencies.
boost::log::sources::severity_logger<int> debug(1);
boost::log::sources::severity_logger<int> error(1);
boost::log::sources::severity_logger<int> info(1);
namespace config {
  sunshine_t sunshine {};
  video_t video {};
}

namespace {
struct options_t {
  platf::rkmpp::codec_e codec {};
  std::uint32_t frames {120};
  std::uint32_t sessions {2};
  std::string output;
  bool osd {};
};

std::size_t fd_count() {
  DIR *directory = opendir("/proc/self/fd");
  if (!directory) throw std::runtime_error("cannot inspect /proc/self/fd");
  const auto directory_fd = dirfd(directory);
  std::size_t count = 0;
  while (const auto *entry = readdir(directory)) {
    if (entry->d_name[0] != '.' && std::string(entry->d_name) != std::to_string(directory_fd)) ++count;
  }
  closedir(directory);
  return count;
}

std::string fd_targets() {
  DIR *directory = opendir("/proc/self/fd");
  if (!directory) throw std::runtime_error("cannot inspect /proc/self/fd");
  const auto directory_fd = dirfd(directory);
  std::string result;
  while (const auto *entry = readdir(directory)) {
    if (entry->d_name[0] == '.' || std::string(entry->d_name) == std::to_string(directory_fd)) continue;
    const std::string path = std::string("/proc/self/fd/") + entry->d_name;
    char target[PATH_MAX] {};
    const auto length = readlink(path.c_str(), target, sizeof(target) - 1);
    if (length > 0) {
      target[length] = '\0';
      result += std::string(entry->d_name) + '=' + target + ';';
    }
  }
  closedir(directory);
  return result;
}

options_t parse(int argc, char **argv) {
  options_t options;
  bool codec_set = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--codec" && index + 1 < argc) {
      const std::string value = argv[++index];
      if (value == "h264") options.codec = platf::rkmpp::codec_e::h264;
      else if (value == "h265") options.codec = platf::rkmpp::codec_e::h265;
      else throw std::invalid_argument("codec must be h264 or h265");
      codec_set = true;
    } else if ((argument == "--frames" || argument == "--sessions") && index + 1 < argc) {
      const auto value = std::strtoul(argv[++index], nullptr, 10);
      if (!value || value > UINT32_MAX) throw std::invalid_argument(argument + " must be positive");
      if (argument == "--frames") options.frames = static_cast<std::uint32_t>(value);
      else options.sessions = static_cast<std::uint32_t>(value);
    } else if (argument == "--output" && index + 1 < argc) {
      options.output = argv[++index];
    } else if (argument == "--osd") {
      options.osd = true;
    } else {
      throw std::invalid_argument("usage: rkmpp_rkmpp_smoke --codec h264|h265 [--frames 120] [--sessions 2] [--output /tmp/out.h264] [--osd]");
    }
  }
  if (!codec_set) throw std::invalid_argument("--codec is required");
  if (options.output.empty()) options.output = options.codec == platf::rkmpp::codec_e::h264 ? "/tmp/rkmpp.h264" : "/tmp/rkmpp.h265";
  return options;
}

bool annexb_has_required_nals(const std::string &path, platf::rkmpp::codec_e codec) {
  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
  bool first = false, second = false, third = false, idr = false;
  for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
    std::size_t payload = index;
    if (bytes[payload] == 0 && bytes[payload + 1] == 0 && bytes[payload + 2] == 1) payload += 3;
    else if (bytes[payload] == 0 && bytes[payload + 1] == 0 && bytes[payload + 2] == 0 && bytes[payload + 3] == 1) payload += 4;
    else continue;
    if (payload >= bytes.size()) continue;
    if (codec == platf::rkmpp::codec_e::h264) {
      const auto type = bytes[payload] & 0x1f;
      first |= type == 7; second |= type == 8; idr |= type == 5;
    } else {
      const auto type = (bytes[payload] >> 1) & 0x3f;
      first |= type == 32; second |= type == 33; third |= type == 34; idr |= type == 19 || type == 20;
    }
  }
  return codec == platf::rkmpp::codec_e::h264 ? first && second && idr : first && second && third && idr;
}

std::vector<std::vector<unsigned char>> annexb_nals(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
  std::vector<std::size_t> starts;
  for (std::size_t index = 0; index + 3 < bytes.size(); ++index) {
    if (bytes[index] == 0 && bytes[index + 1] == 0 && (bytes[index + 2] == 1 || (bytes[index + 2] == 0 && bytes[index + 3] == 1))) starts.push_back(index);
  }
  std::vector<std::vector<unsigned char>> nals;
  for (std::size_t index = 0; index < starts.size(); ++index) {
    const auto start = starts[index] + (bytes[starts[index] + 2] == 1 ? 3 : 4);
    const auto end = index + 1 < starts.size() ? starts[index + 1] : bytes.size();
    if (start < end) nals.emplace_back(bytes.begin() + start, bytes.begin() + end);
  }
  return nals;
}

bool first_key_unit_has_required_nals(const std::string &path, platf::rkmpp::codec_e codec) {
  bool first = false, second = false, third = false;
  for (const auto &nal : annexb_nals(path)) {
    if (codec == platf::rkmpp::codec_e::h264) {
      const auto type = nal.front() & 0x1f;
      first |= type == 7; second |= type == 8;
      if (type == 5) return first && second;
      if (type == 1) return false;
    } else {
      const auto type = (nal.front() >> 1) & 0x3f;
      first |= type == 32; second |= type == 33; third |= type == 34;
      if (type == 19 || type == 20) return first && second && third;
      if (type <= 31) return false;
    }
  }
  return false;
}

bool h264_first_mb_is_zero(const std::vector<unsigned char> &nal) {
  std::vector<unsigned char> rbsp;
  for (std::size_t index = 1; index < nal.size(); ++index) {
    if (index + 2 < nal.size() && nal[index] == 0 && nal[index + 1] == 0 && nal[index + 2] == 3) {
      rbsp.push_back(0); rbsp.push_back(0); index += 2;
    } else rbsp.push_back(nal[index]);
  }
  std::size_t bit = 0, leading_zeroes = 0;
  while (bit < rbsp.size() * 8 && !(rbsp[bit / 8] & (0x80 >> (bit % 8)))) { ++leading_zeroes; ++bit; }
  if (bit >= rbsp.size() * 8) return false;
  ++bit;
  std::uint32_t suffix = 0;
  for (std::size_t index = 0; index < leading_zeroes; ++index, ++bit) {
    if (bit >= rbsp.size() * 8) return false;
    suffix = (suffix << 1) | !!(rbsp[bit / 8] & (0x80 >> (bit % 8)));
  }
  return ((1U << leading_zeroes) - 1U + suffix) == 0;
}

std::size_t access_unit_count(const std::string &path, platf::rkmpp::codec_e codec) {
  std::size_t count = 0;
  for (const auto &nal : annexb_nals(path)) {
    if (codec == platf::rkmpp::codec_e::h264) {
      const auto type = nal.front() & 0x1f;
      if ((type == 1 || type == 5) && h264_first_mb_is_zero(nal)) ++count;
    } else if (nal.size() > 2 && ((nal.front() >> 1) & 0x3f) <= 31 && (nal[2] & 0x80)) {
      ++count;
    }
  }
  return count;
}

struct bitstream_stats_t {
  std::size_t access_units {};
  std::size_t idr {};
  std::size_t parameter_sets {};
};

bitstream_stats_t bitstream_stats(const std::string &path, platf::rkmpp::codec_e codec) {
  bitstream_stats_t stats;
  for (const auto &nal : annexb_nals(path)) {
    if (codec == platf::rkmpp::codec_e::h264) {
      const auto type = nal.front() & 0x1f;
      stats.parameter_sets += type == 7 || type == 8;
      stats.idr += type == 5;
    } else {
      const auto type = (nal.front() >> 1) & 0x3f;
      stats.parameter_sets += type == 32 || type == 33 || type == 34;
      stats.idr += type == 19 || type == 20;
    }
  }
  stats.access_units = access_unit_count(path, codec);
  return stats;
}

std::size_t run_session(const options_t &options, std::uint32_t session, std::string output_path) {
  {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open output");
    auto capture = platf::hdmirx::hdmirx_capture_t::open();
    const auto &format = capture.format();
    std::cout << "session=" << session << " input=" << format.width << 'x' << format.height << " fourcc=" << format.fourcc << " planes=" << format.planes.size() << " bpl=" << format.planes.front().bytesperline << " sizeimage=" << format.planes.front().sizeimage << '\n';
    const auto input_layout = platf::rkmpp::make_input_layout_from_plane(format.width, format.height, format.mpp_format, format.planes.front().bytesperline, format.planes.front().sizeimage);
    if (!input_layout.has_value()) throw std::runtime_error("HDMI RX format has no valid RKMPP direct layout");
    const platf::rkmpp::encoder_config_t config {
      options.codec, *input_layout, format.width, format.height, 60, 1, 12'000'000, 60,
    };
    if (config.fps_num != 60 || config.fps_den != 1 || config.bitrate != 12'000'000 || config.gop != 60) {
      throw std::runtime_error("RKMPP smoke encoder configuration is not 60/1 fps, 12 Mbps, GOP 60");
    }
    std::cout << "session=" << session << " encoder_fps=" << config.fps_num << '/' << config.fps_den
              << " encoder_bitrate=" << config.bitrate << " encoder_gop=" << config.gop << '\n';
    auto encoder = platf::rkmpp::encoder_t::create(config);
    platf::rkmpp::frame_profile_overlay_bitmap_t osd_bitmap;
    if (options.osd) {
      video::frame_profile_snapshot_t snapshot;
      snapshot.captured_frames = options.frames;
      snapshot.rga_bypass_frames = options.frames;
      for (std::size_t index = 0; index < snapshot.metrics.size(); ++index) {
        auto &metric = snapshot.metrics[index];
        metric.count = options.frames;
        metric.p50_us = 500 + static_cast<std::int64_t>(index) * 100;
        metric.p95_us = 900 + static_cast<std::int64_t>(index) * 200;
        metric.p99_us = 1200 + static_cast<std::int64_t>(index) * 300;
      }
      osd_bitmap.render(snapshot);
      encoder.set_osd_region({16, 16, platf::rkmpp::frame_profile_overlay_bitmap_t::width,
                              platf::rkmpp::frame_profile_overlay_bitmap_t::height, osd_bitmap.pixels()});
      std::cout << "session=" << session << " osd=enabled region=16,16,640,160 content=profile-text\n";
    }
    const auto live_baseline = fd_count();
    for (std::uint32_t frame_index = 0; frame_index < options.frames; ++frame_index) {
      auto holder = std::make_shared<platf::hdmirx::captured_frame_t>(capture.dequeue(std::chrono::seconds(2)));
      const auto &plane = holder->planes().front();
      platf::rkmpp::input_frame_t input {
        *input_layout,
        plane.dma_buf_fd,
        plane.allocation_size,
        holder->timestamp().time_since_epoch().count(),
        holder,
      };
      encoder.encode(input, output);
      input.holder.reset();
      holder.reset();
    }
    if (fd_count() != live_baseline) throw std::runtime_error("fd count changed while encoding");
    output.close();
    const auto encoder_stats = encoder.stats();
    if (encoder_stats.frames != options.frames) throw std::runtime_error("encoded frame count mismatch");
    if (!annexb_has_required_nals(output_path, options.codec)) throw std::runtime_error("stream lacks required parameter sets or IDR");
    if (!first_key_unit_has_required_nals(output_path, options.codec)) throw std::runtime_error("first key unit lacks required parameter sets");
    const auto stream_stats = bitstream_stats(output_path, options.codec);
    if (stream_stats.access_units != options.frames) throw std::runtime_error("bitstream access-unit count mismatch");
    std::cout << "session=" << session << " packet_count=" << encoder_stats.packets << " packet_bytes_min=" << encoder_stats.min_packet_bytes << " packet_bytes_max=" << encoder_stats.max_packet_bytes << " packet_bytes_avg=" << encoder_stats.bytes / encoder_stats.packets << " access_units=" << stream_stats.access_units << " idr=" << stream_stats.idr << " parameter_sets=" << stream_stats.parameter_sets << '\n';
    capture.shutdown();
  }
  return fd_count();
}
}  // namespace

int main(int argc, char **argv) {
  try {
    const auto options = parse(argc, argv);
    const auto raw_process_fd = fd_count();
    const auto raw_process_fd_targets = fd_targets();
    auto warmup_options = options;
    warmup_options.frames = 1;
    const auto warmup_path = options.output + ".warmup";
    const auto warmup_fd = run_session(warmup_options, 0, warmup_path);
    const auto steady_process_fd = fd_count();
    const auto steady_process_fd_targets = fd_targets();
    if (warmup_fd != steady_process_fd) throw std::runtime_error("fd count changed after warmup teardown");
    std::size_t previous_session_fds = steady_process_fd;
    for (std::uint32_t session = 1; session <= options.sessions; ++session) {
      const auto path = session == 1 ? options.output : options.output + ".session" + std::to_string(session);
      const auto after_session_fds = run_session(options, session, path);
      std::cout << "session=" << session << " fd_after=" << after_session_fds << " fd_targets=" << fd_targets() << '\n';
      if (session > 1 && after_session_fds != previous_session_fds) throw std::runtime_error("fd count grew across complete encoder sessions");
      previous_session_fds = after_session_fds;
    }
    const auto final_process_fd = fd_count();
    if (final_process_fd != steady_process_fd) throw std::runtime_error("fd count changed after all session teardown");
    std::cout << "rkmpp_rkmpp_smoke=PASS frames=" << options.frames << " sessions=" << options.sessions << " output=" << options.output << " raw_fd=" << raw_process_fd << " raw_fd_targets=" << raw_process_fd_targets << " steady_fd=" << steady_process_fd << " steady_fd_targets=" << steady_process_fd_targets << " final_fd=" << final_process_fd << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "rkmpp_rkmpp_smoke=FAIL: " << error.what() << " fd_now=" << fd_count() << '\n';
    return 1;
  }
}
