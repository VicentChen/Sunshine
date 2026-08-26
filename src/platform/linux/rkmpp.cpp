#include "src/platform/linux/rkmpp.h"

#include "src/platform/linux/hdmirx.h"

#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_meta.h>
#include <mpp_packet.h>
#include <rk_mpi.h>
#include <rk_venc_cfg.h>
#include <rk_venc_cmd.h>
#include <rk_venc_rc.h>

#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace platf::rkmpp {
namespace {
constexpr std::size_t output_buffer_size = 8U * 1024U * 1024U;
constexpr auto packet_timeout = std::chrono::seconds(5);

void check(MPP_RET result, const char *operation) {
  if (result != MPP_OK) {
    throw std::runtime_error(std::string(operation) + " failed (MPP_RET=" + std::to_string(result) + ')');
  }
}

MppCodingType coding(codec_e codec) {
  return codec == codec_e::h264 ? MPP_VIDEO_CodingAVC : MPP_VIDEO_CodingHEVC;
}

std::uint32_t horizontal_stride(const hdmirx::capture_format_t &format) {
  if (format.planes.size() != 1 || !format.planes.front().bytesperline) {
    throw std::runtime_error("Stage 3 accepts only a single-plane HDMI RX layout with a byte stride");
  }
  return format.planes.front().bytesperline;
}

struct frame_t {
  MppFrame value {};
  frame_t() { check(mpp_frame_init(&value), "mpp_frame_init"); }
  ~frame_t() { if (value) mpp_frame_deinit(&value); }
};

struct buffer_t {
  MppBuffer value {};
  explicit buffer_t(const hdmirx::frame_plane_t &plane) {
    MppBufferInfo info {};
    info.type = MPP_BUFFER_TYPE_EXT_DMA;
    // EXT_DMA borrows the V4L2-owned descriptor. The capture state closes it
    // only after stream teardown, and this frame stays alive through EOI.
    info.fd = plane.dma_buf_fd;
    info.size = plane.allocation_size;
    check(mpp_buffer_import(&value, &info), "mpp_buffer_import(EXT_DMA)");
  }
  ~buffer_t() { if (value) mpp_buffer_put(value); }
};

struct output_t {
  MppBuffer buffer {};
  MppPacket packet {};
  output_t() {
    check(mpp_buffer_get(nullptr, &buffer, output_buffer_size), "mpp_buffer_get(output)");
    check(mpp_packet_init_with_buffer(&packet, buffer), "mpp_packet_init_with_buffer");
    mpp_packet_set_length(packet, 0);
  }
  ~output_t() {
    if (packet) mpp_packet_deinit(&packet);
    if (buffer) mpp_buffer_put(buffer);
  }
  std::pair<MppPacket, MppBuffer> release() noexcept {
    auto result = std::make_pair(packet, buffer);
    packet = nullptr;
    buffer = nullptr;
    return result;
  }
};

struct packet_t {
  MppPacket value {};
  packet_t() = default;
  ~packet_t() {
    if (value) mpp_packet_deinit(&value);
  }

  packet_t(const packet_t &) = delete;
  packet_t &operator=(const packet_t &) = delete;
};
}  // namespace

struct encoded_packet_t::state_t {
  MppPacket packet {};
  MppBuffer buffer {};
  bool intra {};
  ~state_t() {
    if (packet) mpp_packet_deinit(&packet);
    if (buffer) mpp_buffer_put(buffer);
  }
};

encoded_packet_t::encoded_packet_t() = default;
encoded_packet_t::encoded_packet_t(std::unique_ptr<state_t> state) noexcept : state_(std::move(state)) {}
encoded_packet_t::encoded_packet_t(encoded_packet_t &&) noexcept = default;
encoded_packet_t &encoded_packet_t::operator=(encoded_packet_t &&) noexcept = default;
encoded_packet_t::~encoded_packet_t() = default;
std::uint8_t *encoded_packet_t::data() const noexcept {
  return state_ && state_->packet ? static_cast<std::uint8_t *>(mpp_packet_get_pos(state_->packet)) : nullptr;
}
std::size_t encoded_packet_t::size() const noexcept {
  return state_ && state_->packet ? mpp_packet_get_length(state_->packet) : 0;
}
bool encoded_packet_t::output_intra() const noexcept { return state_ && state_->intra; }
encoded_packet_t::operator bool() const noexcept { return size() != 0; }

