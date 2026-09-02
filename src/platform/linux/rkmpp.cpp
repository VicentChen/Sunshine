#include "src/platform/linux/rkmpp.h"

#include "src/logging.h"
#include "src/platform/hdmirx_policy.h"

#include <chrono>
#include <limits>
#include <mpp_buffer.h>
#include <mpp_frame.h>
#include <mpp_meta.h>
#include <mpp_packet.h>
#include <rk_mpi.h>
#include <rk_venc_cfg.h>
#include <rk_venc_cmd.h>
#include <rk_venc_rc.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace platf::rkmpp {
  void input_frame_t::reset() noexcept {
    *this = {};
  }

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

    struct frame_t {
      MppFrame value {};

      frame_t() {
        check(mpp_frame_init(&value), "mpp_frame_init");
      }

      ~frame_t() {
        if (value) {
          mpp_frame_deinit(&value);
        }
      }
    };

    MppBuffer import_input_buffer(const input_frame_t &frame, video::frame_profile_t *profile = nullptr) {
      MppBuffer buffer {};
      MppBufferInfo info {};
      info.type = MPP_BUFFER_TYPE_EXT_DMA;
      // EXT_DMA borrows the producer descriptor. input_frame_t::holder pins its
      // owner until synchronous MPP consumption completes below.
      info.fd = frame.dma_buf_fd;
      info.size = static_cast<std::size_t>(frame.allocation_size);
      if (profile) {
        profile->mpp_import_begin = std::chrono::steady_clock::now();
      }
      check(mpp_buffer_import(&buffer, &info), "mpp_buffer_import(EXT_DMA)");
      if (profile) {
        profile->mpp_import_end = std::chrono::steady_clock::now();
      }
      return buffer;
    }

    struct buffer_t {
      MppBuffer value {};

      explicit buffer_t(const input_frame_t &frame):
          value(import_input_buffer(frame, frame.profile)) {}

      ~buffer_t() {
        if (value) {
          mpp_buffer_put(value);
        }
      }
    };

    struct output_t {
      MppBuffer buffer {};
      MppPacket packet {};

      explicit output_t(video::frame_profile_t *profile) {
        if (profile) {
          profile->mpp_output_buffer_begin = std::chrono::steady_clock::now();
        }
        check(mpp_buffer_get(nullptr, &buffer, output_buffer_size), "mpp_buffer_get(output)");
        if (profile) {
          profile->mpp_output_buffer_end = std::chrono::steady_clock::now();
          profile->mpp_output_packet_begin = std::chrono::steady_clock::now();
        }
        check(mpp_packet_init_with_buffer(&packet, buffer), "mpp_packet_init_with_buffer");
        if (profile) {
          profile->mpp_output_packet_end = std::chrono::steady_clock::now();
        }
        mpp_packet_set_length(packet, 0);
      }

      ~output_t() {
        if (packet) {
          mpp_packet_deinit(&packet);
        }
        if (buffer) {
          mpp_buffer_put(buffer);
        }
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
        if (value) {
          mpp_packet_deinit(&value);
        }
      }

      packet_t(const packet_t &) = delete;
      packet_t &operator=(const packet_t &) = delete;
    };
  }  // namespace

  std::uint64_t detail::minimum_allocation_size(const input_layout_t &layout) noexcept {
    const auto stride = static_cast<std::uint64_t>(layout.horizontal_stride);
    const auto rows = static_cast<std::uint64_t>(layout.vertical_stride);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (rows != 0 && stride > maximum / rows) {
      return 0;
    }
    const auto luma_size = stride * rows;
    switch (layout.format) {
      case MPP_FMT_BGR888:
      case MPP_FMT_RGB888:
        return layout.horizontal_stride % 3U == 0 ? luma_size : 0;
      case MPP_FMT_YUV420SP:
        if ((layout.visible_width & 1U) != 0 || (layout.visible_height & 1U) != 0 || (layout.horizontal_stride & 1U) != 0 || (layout.vertical_stride & 1U) != 0) {
          return 0;
        }
        return luma_size <= maximum - luma_size / 2U ? luma_size + luma_size / 2U : 0;
      case MPP_FMT_YUV422SP:
        if ((layout.visible_width & 1U) != 0 || (layout.horizontal_stride & 1U) != 0) {
          return 0;
        }
        return luma_size <= maximum / 2U ? luma_size * 2U : 0;
      case MPP_FMT_YUV444SP:
        return luma_size <= maximum / 3U ? luma_size * 3U : 0;
      default:
        return 0;
    }
  }

  std::optional<input_layout_t> make_input_layout_from_plane(std::uint32_t visible_width, std::uint32_t visible_height, MppFrameFormat format, std::uint32_t horizontal_stride, std::uint64_t plane_extent) noexcept {
    if (horizontal_stride == 0 || plane_extent == 0 || plane_extent % horizontal_stride != 0) {
      return std::nullopt;
    }
    const auto rows = plane_extent / horizontal_stride;
    if (rows > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    std::uint32_t vertical_stride {};
    switch (format) {
      case MPP_FMT_BGR888:
      case MPP_FMT_RGB888:
        if (horizontal_stride % 3U != 0) {
          return std::nullopt;
        }
        vertical_stride = static_cast<std::uint32_t>(rows);
        break;
      case MPP_FMT_YUV420SP:
        if (rows % 3U != 0) {
          return std::nullopt;
        }
        vertical_stride = static_cast<std::uint32_t>(rows / 3U * 2U);
        break;
      case MPP_FMT_YUV422SP:
        if (rows % 2U != 0) {
          return std::nullopt;
        }
        vertical_stride = static_cast<std::uint32_t>(rows / 2U);
        break;
      case MPP_FMT_YUV444SP:
        if (rows % 3U != 0) {
          return std::nullopt;
        }
        vertical_stride = static_cast<std::uint32_t>(rows / 3U);
        break;
      default:
        return std::nullopt;
    }
    input_layout_t layout {visible_width, visible_height, horizontal_stride, vertical_stride, format};
    return validate_input_layout(layout) == input_status_e::ok ? std::optional<input_layout_t> {layout} : std::nullopt;
  }

  input_status_e validate_input_layout(const input_layout_t &layout) noexcept {
    if (!hdmirx::is_valid_resolution({layout.visible_width, layout.visible_height})) {
      return input_status_e::invalid_visible_size;
    }
    const auto bytes_per_pixel = layout.format == MPP_FMT_BGR888 || layout.format == MPP_FMT_RGB888 ? 3U : 1U;
    switch (layout.format) {
      case MPP_FMT_BGR888:
      case MPP_FMT_RGB888:
      case MPP_FMT_YUV420SP:
      case MPP_FMT_YUV422SP:
      case MPP_FMT_YUV444SP:
        break;
      default:
        return input_status_e::unsupported_format;
    }
    const auto allocation_size = detail::minimum_allocation_size(layout);
    if (layout.visible_width > std::numeric_limits<std::uint32_t>::max() / bytes_per_pixel || layout.horizontal_stride < layout.visible_width * bytes_per_pixel || layout.vertical_stride < layout.visible_height || layout.horizontal_stride > std::numeric_limits<RK_S32>::max() || layout.vertical_stride > std::numeric_limits<RK_S32>::max() || allocation_size == 0) {
      return input_status_e::stride_too_small;
    }
    return input_status_e::ok;
  }

  input_status_e validate_input_frame(const input_frame_t &frame, const input_layout_t &expected_layout) noexcept {
    if (const auto status = validate_input_layout(frame.layout); status != input_status_e::ok) {
      return status;
    }
    if (frame.layout != expected_layout) {
      return input_status_e::layout_mismatch;
    }
    if (frame.dma_buf_fd < 0) {
      return input_status_e::invalid_dma_buf;
    }
    if (!frame.holder) {
      return input_status_e::missing_holder;
    }
    if (frame.allocation_size > std::numeric_limits<std::size_t>::max()) {
      return input_status_e::allocation_not_representable;
    }
    if (frame.allocation_size < detail::minimum_allocation_size(frame.layout)) {
      return input_status_e::allocation_too_small;
    }
    return input_status_e::ok;
  }

  encoder_config_status_e validate_encoder_config(const encoder_config_t &settings) noexcept {
    if (settings.codec != codec_e::h264 && settings.codec != codec_e::h265) {
      return encoder_config_status_e::invalid_codec;
    }
    if (validate_input_layout(settings.input_layout) != input_status_e::ok) {
      return encoder_config_status_e::invalid_input;
    }
    if (!hdmirx::is_valid_resolution({settings.coded_width, settings.coded_height}) || (settings.coded_width & 1U) != 0 || (settings.coded_height & 1U) != 0) {
      return encoder_config_status_e::invalid_coded_size;
    }
    if (settings.input_layout.visible_width != settings.coded_width || settings.input_layout.visible_height != settings.coded_height) {
      return encoder_config_status_e::converter_required;
    }
    if (!settings.fps_num || !settings.fps_den || !settings.gop || !settings.bitrate || settings.bitrate > std::numeric_limits<std::uint32_t>::max() - settings.bitrate / 2U) {
      return encoder_config_status_e::invalid_rate_control;
    }
    return encoder_config_status_e::ok;
  }

  struct encoded_packet_t::state_t {
    MppPacket packet {};
    MppBuffer buffer {};
    bool intra {};

    ~state_t() {
      if (packet) {
        mpp_packet_deinit(&packet);
      }
      if (buffer) {
        mpp_buffer_put(buffer);
      }
    }
  };

  encoded_packet_t::encoded_packet_t() = default;

  encoded_packet_t::encoded_packet_t(std::unique_ptr<state_t> state) noexcept:
      state_(std::move(state)) {}

  encoded_packet_t::encoded_packet_t(encoded_packet_t &&) noexcept = default;
  encoded_packet_t &encoded_packet_t::operator=(encoded_packet_t &&) noexcept = default;
  encoded_packet_t::~encoded_packet_t() = default;

  std::uint8_t *encoded_packet_t::data() const noexcept {
    return state_ && state_->packet ? static_cast<std::uint8_t *>(mpp_packet_get_pos(state_->packet)) : nullptr;
  }

  std::size_t encoded_packet_t::size() const noexcept {
    return state_ && state_->packet ? mpp_packet_get_length(state_->packet) : 0;
  }

  bool encoded_packet_t::output_intra() const noexcept {
    return state_ && state_->intra;
  }

  encoded_packet_t::operator bool() const noexcept {
    return size() != 0;
  }

  bool detail::is_access_unit_complete(bool partition, bool eoi) noexcept {
    // MPP marks only split packets with SOI/EOI. A non-partition packet already
    // contains the complete access unit; a partitioned unit must wait for EOI.
    return !partition || eoi;
  }

  struct encoder_t::state_t {
    /** @brief One cached MPP import for a producer allocation identity. */
    struct cached_input_buffer_t {
      input_buffer_key_t key;
      int dma_buf_fd {};
      std::uint64_t allocation_size {};
      MppBuffer buffer {};
    };

    MppCtx context {};
    MppApi *api {};
    MppEncCfg config {};
    encoder_config_t settings;
    std::uint32_t horizontal_stride {};
    std::uint32_t vertical_stride {};
    encoder_stats_t stats {};
    std::vector<cached_input_buffer_t> input_buffers;

    /**
     * @brief Return a cached DMA-BUF import, importing this producer slot once.
     *
     * @param frame Valid input frame with an optional stable producer identity.
     * @return Cached MPP buffer, or null when the input intentionally bypasses the cache.
     */
    MppBuffer input_buffer_for(const input_frame_t &frame) {
      if (!frame.cache_key) {
        return nullptr;
      }
      for (const auto &cached : input_buffers) {
        if (cached.key != *frame.cache_key) {
          continue;
        }
        if (cached.dma_buf_fd != frame.dma_buf_fd || cached.allocation_size != frame.allocation_size) {
          throw std::runtime_error("RKMPP input cache identity changed its DMA-BUF descriptor or allocation size");
        }
        return cached.buffer;
      }
      auto buffer = import_input_buffer(frame, frame.profile);
      try {
        input_buffers.push_back({*frame.cache_key, frame.dma_buf_fd, frame.allocation_size, buffer});
      } catch (...) {
        mpp_buffer_put(buffer);
        throw;
      }
      return buffer;
    }

    /** @brief Release all cached external DMA-BUF imports. */
    void clear_input_buffers() noexcept {
      for (auto &cached : input_buffers) {
        if (cached.buffer) {
          mpp_buffer_put(cached.buffer);
        }
      }
      input_buffers.clear();
    }

    ~state_t() {
      clear_input_buffers();
      if (config) {
        mpp_enc_cfg_deinit(config);
      }
      if (context) {
        mpp_destroy(context);
      }
    }
  };

  encoder_t::encoder_t() = default;

  encoder_t::encoder_t(std::unique_ptr<state_t> state) noexcept:
      state_(std::move(state)) {}

  encoder_t::encoder_t(encoder_t &&) noexcept = default;
  encoder_t &encoder_t::operator=(encoder_t &&) noexcept = default;
  encoder_t::~encoder_t() = default;

  encoder_t encoder_t::create(const encoder_config_t &settings) {
    switch (validate_encoder_config(settings)) {
      case encoder_config_status_e::ok:
        break;
      case encoder_config_status_e::converter_required:
        throw std::runtime_error("RKMPP input conversion required: input layout does not match requested coded size");
      case encoder_config_status_e::invalid_input:
      case encoder_config_status_e::invalid_coded_size:
      case encoder_config_status_e::invalid_rate_control:
      case encoder_config_status_e::invalid_codec:
        throw std::invalid_argument("incomplete RKMPP encoder configuration");
    }
    auto state = std::make_unique<state_t>();
    state->settings = settings;
    state->horizontal_stride = settings.input_layout.horizontal_stride;
    state->vertical_stride = settings.input_layout.vertical_stride;

    check(mpp_create(&state->context, &state->api), "mpp_create");
    RK_S64 timeout_ms = 2000;
    check(state->api->control(state->context, MPP_SET_INPUT_TIMEOUT, &timeout_ms), "MPP_SET_INPUT_TIMEOUT");
    check(state->api->control(state->context, MPP_SET_OUTPUT_TIMEOUT, &timeout_ms), "MPP_SET_OUTPUT_TIMEOUT");
    check(mpp_init(state->context, MPP_CTX_ENC, coding(settings.codec)), "mpp_init");
    check(mpp_enc_cfg_init(&state->config), "mpp_enc_cfg_init");
    const auto set = [&](const char *key, RK_S32 value) {
      check(mpp_enc_cfg_set_s32(state->config, key, value), key);
    };
    const auto set_unsigned = [&](const char *key, RK_U32 value) {
      check(mpp_enc_cfg_set_u32(state->config, key, value), key);
    };
    set("prep:width", settings.input_layout.visible_width);
    set("prep:height", settings.input_layout.visible_height);
    set("prep:hor_stride", state->horizontal_stride);
    set("prep:ver_stride", state->vertical_stride);
    set("prep:format", settings.input_layout.format);
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
    if (settings.low_delay) {
      set("base:low_delay", 1);
    }
    if (settings.disable_reencode) {
      set("rc:max_reenc_times", 0);
    }
    check(state->api->control(state->context, MPP_ENC_SET_CFG, state->config), "MPP_ENC_SET_CFG");
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    check(state->api->control(state->context, MPP_ENC_SET_HEADER_MODE, &header_mode), "MPP_ENC_SET_HEADER_MODE");

    BOOST_LOG(info) << "RKMPP encoder created: codec=" << (settings.codec == codec_e::h264 ? "H.264" : "HEVC")
                    << " dimensions=" << settings.coded_width << "x" << settings.coded_height
                    << " fps=" << settings.fps_num << "/" << settings.fps_den
                    << " bitrate=" << settings.bitrate << " gop=" << settings.gop
                    << " low_delay=" << settings.low_delay << " max_reenc_times=" << (settings.disable_reencode ? 0 : -1)
                    << " split:mode=0 split:out=0";

    return encoder_t(std::move(state));
  }

  encoded_packet_t encoder_t::encode_packet(const input_frame_t &input) {
    if (!state_) {
      throw std::runtime_error("RKMPP encoder is not initialized");
    }
    if (const auto status = validate_input_frame(input, state_->settings.input_layout); status != input_status_e::ok) {
      throw std::runtime_error("RKMPP input frame is invalid (status=" + std::to_string(static_cast<int>(status)) + ')');
    }
    // Preserve the producer lease even if the caller releases its input frame
    // object while MPP synchronously processes the borrowed descriptor.
    auto holder = input.holder;

    try {
      std::optional<buffer_t> transient_input_buffer;
      auto input_buffer = state_->input_buffer_for(input);
      if (!input_buffer) {
        transient_input_buffer.emplace(input);
        input_buffer = transient_input_buffer->value;
      }
      frame_t input_frame;
      output_t encoded_output(input.profile);
      mpp_frame_set_width(input_frame.value, input.layout.visible_width);
      mpp_frame_set_height(input_frame.value, input.layout.visible_height);
      mpp_frame_set_hor_stride(input_frame.value, state_->horizontal_stride);
      if (input.layout.format == MPP_FMT_BGR888 || input.layout.format == MPP_FMT_RGB888) {
        mpp_frame_set_hor_stride_pixel(input_frame.value, state_->horizontal_stride / 3U);
      }
      mpp_frame_set_ver_stride(input_frame.value, state_->vertical_stride);
      mpp_frame_set_fmt(input_frame.value, input.layout.format);
      mpp_frame_set_pts(input_frame.value, input.pts);
      mpp_frame_set_buf_size(input_frame.value, static_cast<std::size_t>(input.allocation_size));
      mpp_frame_set_buffer(input_frame.value, input_buffer);
      const auto frame_meta = mpp_frame_get_meta(input_frame.value);
      check(mpp_meta_set_packet(frame_meta, KEY_OUTPUT_PACKET, encoded_output.packet), "mpp_meta_set_packet(KEY_OUTPUT_PACKET)");
      if (!state_->stats.frames) {
        check(state_->api->control(state_->context, MPP_ENC_SET_IDR_FRAME, nullptr), "MPP_ENC_SET_IDR_FRAME");
      }
      if (input.profile) {
        input.profile->mpp_submit_begin = std::chrono::steady_clock::now();
      }
      check(state_->api->encode_put_frame(state_->context, input_frame.value), "encode_put_frame");
      if (input.profile) {
        input.profile->mpp_submit_end = std::chrono::steady_clock::now();
      }

      const auto deadline = std::chrono::steady_clock::now() + packet_timeout;
      for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
          throw std::runtime_error("RKMPP timed out waiting for an encoded packet");
        }
        packet_t packet;
        check(state_->api->encode_get_packet(state_->context, &packet.value), "encode_get_packet");
        if (!packet.value) {
          continue;
        }
        const auto length = mpp_packet_get_length(packet.value);
        if (!length) {
          throw std::runtime_error("RKMPP returned an empty packet");
        }
        if (mpp_packet_is_partition(packet.value)) {
          throw std::runtime_error("RKMPP returned a partition packet; split output must produce one complete access unit");
        }
        if (packet.value != encoded_output.packet) {
          throw std::runtime_error("RKMPP returned an unexpected output packet");
        }
        if (input.profile) {
          input.profile->mpp_output = std::chrono::steady_clock::now();
        }
        RK_S32 intra = 0;
        if (auto meta = mpp_packet_get_meta(packet.value)) {
          (void) mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra);
        }
        auto state = std::make_unique<encoded_packet_t::state_t>();
        auto [output_packet, output_buffer] = encoded_output.release();
        packet.value = nullptr;
        state->packet = output_packet;
        state->buffer = output_buffer;
        state->intra = intra != 0;
        ++state_->stats.packets;
        state_->stats.bytes += length;
        if (state_->stats.min_packet_bytes == 0 || length < state_->stats.min_packet_bytes) {
          state_->stats.min_packet_bytes = static_cast<std::uint32_t>(length);
        }
        if (length > state_->stats.max_packet_bytes) {
          state_->stats.max_packet_bytes = static_cast<std::uint32_t>(length);
        }
        holder.reset();  // MPP has synchronously consumed the DMA-BUF before the packet reaches the network thread.
        ++state_->stats.frames;
        return encoded_packet_t(std::move(state));
      }
    } catch (...) {
      if (state_ && state_->api && state_->context) {
        (void) state_->api->reset(state_->context);
      }
      throw;
    }
  }

  void encoder_t::encode(const input_frame_t &input, std::ostream &output) {
    auto packet = encode_packet(input);
    output.write(static_cast<const char *>(static_cast<const void *>(packet.data())), static_cast<std::streamsize>(packet.size()));
    if (!output) {
      throw std::runtime_error("failed to write RKMPP bitstream");
    }
  }

  void encoder_t::request_idr() {
    if (!state_ || !state_->api || !state_->context) {
      throw std::runtime_error("RKMPP encoder is not initialized");
    }
    check(state_->api->control(state_->context, MPP_ENC_SET_IDR_FRAME, nullptr), "MPP_ENC_SET_IDR_FRAME");
  }

  void encoder_t::clear_input_cache() noexcept {
    if (state_) {
      state_->clear_input_buffers();
    }
  }

  std::vector<std::uint8_t> encoder_t::encode_to_vector(const input_frame_t &input) {
    // Stage 4 deliberately uses a temporary byte vector. Stage 5 owns the
    // MppPacket-to-network zero-copy handoff.
    std::ostringstream output(std::ios::out | std::ios::binary);
    encode(input, output);
    const auto bytes = output.str();
    return {bytes.begin(), bytes.end()};
  }

  std::uint64_t encoder_t::encoded_frames() const noexcept {
    return state_ ? state_->stats.frames : 0;
  }

  encoder_stats_t encoder_t::stats() const noexcept {
    return state_ ? state_->stats : encoder_stats_t {};
  }

  bool is_compiled() noexcept {
    return true;
  }
}  // namespace platf::rkmpp