std::uint32_t detail::derive_vertical_stride(const hdmirx::capture_format_t &format) {
  if (format.planes.size() != 1) {
    throw std::runtime_error("Stage 3 rejects multi-plane HDMI RX: MppFrame exposes one MppBuffer only");
  }
  const auto &plane = format.planes.front();
  if (!plane.bytesperline || !plane.sizeimage || plane.sizeimage % plane.bytesperline) {
    throw std::runtime_error("HDMI RX sizeimage is not an integral byte-stride layout");
  }
  const auto rows = plane.sizeimage / plane.bytesperline;
  switch (format.mpp_format) {
    case MPP_FMT_BGR888:
    case MPP_FMT_RGB888:
      return rows;
    case MPP_FMT_YUV420SP:
      if (rows % 3U) throw std::runtime_error("NV12 sizeimage is not 3/2 of the luma layout");
      return rows * 2U / 3U;
    case MPP_FMT_YUV422SP:
      if (rows % 2U) throw std::runtime_error("NV16 sizeimage is not twice the luma layout");
      return rows / 2U;
    case MPP_FMT_YUV444SP:
      if (rows % 3U) throw std::runtime_error("NV24 sizeimage is not three times the luma layout");
      return rows / 3U;
    default:
      throw std::runtime_error("unsupported MPP DMA-BUF format");
  }
}

bool detail::is_access_unit_complete(bool partition, bool eoi) noexcept {
  // MPP marks only split packets with SOI/EOI. A non-partition packet already
  // contains the complete access unit; a partitioned unit must wait for EOI.
  return !partition || eoi;
}

struct encoder_t::state_t {
  MppCtx context {};
  MppApi *api {};
  MppEncCfg config {};
  encoder_config_t settings;
  std::uint32_t horizontal_stride {};
  std::uint32_t vertical_stride {};
  encoder_stats_t stats {};

  ~state_t() {
    if (config) mpp_enc_cfg_deinit(config);
    if (context) mpp_destroy(context);
  }
};

encoder_t::encoder_t() = default;
encoder_t::encoder_t(std::unique_ptr<state_t> state) noexcept : state_(std::move(state)) {}
encoder_t::encoder_t(encoder_t &&) noexcept = default;
encoder_t &encoder_t::operator=(encoder_t &&) noexcept = default;
encoder_t::~encoder_t() = default;

encoder_t encoder_t::create(const encoder_config_t &settings) {
  if (!settings.input_format || !settings.fps_num || !settings.fps_den || !settings.gop || !settings.bitrate || !hdmirx::capture_format_is_valid(*settings.input_format)) {
    throw std::invalid_argument("incomplete RKMPP encoder configuration");
  }
  if (settings.bitrate > std::numeric_limits<std::uint32_t>::max() - settings.bitrate / 2U) {
    throw std::invalid_argument("RKMPP bitrate is too high for MPP rate control");
  }
  auto state = std::make_unique<state_t>();
  state->settings = settings;
  state->horizontal_stride = horizontal_stride(*settings.input_format);
  state->vertical_stride = detail::derive_vertical_stride(*settings.input_format);
  if (state->horizontal_stride < settings.input_format->width || state->vertical_stride < settings.input_format->height) {
    throw std::runtime_error("HDMI RX stride is smaller than its visible dimensions");
  }

  check(mpp_create(&state->context, &state->api), "mpp_create");
  RK_S64 timeout_ms = 2000;
  check(state->api->control(state->context, MPP_SET_INPUT_TIMEOUT, &timeout_ms), "MPP_SET_INPUT_TIMEOUT");
  check(state->api->control(state->context, MPP_SET_OUTPUT_TIMEOUT, &timeout_ms), "MPP_SET_OUTPUT_TIMEOUT");
  check(mpp_init(state->context, MPP_CTX_ENC, coding(settings.codec)), "mpp_init");
  check(mpp_enc_cfg_init(&state->config), "mpp_enc_cfg_init");
  const auto set = [&](const char *key, RK_S32 value) { check(mpp_enc_cfg_set_s32(state->config, key, value), key); };
  const auto set_unsigned = [&](const char *key, RK_U32 value) { check(mpp_enc_cfg_set_u32(state->config, key, value), key); };
  set("prep:width", settings.input_format->width);
  set("prep:height", settings.input_format->height);
  set("prep:hor_stride", state->horizontal_stride);
  set("prep:ver_stride", state->vertical_stride);
  set("prep:format", settings.input_format->mpp_format);
  set("rc:mode", MPP_ENC_RC_MODE_CBR);
  set("rc:bps_target", settings.bitrate);
  set("rc:bps_max", settings.bitrate * 3U / 2U);
  set("rc:bps_min", settings.bitrate / 2U);
  set("rc:fps_in_flex", 0);
  set("rc:fps_in_num", settings.fps_num);
  set("rc:fps_in_denom", settings.fps_den);
  set("rc:fps_out_flex", 0);
  set("rc:fps_out_num", settings.fps_num);
  set("rc:fps_out_denom", settings.fps_den);
  set("rc:gop", settings.gop);
  set("codec:type", coding(settings.codec));
  set_unsigned("split:mode", 0);
  set_unsigned("split:out", 0);
  if (settings.codec == codec_e::h264) {
    set("h264:stream_type", 0);
    set("h264:profile", 100);
    set("h264:level", 42);
  } else {
    set("h265:profile", 1);
    set("h265:level", 120);
  }
  check(state->api->control(state->context, MPP_ENC_SET_CFG, state->config), "MPP_ENC_SET_CFG");
  MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
  check(state->api->control(state->context, MPP_ENC_SET_HEADER_MODE, &header_mode), "MPP_ENC_SET_HEADER_MODE");
  return encoder_t(std::move(state));
}

encoded_packet_t encoder_t::encode_packet(hdmirx::captured_frame_t &captured) {
  if (!state_ || captured.released()) throw std::runtime_error("RKMPP encode requires a live HDMI RX frame");
  if (captured.planes().size() != 1 || captured.planes().front().data_offset != 0) {
    throw std::runtime_error("RKMPP rejects multi-plane or non-zero-offset DMA-BUF import");
  }
  const auto &plane = captured.planes().front();
  if (plane.dma_buf_fd < 0 || plane.allocation_size < plane.sizeimage || plane.sizeimage != state_->settings.input_format->planes.front().sizeimage) {
    throw std::runtime_error("HDMI RX DMA-BUF metadata changed after encoder setup");
  }

  try {
    buffer_t input_buffer(plane);
    frame_t input_frame;
    output_t encoded_output;
    mpp_frame_set_width(input_frame.value, state_->settings.input_format->width);
    mpp_frame_set_height(input_frame.value, state_->settings.input_format->height);
    mpp_frame_set_hor_stride(input_frame.value, state_->horizontal_stride);
    if (state_->settings.input_format->mpp_format == MPP_FMT_BGR888 || state_->settings.input_format->mpp_format == MPP_FMT_RGB888) {
      mpp_frame_set_hor_stride_pixel(input_frame.value, state_->horizontal_stride / 3U);
    }
    mpp_frame_set_ver_stride(input_frame.value, state_->vertical_stride);
    mpp_frame_set_fmt(input_frame.value, state_->settings.input_format->mpp_format);
    mpp_frame_set_pts(input_frame.value, captured.timestamp().time_since_epoch().count());
    mpp_frame_set_buf_size(input_frame.value, plane.allocation_size);
    mpp_frame_set_buffer(input_frame.value, input_buffer.value);
    check(mpp_meta_set_packet(mpp_frame_get_meta(input_frame.value), KEY_OUTPUT_PACKET, encoded_output.packet), "mpp_meta_set_packet(KEY_OUTPUT_PACKET)");
    if (!state_->stats.frames) check(state_->api->control(state_->context, MPP_ENC_SET_IDR_FRAME, nullptr), "MPP_ENC_SET_IDR_FRAME");
    check(state_->api->encode_put_frame(state_->context, input_frame.value), "encode_put_frame");

    const auto deadline = std::chrono::steady_clock::now() + packet_timeout;
    for (;;) {
      if (std::chrono::steady_clock::now() >= deadline) {
        throw std::runtime_error("RKMPP timed out waiting for an encoded packet");
      }
      packet_t packet;
      check(state_->api->encode_get_packet(state_->context, &packet.value), "encode_get_packet");
      if (!packet.value) continue;
      const auto length = mpp_packet_get_length(packet.value);
      if (!length) throw std::runtime_error("RKMPP returned an empty packet");
      if (mpp_packet_is_partition(packet.value)) {
        throw std::runtime_error("RKMPP returned a partition packet; split output must produce one complete access unit");
      }
      if (packet.value != encoded_output.packet) {
        throw std::runtime_error("RKMPP returned an unexpected output packet");
      }
      RK_S32 intra = 0;
      if (auto meta = mpp_packet_get_meta(packet.value)) (void) mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra);
      auto state = std::make_unique<encoded_packet_t::state_t>();
      auto [output_packet, output_buffer] = encoded_output.release();
      packet.value = nullptr;
      state->packet = output_packet;
      state->buffer = output_buffer;
      state->intra = intra != 0;
      ++state_->stats.packets;
      state_->stats.bytes += length;
      if (state_->stats.min_packet_bytes == 0 || length < state_->stats.min_packet_bytes) state_->stats.min_packet_bytes = static_cast<std::uint32_t>(length);
      if (length > state_->stats.max_packet_bytes) state_->stats.max_packet_bytes = static_cast<std::uint32_t>(length);
      captured.release();  // MPP has consumed the DMA-BUF before the packet reaches the network thread.
      ++state_->stats.frames;
      return encoded_packet_t(std::move(state));
    }
  } catch (...) {
    if (state_ && state_->api && state_->context) (void) state_->api->reset(state_->context);
    throw;
  }
}

void encoder_t::encode(hdmirx::captured_frame_t &captured, std::ostream &output) {
  auto packet = encode_packet(captured);
  output.write(static_cast<const char *>(static_cast<const void *>(packet.data())), static_cast<std::streamsize>(packet.size()));
  if (!output) throw std::runtime_error("failed to write RKMPP bitstream");
}

void encoder_t::request_idr() {
  if (!state_ || !state_->api || !state_->context) throw std::runtime_error("RKMPP encoder is not initialized");
  check(state_->api->control(state_->context, MPP_ENC_SET_IDR_FRAME, nullptr), "MPP_ENC_SET_IDR_FRAME");
}

std::vector<std::uint8_t> encoder_t::encode_to_vector(hdmirx::captured_frame_t &captured) {
  // Stage 4 deliberately uses a temporary byte vector. Stage 5 owns the
  // MppPacket-to-network zero-copy handoff.
  std::ostringstream output(std::ios::out | std::ios::binary);
  encode(captured, output);
  const auto bytes = output.str();
  return {bytes.begin(), bytes.end()};
}

std::uint64_t encoder_t::encoded_frames() const noexcept { return state_ ? state_->stats.frames : 0; }
encoder_stats_t encoder_t::stats() const noexcept { return state_ ? state_->stats : encoder_stats_t {}; }
bool is_compiled() noexcept { return true; }
}  // namespace platf::rkmpp
