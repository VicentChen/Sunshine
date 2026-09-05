/**
 * @file src/video.cpp
 * @brief Definitions for video.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <limits>
#include <list>
#include <thread>
#include <utility>

// lib includes
#include <boost/pointer_cast.hpp>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#if !defined(_WIN32) && !defined(__APPLE__)
  #include <ffnvcodec/nvEncodeAPI.h>
#endif
}

// local includes
#include "cbs.h"
#include "config.h"
#include "display_device.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "nvenc/nvenc_encoder.h"
#include "platform/common.h"
#include "sync.h"
#include "video.h"
#ifdef SUNSHINE_BUILD_RKMPP
  #include "platform/hdmirx_policy.h"
  #include "platform/linux/hdmirx.h"
  #include "platform/linux/rga.h"
  #include "platform/linux/rkmpp.h"
  #include "platform/linux/rkmpp_preprocess.h"
  #include "platform/linux/ui_controller.h"
  #ifdef SUNSHINE_BUILD_VULKAN
    #include "platform/linux/vulkan_ui.h"
    #include "platform/linux/vulkan_ui_surface.h"
  #endif
#endif

#ifdef _WIN32
extern "C" {
  #include <libavutil/hwcontext_d3d11va.h>
}
#endif

using namespace std::literals;

namespace video {

  namespace {
    /**
     * @brief Check if we can allow probing for the encoders.
     * @return True if there should be no issues with the probing, false if we should prevent it.
     */
    bool allow_encoder_probing() {
      const auto devices {display_device::enumerate_devices()};

      // If there are no devices, then either the API is not working correctly or OS does not support the lib.
      // Either way we should not block the probing in this case as we can't tell what's wrong.
      if (devices.empty()) {
        return true;
      }

      // Since Windows 11 24H2, it is possible that there will be no active devices present
      // for some reason (probably a bug). Trying to probe encoders in such a state locks/breaks the DXGI
      // and also the display device for Windows. So we must have at least 1 active device.
      const bool at_least_one_device_is_active = std::any_of(std::begin(devices), std::end(devices), [](const auto &device) {
        // If device has additional info, it is active.
        return static_cast<bool>(device.m_info);
      });

      if (at_least_one_device_is_active) {
        return true;
      }

      BOOST_LOG(error) << "No display devices are active at the moment! Cannot probe the encoders.";
      return false;
    }
  }  // namespace

  /**
   * @brief Release context resources.
   */
#ifdef SUNSHINE_BUILD_RKMPP
  class packet_raw_rkmpp final: public packet_raw_t {
  public:
    packet_raw_rkmpp(platf::rkmpp::encoded_packet_t &&packet, int64_t frame_index, bool idr):
        packet_ {std::move(packet)},
        frame_index_ {frame_index},
        idr_ {idr} {}

    bool is_idr() override {
      return idr_;
    }

    int64_t frame_index() override {
      return frame_index_;
    }

    uint8_t *data() override {
      return packet_.data();
    }

    size_t data_size() override {
      return packet_.size();
    }

  private:
    // Destroyed by the network consumer thread after RTP/FEC payload creation.
    platf::rkmpp::encoded_packet_t packet_;
    int64_t frame_index_;
    bool idr_;
  };
#endif

  void free_ctx(AVCodecContext *ctx) {
    avcodec_free_context(&ctx);
  }

  /**
   * @brief Release an FFmpeg frame allocated by the capture or conversion backend.
   */
  void free_frame(AVFrame *frame) {
    av_frame_free(&frame);
  }

  /**
   * @brief Release a backend buffer allocated for capture or conversion.
   */
  void free_buffer(AVBufferRef *ref) {
    av_buffer_unref(&ref);
  }

  namespace nv {

    /**
     * @brief Enumerates supported profile h264 options.
     */
    enum class profile_h264_e : int {
      high = 2,  ///< High profile
      high_444p = 3,  ///< High 4:4:4 Predictive profile
    };

    /**
     * @brief Enumerates supported profile HEVC options.
     */
    enum class profile_hevc_e : int {
      main = 0,  ///< Main profile
      main_10 = 1,  ///< Main 10 profile
      rext = 2,  ///< Rext profile
    };

  }  // namespace nv

  namespace qsv {

    /**
     * @brief Enumerates supported profile h264 options.
     */
    enum class profile_h264_e : int {
      high = 100,  ///< High profile
      high_444p = 244,  ///< High 4:4:4 Predictive profile
    };

    /**
     * @brief Enumerates supported profile HEVC options.
     */
    enum class profile_hevc_e : int {
      main = 1,  ///< Main profile
      main_10 = 2,  ///< Main 10 profile
      rext = 4,  ///< RExt profile
    };

    /**
     * @brief Enumerates supported profile AV1 options.
     */
    enum class profile_av1_e : int {
      main = 1,  ///< Main profile
      high = 2,  ///< High profile
    };

  }  // namespace qsv

  int select_h264_profile(std::string_view encoder_name, const config_t &config, int amd_coder) {
    if (config.chromaSamplingType == 1) {
      return AV_PROFILE_H264_HIGH_444_PREDICTIVE;
    }

    if (encoder_name == "h264_amf"sv && amd_coder == std::to_underlying(amf::coder_e::cavlc)) {
      return AV_PROFILE_H264_CONSTRAINED_BASELINE;
    }

    return AV_PROFILE_H264_HIGH;
  }

  /**
   * @brief Create an FFmpeg hardware device buffer for D3D11VA input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for VA-API input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for CUDA input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for VideoToolbox input.
   *
   * @param encode_device Encode device.
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);
  /**
   * @brief Create an FFmpeg hardware device buffer for Vulkan input.
   *
   * @return Hardware buffer on success, or an error code on failure.
   */
  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *);

  int avcodec_software_encode_device_t::convert(platf::img_t &img) {
    // If we need to add aspect ratio padding, we need to scale into an intermediate output buffer
    bool requires_padding = (sw_frame->width != sws_output_frame->width || sw_frame->height != sws_output_frame->height);

    // Detect the actual capture pixel format. PipeWire-based captures (KWin
    // screencast / XDG portal) deliver NV12 with 1 byte per pixel, while
    // KMS/DMABUF captures deliver BGR0 (4 bytes per pixel). The capture
    // backend reports bytes per pixel in img.pixel_pitch; fall back to the row
    // pitch heuristic when it is unavailable.
    const auto pixel_pitch = img.pixel_pitch > 0 ? img.pixel_pitch : (img.row_pitch / std::max(img.width, 1));
    const auto input_fmt = (pixel_pitch == 1) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_BGR0;

    // The sws context is created with the default BGR0 source format;
    // recreate it once if the capture is actually NV12.
    if (input_fmt != sws_src_format) {
      sws_src_format = input_fmt;
      if (reinit_sws(input_fmt) < 0) {
        return -1;
      }
      // The colorspace details were applied to the previous sws context.
      apply_colorspace();
    }

    // Setup the input frame using the caller's img_t
    sws_input_frame->data[0] = img.data;
    sws_input_frame->linesize[0] = img.row_pitch;
    if (input_fmt == AV_PIX_FMT_NV12) {
      sws_input_frame->data[1] = img.data + static_cast<std::size_t>(img.row_pitch) * img.height;
      sws_input_frame->linesize[1] = img.row_pitch;
    } else {
      sws_input_frame->data[1] = nullptr;
      sws_input_frame->linesize[1] = 0;
    }
    sws_input_frame->data[2] = nullptr;
    sws_input_frame->linesize[2] = 0;
    sws_input_frame->data[3] = nullptr;
    sws_input_frame->linesize[3] = 0;

    // Perform color conversion and scaling to the final size
    auto status = sws_scale_frame(sws.get(), requires_padding ? sws_output_frame.get() : sw_frame.get(), sws_input_frame.get());
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Couldn't scale frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    // If we require aspect ratio padding, copy the output frame into the final padded frame
    if (requires_padding) {
      auto fmt_desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(sws_output_frame->format));
      auto planes = av_pix_fmt_count_planes(static_cast<AVPixelFormat>(sws_output_frame->format));
      for (int plane = 0; plane < planes; plane++) {
        auto shift_h = plane == 0 ? 0 : fmt_desc->log2_chroma_h;
        auto shift_w = plane == 0 ? 0 : fmt_desc->log2_chroma_w;
        auto offset = ((offsetW >> shift_w) * fmt_desc->comp[plane].step) + (offsetH >> shift_h) * sw_frame->linesize[plane];

        // Copy line-by-line to preserve leading padding for each row
        for (int line = 0; line < sws_output_frame->height >> shift_h; line++) {
          memcpy(sw_frame->data[plane] + offset + (line * sw_frame->linesize[plane]), sws_output_frame->data[plane] + (line * sws_output_frame->linesize[plane]), static_cast<std::size_t>(sws_output_frame->width >> shift_w) * fmt_desc->comp[plane].step);
        }
      }
    }

    // If frame is not a software frame, it means we still need to transfer from main memory
    // to vram memory
    if (frame->hw_frames_ctx) {
      auto status = av_hwframe_transfer_data(frame, sw_frame.get(), 0);
      if (status < 0) {
        char string[AV_ERROR_MAX_STRING_SIZE];
        BOOST_LOG(error) << "Failed to transfer image data to hardware frame: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
        return -1;
      }
    }

    return 0;
  }

  int avcodec_software_encode_device_t::set_frame(AVFrame *in_frame, AVBufferRef *hw_frames_ctx) {
    this->frame = in_frame;

    // If it's a hwframe, allocate buffers for hardware
    if (hw_frames_ctx) {
      hw_frame.reset(in_frame);

      if (av_hwframe_get_buffer(hw_frames_ctx, in_frame, 0)) {
        return -1;
      }
    } else {
      sw_frame.reset(in_frame);
    }

    return 0;
  }

  void avcodec_software_encode_device_t::apply_colorspace() {
    auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);
    sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 0, sws_getCoefficients(avcodec_colorspace.software_format), avcodec_colorspace.range - 1, 0, 1 << 16, 1 << 16);
  }

  void avcodec_software_encode_device_t::prefill() {
    auto active_frame = sw_frame ? sw_frame.get() : this->frame;
    av_frame_get_buffer(active_frame, 0);
    av_frame_make_writable(active_frame);
    std::array<ptrdiff_t, 4> linesize = {active_frame->linesize[0], active_frame->linesize[1], active_frame->linesize[2], active_frame->linesize[3]};
    av_image_fill_black(active_frame->data, linesize.data(), static_cast<AVPixelFormat>(active_frame->format), active_frame->color_range, active_frame->width, active_frame->height);
  }

  int avcodec_software_encode_device_t::init(int in_width, int in_height, AVFrame *in_frame, AVPixelFormat format, bool hardware) {
    // If the device used is hardware, yet the image resides on main memory
    if (hardware) {
      sw_frame.reset(av_frame_alloc());

      sw_frame->width = in_frame->width;
      sw_frame->height = in_frame->height;
      sw_frame->format = format;
    } else {
      this->frame = in_frame;
    }

    // Fill aspect ratio padding in the destination frame
    prefill();

    auto out_width = in_frame->width;
    auto out_height = in_frame->height;

    // Ensure aspect ratio is maintained
    auto scalar = std::fminf(static_cast<float>(out_width) / in_width, static_cast<float>(out_height) / in_height);
    out_width = in_width * scalar;
    out_height = in_height * scalar;

    sws_input_frame.reset(av_frame_alloc());
    sws_input_frame->width = in_width;
    sws_input_frame->height = in_height;
    sws_input_frame->format = AV_PIX_FMT_BGR0;

    sws_output_frame.reset(av_frame_alloc());
    sws_output_frame->width = out_width;
    sws_output_frame->height = out_height;
    sws_output_frame->format = format;

    // Result is always positive
    offsetW = (in_frame->width - out_width) / 2;
    offsetH = (in_frame->height - out_height) / 2;

    sws_src_format = AV_PIX_FMT_BGR0;

    return reinit_sws(sws_src_format);
  }

  int avcodec_software_encode_device_t::reinit_sws(AVPixelFormat src_format) {
    sws_input_frame->format = src_format;

    sws.reset(sws_alloc_context());
    if (!sws) {
      return -1;
    }

    AVDictionary *options {nullptr};
    av_dict_set_int(&options, "srcw", sws_input_frame->width, 0);
    av_dict_set_int(&options, "srch", sws_input_frame->height, 0);
    av_dict_set_int(&options, "src_format", src_format, 0);
    av_dict_set_int(&options, "dstw", sws_output_frame->width, 0);
    av_dict_set_int(&options, "dsth", sws_output_frame->height, 0);
    av_dict_set_int(&options, "dst_format", sws_output_frame->format, 0);
    av_dict_set_int(&options, "sws_flags", SWS_LANCZOS | SWS_ACCURATE_RND, 0);
    av_dict_set_int(&options, "threads", config::video.min_threads, 0);

    auto status = av_opt_set_dict(sws.get(), &options);
    av_dict_free(&options);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to set SWS options: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    status = sws_init_context(sws.get(), nullptr, nullptr);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to initialize SWS: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return 0;
  }

  /**
   * @brief Enumerates supported flag options.
   */
  enum flag_e : uint32_t {
    DEFAULT = 0,  ///< Default flags
    PARALLEL_ENCODING = 1 << 1,  ///< Capture and encoding can run concurrently on separate threads
    H264_ONLY = 1 << 2,  ///< When HEVC is too heavy
    LIMITED_GOP_SIZE = 1 << 3,  ///< Some encoders don't like it when you have an infinite GOP_SIZE. e.g. VAAPI
    SINGLE_SLICE_ONLY = 1 << 4,  ///< Never use multiple slices. Older intel iGPU's ruin it for everyone else
    CBR_WITH_VBR = 1 << 5,  ///< Use a VBR rate control mode to simulate CBR
    RELAXED_COMPLIANCE = 1 << 6,  ///< Use FF_COMPLIANCE_UNOFFICIAL compliance mode
    NO_RC_BUF_LIMIT = 1 << 7,  ///< Don't set rc_buffer_size
    REF_FRAMES_INVALIDATION = 1 << 8,  ///< Support reference frames invalidation
    ALWAYS_REPROBE = 1 << 9,  ///< This is an encoder of last resort and we want to aggressively probe for a better one
    YUV444_SUPPORT = 1 << 10,  ///< Encoder may support 4:4:4 chroma sampling depending on hardware
    ASYNC_TEARDOWN = 1 << 11,  ///< Encoder supports async teardown on a different thread
    FIXED_GOP_SIZE = 1 << 12,  ///< Use fixed small GOP size (encoder doesn't support on-demand IDR frames)
    SINGLE_USE_INPUT = 1 << 13,  ///< Each converted input may be submitted to the encoder only once
  };

  /**
   * @brief FFmpeg AVCodec encode session and parameter-set rewriting state.
   */
  class avcodec_encode_session_t: public encode_session_t {
  public:
    avcodec_encode_session_t() = default;

    /**
     * @brief Initialize an FFmpeg encode session and its hardware encode device.
     *
     * @param avcodec_ctx Open FFmpeg codec context for the selected encoder.
     * @param encode_device Platform encode device that supplies frames to FFmpeg.
     * @param inject Whether SPS/VPS replacement data should be injected.
     */
    avcodec_encode_session_t(avcodec_ctx_t &&avcodec_ctx, std::unique_ptr<platf::avcodec_encode_device_t> encode_device, int inject):
        avcodec_ctx {std::move(avcodec_ctx)},
        device {std::move(encode_device)},
        inject {inject} {
    }

    /**
     * @brief Move an FFmpeg encode session without duplicating codec/device ownership.
     *
     * @param other Source object whose state is copied or moved into this object.
     */
    avcodec_encode_session_t(avcodec_encode_session_t &&other) noexcept = default;

    ~avcodec_encode_session_t() {
      // Flush any remaining frames in the encoder if the encoder started up (frame num > 0)
      if (avcodec_ctx->frame_num > 0 && avcodec_send_frame(avcodec_ctx.get(), nullptr) == 0) {
        packet_raw_avcodec pkt;
        while (avcodec_receive_packet(avcodec_ctx.get(), pkt.av_packet) == 0);
      }

      // Order matters here because the context relies on the hwdevice still being valid
      avcodec_ctx.reset();
      device.reset();
    }

    // Ensure objects are destroyed in the correct order
    /**
     * @brief Assign state from another instance while preserving ownership semantics.
     *
     * @param other Source object whose state is copied or moved into this object.
     * @return Reference or value produced by the operator.
     */
    avcodec_encode_session_t &operator=(avcodec_encode_session_t &&other) {
      device = std::move(other.device);
      avcodec_ctx = std::move(other.avcodec_ctx);
      replacements = std::move(other.replacements);
      sps = std::move(other.sps);
      vps = std::move(other.vps);

      inject = other.inject;

      return *this;
    }

    /**
     * @brief Encode one frame with FFmpeg AVCodec and prepare packet replacements.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
      }
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      if (device && device->frame) {
        auto &frame = device->frame;
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
      }
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      BOOST_LOG(error) << "Encoder doesn't support reference frame invalidation";
      request_idr_frame();
    }

    avcodec_ctx_t avcodec_ctx;  ///< FFmpeg codec context owned by the encode session.
    std::unique_ptr<platf::avcodec_encode_device_t> device;  ///< Platform device used by the FFmpeg hardware encoder.

    std::vector<packet_raw_t::replace_t> replacements;  ///< NAL-unit byte ranges that must be replaced before packet send.

    cbs::nal_t sps;  ///< Original and rewritten sequence parameter set for IDR injection.
    cbs::nal_t vps;  ///< Original and rewritten HEVC video parameter set for IDR injection.

    // inject sps/vps data into idr pictures
    int inject;  ///< Number of upcoming IDR frames that should receive rewritten parameter sets.
  };

  /**
   * @brief NVENC encode session and device state for hardware encoding.
   */
  class nvenc_encode_session_t: public encode_session_t {
  public:
    /**
     * @brief Initialize an NVENC encode session and take ownership of its device.
     *
     * @param encode_device Encode device.
     */
    nvenc_encode_session_t(std::unique_ptr<platf::nvenc_encode_device_t> encode_device):
        device(std::move(encode_device)) {
    }

    /**
     * @brief Encode one frame with NVENC and return the packet payload.
     *
     * @param img Image or frame object to read from or populate.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      if (!device) {
        return -1;
      }
      return device->convert(img);
    }

    /**
     * @brief Mark the frame as a request for an IDR frame.
     */
    void request_idr_frame() override {
      force_idr = true;
    }

    /**
     * @brief Mark the frame as a request for a normal inter frame.
     */
    void request_normal_frame() override {
      force_idr = false;
    }

    /**
     * @brief Mark the frame range whose references must be invalidated.
     *
     * @param first_frame First frame.
     * @param last_frame Last frame.
     */
    void invalidate_ref_frames(int64_t first_frame, int64_t last_frame) override {
      if (!device || !device->nvenc) {
        return;
      }

      if (!device->nvenc->invalidate_ref_frames(first_frame, last_frame)) {
        force_idr = true;
      }
    }

    /**
     * @brief Submit the next frame to NVENC and return the encoded payload.
     *
     * @param frame_index Monotonic frame index assigned by the video pipeline.
     * @return Encoded NVENC frame payload and frame metadata.
     */
    nvenc::nvenc_encoded_frame encode_frame(uint64_t frame_index) {
      if (!device || !device->nvenc) {
        return {};
      }

      auto result = device->nvenc->encode_frame(frame_index, force_idr);
      force_idr = false;
      return result;
    }

  private:
    std::unique_ptr<platf::nvenc_encode_device_t> device;
    bool force_idr = false;
  };

  /**
   * @brief Context object used while synchronizing encode sessions.
   */
  struct sync_session_ctx_t {
    safe::signal_t *join_event;  ///< Signal raised when the capture and encode workers should join.
    safe::mail_raw_t::event_t<bool> shutdown_event;  ///< Event raised when the stream should shut down.
    safe::mail_raw_t::queue_t<packet_t> packets;  ///< Queue receiving encoded video packets for the stream sender.
    safe::mail_raw_t::event_t<bool> idr_events;  ///< Event raised when an IDR frame is requested.
    safe::mail_raw_t::event_t<hdr_info_t> hdr_events;  ///< Event carrying updated HDR metadata.
    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_events;  ///< Event carrying updated touch viewport metadata.

    config_t config;  ///< Stream or encoder configuration captured for the worker.
    int frame_nr;  ///< Next capture-frame number assigned to encoded packets.
    void *channel_data;  ///< Platform-specific channel data forwarded to packet senders.
  };

  /**
   * @brief Synchronization state for one encode session.
   */
  struct sync_session_t {
    sync_session_ctx_t *ctx;  ///< Shared capture/encode synchronization context.
    std::unique_ptr<encode_session_t> session;  ///< Active encoder session used by the capture thread.
  };

  /**
   * @brief Queue of encode-session contexts waiting for capture work.
   */
  using encode_session_ctx_queue_t = safe::queue_t<sync_session_ctx_t>;
  /**
   * @brief Platform capture status returned by encode operations.
   */
  using encode_e = platf::capture_e;

  /**
   * @brief Capture thread context shared with the encoder session.
   */
  struct capture_ctx_t {
    img_event_t images;  ///< Queue of captured images waiting for encode.
    config_t config;  ///< Stream or encoder configuration captured for the worker.
  };

  /**
   * @brief Asynchronous capture thread state.
   */
  struct capture_thread_async_ctx_t {
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue;  ///< Capture ctx queue.
    std::jthread capture_thread;  ///< Capture thread.

    safe::signal_t reinit_event;  ///< Reinit event.
    const encoder_t *encoder_p;  ///< Encoder p.
    sync_util::sync_t<std::weak_ptr<platf::display_t>> display_wp;  ///< Display wp.
  };

  /**
   * @brief Synchronous capture thread state.
   */
  struct capture_thread_sync_ctx_t {
    encode_session_ctx_queue_t encode_session_ctx_queue {30};  ///< Encode session ctx queue.
  };

  /**
   * @brief Start the synchronous multi-client capture thread.
   *
   * @param ctx Native context object used by the operation or callback.
   * @return 0 when the capture thread is started.
   */
  int start_capture_sync(capture_thread_sync_ctx_t &ctx);
  /**
   * @brief Stop capture sync processing.
   *
   * @param ctx Native context object used by the operation or callback.
   */
  void end_capture_sync(capture_thread_sync_ctx_t &ctx);
  /**
   * @brief Start the asynchronous capture thread.
   *
   * @param ctx Native context object used by the operation or callback.
   * @return 0 when the capture thread is started; nonzero on setup failure.
   */
  int start_capture_async(capture_thread_async_ctx_t &ctx);
  /**
   * @brief Stop capture async processing.
   *
   * @param ctx Native context object used by the operation or callback.
   */
  void end_capture_async(capture_thread_async_ctx_t &ctx);

  // Keep a reference counter to ensure the capture thread only runs when other threads have a reference to the capture thread
  auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(start_capture_async, end_capture_async);  ///< Capture thread async.
  auto capture_thread_sync = safe::make_shared<capture_thread_sync_ctx_t>(start_capture_sync, end_capture_sync);  ///< Capture thread sync.

#ifdef _WIN32
  /**
   * @brief NVENC.
   */
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_nvenc>(
      platf::mem_type_e::dxgi,
      platf::pix_fmt_e::nv12,
      platf::pix_fmt_e::p010,
      platf::pix_fmt_e::ayuv,
      platf::pix_fmt_e::yuv444p16
    ),
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {},  // Common options
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING | REF_FRAMES_INVALIDATION | YUV444_SUPPORT | ASYNC_TEARDOWN  // flags
  };
#elif !defined(__APPLE__)
  encoder_t nvenc {
    "nvenc"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
  #ifdef _WIN32
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
  #else
      AV_HWDEVICE_TYPE_CUDA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_CUDA,
  #endif
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P16,
  #ifdef _WIN32
      dxgi_init_avcodec_hardware_input_buffer
  #else
      cuda_init_avcodec_hardware_input_buffer
  #endif
    ),
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_nvenc"s,
    },
    {
      // Common options
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(nv::profile_hevc_e::main)},
      },
      {
        // HDR-specific options
        {"profile"s, std::to_underlying(nv::profile_hevc_e::main_10)},
      },
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_nvenc"s,
    },
    {
      {
        {"delay"s, 0},
        {"forced-idr"s, 1},
        {"zerolatency"s, 1},
        {"surfaces"s, 1},
        {"cbr_padding"s, false},
        {"preset"s, &config::video.nv_legacy.preset},
        {"tune"s, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {"rc"s, NV_ENC_PARAMS_RC_CBR},
        {"coder"s, &config::video.nv_legacy.h264_coder},
        {"multipass"s, &config::video.nv_legacy.multipass},
        {"aq"s, &config::video.nv_legacy.aq},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(nv::profile_h264_e::high)},
      },
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_nvenc"s,
    },
    PARALLEL_ENCODING | YUV444_SUPPORT
  };
#endif

#ifdef _WIN32
  /**
   * @brief Quicksync.
   */
  encoder_t quicksync {
    "quicksync"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_QSV,
      AV_PIX_FMT_QSV,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_VUYX,
      AV_PIX_FMT_XV30,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::main)},
      },
      {
        // HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::main)},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::high)},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_av1_e::high)},
      },
      {},  // Fallback options
      "av1_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"pic_timing_sei"s, 0},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::main)},
      },
      {
        // HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::main_10)},
      },
      {
        // YUV444 SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::rext)},
      },
      {
        // YUV444 HDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_hevc_e::rext)},
      },
      {
        // Fallback options
        {"low_power"s, []() {
           return config::video.qsv.qsv_slow_hevc ? 0 : 1;
         }},
      },
      "hevc_qsv"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.qsv.qsv_preset},
        {"cavlc"s, &config::video.qsv.qsv_cavlc},
        {"forced_idr"s, 1},
        {"async_depth"s, 1},
        {"low_delay_brc"s, 1},
        {"low_power"s, 1},
        {"recovery_point_sei"s, 0},
        {"vcm"s, 1},
        {"pic_timing_sei"s, 0},
        {"max_dec_frame_buffering"s, 1},
      },
      {
        // SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_h264_e::high)},
      },
      {},  // HDR-specific options
      {
        // YUV444 SDR-specific options
        {"profile"s, std::to_underlying(qsv::profile_h264_e::high_444p)},
      },
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"low_power"s, 0},  // Some old/low-end Intel GPUs don't support low power encoding
      },
      "h264_qsv"s,
    },
    PARALLEL_ENCODING | CBR_WITH_VBR | RELAXED_COMPLIANCE | NO_RC_BUF_LIMIT | YUV444_SUPPORT
  };

  /**
   * @brief Amdvce.
   */
  encoder_t amdvce {
    "amdvce"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, "lowest_latency"s},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_av1},
        {"rc"s, &config::video.amd.amd_rc_av1},
        {"usage"s, &config::video.amd.amd_usage_av1},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"skip_frame"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"gops_per_idr"s, 1},
        {"header_insertion_mode"s, "idr"s},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_hevc},
        {"rc"s, &config::video.amd.amd_rc_hevc},
        {"usage"s, &config::video.amd.amd_usage_hevc},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"max_au_size"s, &config::video.amd.amd_max_au_size},
        {"level"s, [](const config_t &cfg) {
           auto size = cfg.width * cfg.height;
           // For 4K and below, try to use level 5.1 or 5.2 if possible
           if (size <= 8912896) {
             if (size * cfg.framerate <= 534773760) {
               return "5.1"s;
             } else if (size * cfg.framerate <= 1069547520) {
               return "5.2"s;
             }
           }
           return "auto"s;
         }},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_amf"s,
    },
    {
      // Common options
      {
        {"filler_data"s, false},
        {"forced_idr"s, 1},
        {"latency"s, 1},
        {"async_depth"s, 1},
        {"frame_skipping"s, 0},
        {"log_to_dbg"s, []() {
           return config::sunshine.min_log_level < 2 ? 1 : 0;
         }},
        {"preencode"s, &config::video.amd.amd_preanalysis},
        {"quality"s, &config::video.amd.amd_quality_h264},
        {"rc"s, &config::video.amd.amd_rc_h264},
        {"usage"s, &config::video.amd.amd_usage_h264},
        {"vbaq"s, &config::video.amd.amd_vbaq},
        {"coder"s, &config::video.amd.amd_coder},
        {"enforce_hrd"s, &config::video.amd.amd_enforce_hrd},
        {"max_au_size"s, &config::video.amd.amd_max_au_size},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"usage"s, 2 /* AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY */},  // Workaround for https://github.com/GPUOpen-LibrariesAndSDKs/AMF/issues/410
      },
      "h264_amf"s,
    },
    PARALLEL_ENCODING
  };

  /**
   * @brief Mediafoundation.
   */
  encoder_t mediafoundation {
    "mediafoundation"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_D3D11VA,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_D3D11,
      AV_PIX_FMT_NV12,  // SDR 4:2:0 8-bit (only format Qualcomm supports)
      AV_PIX_FMT_NONE,  // No HDR - Qualcomm MF only supports 8-bit
      AV_PIX_FMT_NONE,  // No YUV444 SDR
      AV_PIX_FMT_NONE,  // No YUV444 HDR
      dxgi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options for AV1 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_mf"s,
    },
    {
      // Common options for HEVC - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_mf"s,
    },
    {
      // Common options for H.264 - Qualcomm MF encoder
      {
        {"hw_encoding"s, 1},
        {"rate_control"s, "cbr"s},
        {"scenario"s, "display_remoting"s},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_mf"s,
    },
    PARALLEL_ENCODING | FIXED_GOP_SIZE  // MF encoder doesn't support on-demand IDR frames
  };
#endif

  /**
   * @brief Software.
   */
  encoder_t software {
    "software"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_NONE,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_YUV420P,
      AV_PIX_FMT_YUV420P10,
      AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV444P10,
      nullptr
    ),
    {
      // libsvtav1 takes different presets than libx264/libx265.
      // We set an infinite GOP length, use a low delay prediction structure,
      // force I frames to be key frames, and set max bitrate to default to work
      // around a FFmpeg bug with CBR mode.
      {
        {"svtav1-params"s, "keyint=-1:pred-struct=1:force-key-frames=1:mbr=0"s},
        {"preset"s, &config::video.sw.svtav1_preset},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options

#ifdef ENABLE_BROKEN_AV1_ENCODER
           // Due to bugs preventing on-demand IDR frames from working and very poor
           // real-time encoding performance, we do not enable libsvtav1 by default.
           // It is only suitable for testing AV1 until the IDR frame issue is fixed.
      "libsvtav1"s,
#else
      {},
#endif
    },
    {
      // x265's Info SEI is so long that it causes the IDR picture data to be
      // kicked to the 2nd packet in the frame, breaking Moonlight's parsing logic.
      // It also looks like gop_size isn't passed on to x265, so we have to set
      // 'keyint=-1' in the parameters ourselves.
      {
        {"forced-idr"s, 1},
        {"x265-params"s, "info=0:keyint=-1"s},
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx265"s,
    },
    {
      // Common options
      {
        {"preset"s, &config::video.sw.sw_preset},
        {"tune"s, &config::video.sw.sw_tune},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "libx264"s,
    },
    H264_ONLY | PARALLEL_ENCODING | ALWAYS_REPROBE | YUV444_SUPPORT
  };

#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  /**
   * @brief VA-API.
   */
  #ifdef SUNSHINE_BUILD_RKMPP
  encoder_t rkmpp {
    "rkmpp"sv,
    std::make_unique<encoder_platform_formats_rkmpp>(),
    {{}, {}, {}, {}, {}, {}, {}},
    {{}, {}, {}, {}, {}, {}, "hevc_rkmpp"s},
    {{}, {}, {}, {}, {}, {}, "h264_rkmpp"s},
    PARALLEL_ENCODING | SINGLE_USE_INPUT
  };
  #endif

  encoder_t vaapi {
    "vaapi"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VAAPI,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VAAPI,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vaapi_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"blbrc"s, &config::video.vaapi.blbrc},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vaapi"s,
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"blbrc"s, &config::video.vaapi.blbrc},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vaapi"s,
    },
    {
      // Common options
      {
        {"async_depth"s, 1},
        {"blbrc"s, &config::video.vaapi.blbrc},
        {"sei"s, 0},
        {"idr_interval"s, std::numeric_limits<int>::max()},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vaapi"s,
    },
    // RC buffer size will be set in platform code if supported
    LIMITED_GOP_SIZE | PARALLEL_ENCODING | NO_RC_BUF_LIMIT
  };

  #ifdef SUNSHINE_BUILD_VULKAN
  encoder_t vulkan {
    "vulkan"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VULKAN,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VULKAN,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vulkan_init_avcodec_hardware_input_buffer
    ),
    {
      // AV1
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
        {"async_depth"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_vulkan"s,
    },
    {
      // HEVC
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
        {"async_depth"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_vulkan"s,
    },
    {
      // H.264
      {
        {"idr_interval"s, std::numeric_limits<int>::max()},
        {"tune"s, &config::video.vk.tune},
        {"rc_mode"s, &config::video.vk.rc_mode},
        {"units"s, 0},
        {"usage"s, "stream"s},
        {"content"s, "rendered"s},
        {"async_depth"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "h264_vulkan"s,
    },
    LIMITED_GOP_SIZE | PARALLEL_ENCODING
  };
  #endif  // SUNSHINE_BUILD_VULKAN
#endif  // linux

#ifdef __APPLE__
  /**
   * @brief Videotoolbox.
   */
  encoder_t videotoolbox {
    "videotoolbox"sv,
    std::make_unique<encoder_platform_formats_avcodec>(
      AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
      AV_HWDEVICE_TYPE_NONE,
      AV_PIX_FMT_VIDEOTOOLBOX,
      AV_PIX_FMT_NV12,
      AV_PIX_FMT_P010,
      AV_PIX_FMT_NONE,
      AV_PIX_FMT_NONE,
      vt_init_avcodec_hardware_input_buffer
    ),
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "av1_videotoolbox"s,
    },
    {
      // Common options
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
        {"max_ref_frames"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {},  // Fallback options
      "hevc_videotoolbox"s,
    },
    {
      // Common options
      // Note: max_ref_frames is intentionally omitted for H.264 because
      // VideoToolbox on Apple Silicon produces all-IDR output when
      // ReferenceBufferCount=1 is set for H.264, causing massive bandwidth
      // inflation (~3x) and frame drops. HEVC and AV1 are unaffected and
      // retain max_ref_frames=1. See LizardByte/Sunshine#5013.
      {
        {"allow_sw"s, &config::video.vt.vt_allow_sw},
        {"require_sw"s, &config::video.vt.vt_require_sw},
        {"realtime"s, &config::video.vt.vt_realtime},
        {"prio_speed"s, 1},
      },
      {},  // SDR-specific options
      {},  // HDR-specific options
      {},  // YUV444 SDR-specific options
      {},  // YUV444 HDR-specific options
      {
        // Fallback options
        {"flags"s, "-low_delay"},
      },
      "h264_videotoolbox"s,
    },
    PARALLEL_ENCODING
  };
#endif

  static const std::vector encoders {
#ifndef __APPLE__
    &nvenc,
#endif
#ifdef _WIN32
    &quicksync,
    &amdvce,
    &mediafoundation,
#endif
#if defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
  #ifdef SUNSHINE_BUILD_RKMPP
    &rkmpp,
  #endif
  #ifdef SUNSHINE_BUILD_VULKAN
    &vulkan,
  #endif
    &vaapi,
#endif
#ifdef __APPLE__
    &videotoolbox,
#endif
    &software
  };

  static encoder_t *chosen_encoder;
  int active_hevc_mode;  ///< HEVC mode selected by the most recent encoder probe.
  int active_av1_mode;  ///< AV1 mode selected by the most recent encoder probe.
  bool last_encoder_probe_supported_ref_frames_invalidation = false;  ///< Whether the last probe found reference-frame invalidation support.
  std::array<bool, 3> last_encoder_probe_supported_yuv444_for_codec = {};  ///< YUV444 support discovered for each probed codec.

  /**
   * @brief Recreate a display capture object after a capture failure.
   *
   * @param disp Display connection or display handle.
   * @param type Protocol, message, or resource type selector.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   */
  void reset_display(
    std::shared_ptr<platf::display_t> &disp,
    const platf::mem_type_e &type,
    const std::string &display_name,
    const config_t &config,
    platf::display_purpose_e purpose = platf::display_purpose_e::stream
  ) {
    // We try this twice, in case we still get an error on reinitialization
    for (int x = 0; x < 2; ++x) {
      disp.reset();
      disp = platf::display(type, display_name, config, purpose);
      if (disp) {
        break;
      }

      // The capture code depends on us to sleep between failures
      std::this_thread::sleep_for(200ms);
    }
  }

  /**
   * @brief Update the list of display names before or during a stream.
   * @details This will attempt to keep `current_display_index` pointing at the same display.
   * @param dev_type The encoder device type used for display lookup.
   * @param display_names The list of display names to repopulate.
   * @param current_display_index The current display index or -1 if not yet known.
   */
  void refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index) {
    // It is possible that the output name may be empty even if it wasn't before (device disconnected) or vice-versa
    const auto output_name {display_device::map_output_name(config::video.output_name)};
    std::string current_display_name;

    // If we have a current display index, let's start with that
    if (current_display_index >= 0 && current_display_index < display_names.size()) {
      current_display_name = display_names.at(current_display_index);
    }

    // Refresh the display names
    auto old_display_names = std::move(display_names);
    display_names = platf::display_names(dev_type);

    // If we now have no displays, let's put the old display array back and fail
    if (display_names.empty() && !old_display_names.empty()) {
      BOOST_LOG(error) << "No displays were found after reenumeration!"sv;
      display_names = std::move(old_display_names);
      return;
    } else if (display_names.empty()) {
      display_names.emplace_back(output_name);
    }

    // We now have a new display name list, so reset the index back to 0
    current_display_index = 0;

    // If we had a name previously, let's try to find it in the new list
    if (!current_display_name.empty()) {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == current_display_name) {
          current_display_index = x;
          return;
        }
      }

      // The old display was removed, so we'll start back at the first display again
      BOOST_LOG(warning) << "Previous active display ["sv << current_display_name << "] is no longer present"sv;
    } else {
      for (int x = 0; x < display_names.size(); ++x) {
        if (display_names[x] == output_name) {
          current_display_index = x;
          return;
        }
      }
    }
  }

  /**
   * @brief Run the shared display capture thread for asynchronous encoding.
   *
   * @param capture_ctx_queue Capture context queue.
   * @param display_wp Weak pointer holder for the active display.
   * @param reinit_event Signal raised while the display is being reinitialized.
   * @param encoder Selected encoder.
   */
  void captureThread(
    std::shared_ptr<safe::queue_t<capture_ctx_t>> capture_ctx_queue,
    sync_util::sync_t<std::weak_ptr<platf::display_t>> &display_wp,
    safe::signal_t &reinit_event,
    const encoder_t &encoder
  ) {
    std::vector<capture_ctx_t> capture_ctxs;

    auto fg = util::fail_guard([&]() {
      capture_ctx_queue->stop();

      // Stop all sessions listening to this thread
      for (auto &capture_ctx : capture_ctxs) {
        capture_ctx.images->stop();
      }
      for (auto &capture_ctx : capture_ctx_queue->unsafe()) {
        capture_ctx.images->stop();
      }
    });

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    // Wait for the initial capture context or a request to stop the queue
    auto initial_capture_ctx = capture_ctx_queue->pop();
    if (!initial_capture_ctx) {
      return;
    }
    capture_ctxs.emplace_back(std::move(*initial_capture_ctx));

    // Get all the monitor names now, rather than at boot, to
    // get the most up-to-date list available monitors
    std::vector<std::string> display_names;
    int display_p = -1;
    refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);
    auto disp = platf::display(encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
    if (!disp) {
      return;
    }
    display_wp = disp;

    constexpr auto capture_buffer_size = 12;
    std::list<std::shared_ptr<platf::img_t>> imgs(capture_buffer_size);

    std::vector<std::optional<std::chrono::steady_clock::time_point>> imgs_used_timestamps;
    const std::chrono::seconds trim_timeot = 3s;
    auto trim_imgs = [&]() {
      // count allocated and used within current pool
      size_t allocated_count = 0;
      size_t used_count = 0;
      for (const auto &img : imgs) {
        if (img) {
          allocated_count += 1;
          if (img.use_count() > 1) {
            used_count += 1;
          }
        }
      }

      // remember the timestamp of currently used count
      const auto now = std::chrono::steady_clock::now();
      if (imgs_used_timestamps.size() <= used_count) {
        imgs_used_timestamps.resize(used_count + 1);
      }
      imgs_used_timestamps[used_count] = now;

      // decide whether to trim allocated unused above the currently used count
      // based on last used timestamp and universal timeout
      size_t trim_target = used_count;
      for (size_t i = used_count; i < imgs_used_timestamps.size(); i++) {
        if (imgs_used_timestamps[i] && now - *imgs_used_timestamps[i] < trim_timeot) {
          trim_target = i;
        }
      }

      // trim allocated unused above the newly decided trim target
      if (allocated_count > trim_target) {
        size_t to_trim = allocated_count - trim_target;
        // prioritize trimming least recently used
        for (auto it = imgs.rbegin(); it != imgs.rend(); it++) {
          auto &img = *it;
          if (img && img.use_count() == 1) {
            img.reset();
            to_trim -= 1;
            if (to_trim == 0) {
              break;
            }
          }
        }
        // forget timestamps that no longer relevant
        imgs_used_timestamps.resize(trim_target + 1);
      }
    };

    auto pull_free_image_callback = [&](std::shared_ptr<platf::img_t> &img_out) -> bool {
      img_out.reset();
      while (capture_ctx_queue->running()) {
        // pick first allocated but unused
        for (auto it = imgs.begin(); it != imgs.end(); it++) {
          if (*it && it->use_count() == 1) {
            img_out = *it;
            if (it != imgs.begin()) {
              // move image to the front of the list to prioritize its reusal
              imgs.erase(it);
              imgs.push_front(img_out);
            }
            break;
          }
        }
        // otherwise pick first unallocated
        if (!img_out) {
          for (auto it = imgs.begin(); it != imgs.end(); it++) {
            if (!*it) {
              // allocate image
              *it = disp->alloc_img();
              img_out = *it;
              if (it != imgs.begin()) {
                // move image to the front of the list to prioritize its reusal
                imgs.erase(it);
                imgs.push_front(img_out);
              }
              break;
            }
          }
        }
        if (img_out) {
          // trim allocated but unused portion of the pool based on timeouts
          trim_imgs();
          img_out->frame_timestamp.reset();
          img_out->frame_profile.reset();
          return true;
        } else {
          // sleep and retry if image pool is full
          std::this_thread::sleep_for(1ms);
        }
      }
      return false;
    };

    // Capture takes place on this thread
    platf::set_thread_name("video::capture");
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    while (capture_ctx_queue->running()) {
      bool artificial_reinit = false;

      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
#ifdef SUNSHINE_BUILD_RKMPP
        const bool direct_ui_write_safe = encoder.name == "rkmpp" && std::count_if(capture_ctxs.begin(), capture_ctxs.end(), [](const auto &capture_ctx) {
                                                                       return capture_ctx.images->running();
                                                                     }) == 1;
#endif
        KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
          if (!capture_ctx->images->running()) {
            capture_ctx = capture_ctxs.erase(capture_ctx);

            continue;
          }

          if (frame_captured) {
            auto session_image = img;
#ifdef SUNSHINE_BUILD_RKMPP
            if (encoder.name == "rkmpp") {
              auto source = std::dynamic_pointer_cast<platf::hdmirx::hdmirx_img_t>(img);
              if (!source) {
                return false;
              }
              auto clone = std::make_shared<platf::hdmirx::hdmirx_img_t>();
              clone->data = source->data;
              clone->width = source->width;
              clone->height = source->height;
              clone->pixel_pitch = source->pixel_pitch;
              clone->row_pitch = source->row_pitch;
              clone->frame_timestamp = source->frame_timestamp;
              clone->frame_profile = source->frame_profile;
              clone->frame = source->frame;
              clone->capture_format = source->capture_format;
              clone->placeholder = source->placeholder;
              clone->request_idr = source->request_idr;
              clone->connection_state = source->connection_state;
              clone->moonlight_width = capture_ctx->config.width;
              clone->moonlight_height = capture_ctx->config.height;
              clone->input_width = source->input_width;
              clone->input_height = source->input_height;
              clone->direct_ui_write_safe = direct_ui_write_safe;
              if (clone->frame_profile) {
                clone->frame_profile->moonlight_width = static_cast<std::uint32_t>(capture_ctx->config.width);
                clone->frame_profile->moonlight_height = static_cast<std::uint32_t>(capture_ctx->config.height);
              }
              session_image = std::move(clone);
            }
#endif
            capture_ctx->images->raise_latest(std::move(session_image), [](std::shared_ptr<platf::img_t> &displaced, std::shared_ptr<platf::img_t> &replacement) {
#ifdef SUNSHINE_BUILD_RKMPP
              auto old_rx = std::dynamic_pointer_cast<platf::hdmirx::hdmirx_img_t>(displaced);
              auto new_rx = std::dynamic_pointer_cast<platf::hdmirx::hdmirx_img_t>(replacement);
              if (!old_rx || !new_rx) {
                return;
              }
              if (new_rx->frame_profile) {
                new_rx->frame_profile->raw_replaced += 1U;
                if (old_rx->frame_profile) {
                  new_rx->frame_profile->raw_replaced += old_rx->frame_profile->raw_replaced;
                  new_rx->frame_profile->prepared_replaced += old_rx->frame_profile->prepared_replaced;
                  new_rx->frame_profile->target_waits += old_rx->frame_profile->target_waits;
                  new_rx->frame_profile->sticky_idr_transfers += old_rx->frame_profile->sticky_idr_transfers;
                }
              }
              if (old_rx->request_idr) {
                new_rx->request_idr = true;
                if (new_rx->frame_profile) {
                  ++new_rx->frame_profile->sticky_idr_transfers;
                }
              }
#endif
            });
          }

          ++capture_ctx;
        })

        if (!capture_ctx_queue->running()) {
          return false;
        }

        while (capture_ctx_queue->peek()) {
          capture_ctxs.emplace_back(std::move(*capture_ctx_queue->pop()));
        }

        if (switch_display_event->peek()) {
          artificial_reinit = true;
          return false;
        }

        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);

      if (artificial_reinit && status != platf::capture_e::error) {
        status = platf::capture_e::reinit;

        artificial_reinit = false;
      }

      switch (status) {
        case platf::capture_e::reinit:
          {
            reinit_event.raise(true);

            // Some classes of images contain references to the display --> display won't delete unless img is deleted
            for (auto &img : imgs) {
              img.reset();
            }

            // display_wp is modified in this thread only
            // Wait for the other shared_ptr's of display to be destroyed.
            // New displays will only be created in this thread.
            while (display_wp->use_count() != 1) {
              // Free images that weren't consumed by the encoders. These can reference the display and prevent
              // the ref count from reaching 1. We do this here rather than on the encoder thread to avoid race
              // conditions where the encoding loop might free a good frame after reinitializing if we capture
              // a new frame here before the encoder has finished reinitializing.
              KITTY_WHILE_LOOP(auto capture_ctx = std::begin(capture_ctxs), capture_ctx != std::end(capture_ctxs), {
                if (!capture_ctx->images->running()) {
                  capture_ctx = capture_ctxs.erase(capture_ctx);
                  continue;
                }

                while (capture_ctx->images->try_pop()) {
                }

                ++capture_ctx;
              });

              std::this_thread::sleep_for(20ms);
            }

            while (capture_ctx_queue->running()) {
              // Release the display before reenumerating displays, since some capture backends
              // only support a single display session per device/application.
              disp.reset();

              // Refresh display names since a display removal might have caused the reinitialization
              refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

              // Process any pending display switch with the new list of displays
              if (switch_display_event->peek()) {
                display_p = std::clamp(*switch_display_event->pop(), 0, static_cast<int>(display_names.size()) - 1);
              }

              // reset_display() will sleep between retries
              reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], capture_ctxs.front().config);
              if (disp) {
                break;
              }
            }
            if (!disp) {
              return;
            }

            display_wp = disp;

            reinit_event.reset();
            continue;
          }
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return;
        default:
          BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
          return;
      }
    }
  }

  /**
   * @brief Drain encoded packets from an FFmpeg encoder session.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active FFmpeg encoder session.
   * @param packets Output queue that receives encoded packets.
   * @param channel_data Platform or protocol state attached to each packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when encoding or packetization fails.
   */
#ifdef SUNSHINE_BUILD_RKMPP

  static std::optional<platf::rga::pixel_format_e> rga_format_from_mpp(std::uint32_t mpp_fmt) {
    if (mpp_fmt == MPP_FMT_YUV420SP) {
      return platf::rga::pixel_format_e::nv12;
    }
    if (mpp_fmt == MPP_FMT_BGR888) {
      return platf::rga::pixel_format_e::bgr888;
    }
    return std::nullopt;
  }

  platf::rkmpp::encoder_config_t make_rkmpp_encoder_config(const config_t &config, const platf::rkmpp::input_layout_t &input_layout) {
    if (config.numRefFrames != 0) {
      throw std::invalid_argument("RKMPP does not support a requested reference-frame count");
    }
    if (config.slicesPerFrame > 1) {
      throw std::invalid_argument("RKMPP does not support more than one slice per frame");
    }
    if (config.videoFormat != 0 && config.videoFormat != 1) {
      throw std::invalid_argument("RKMPP supports only H.264 and HEVC");
    }
    if (config.width <= 0 || config.height <= 0) {
      throw std::invalid_argument("RKMPP coded dimensions are invalid");
    }
    if (config.bitrate <= 0 || static_cast<std::uint64_t>(config.bitrate) > std::numeric_limits<std::uint32_t>::max() / 1000U) {
      throw std::invalid_argument("RKMPP bitrate in kbit/s is out of range");
    }
    const auto fps = framerate_to_rational(config);
    if (fps.num <= 0 || fps.den <= 0) {
      throw std::invalid_argument("RKMPP frame rate is invalid");
    }
    const auto gop = (static_cast<std::uint64_t>(fps.num) + static_cast<std::uint64_t>(fps.den) / 2U) / static_cast<std::uint64_t>(fps.den);
    if (gop == 0 || gop > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("RKMPP GOP is out of range");
    }
    return {config.videoFormat == 0 ? platf::rkmpp::codec_e::h264 : platf::rkmpp::codec_e::h265, input_layout, static_cast<std::uint32_t>(config.width), static_cast<std::uint32_t>(config.height), static_cast<std::uint32_t>(fps.num), static_cast<std::uint32_t>(fps.den), static_cast<std::uint32_t>(config.bitrate) * 1000U, static_cast<std::uint32_t>(gop), ::config::video.rkmpp_low_delay, ::config::video.rkmpp_disable_reencode, config.videoFormat == 1};
  }

  /**
   * @brief Cache a Vulkan-rendered UI and cover each live HDMI RX DMA-BUF ROI.
   *
   * Matching exclusive BGR capture is covered by Vulkan. Validated exclusive
   * NV12 capture receives a synchronous RGA conversion of the published panel.
   * Shared input and unsupported layouts use a session-private BGR target.
   */
  class rkmpp_vulkan_ui_session_t {
    struct surface_t;

  public:
    rkmpp_vulkan_ui_session_t(std::uint32_t output_width, std::uint32_t output_height, std::uint32_t output_fps_x100):
        output_width_(output_width),
        output_height_(output_height),
        output_fps_x100_(output_fps_x100),
        metrics_(platf::vulkan_ui::make_layout_metrics(output_width, output_height)),
        backend_(platf::rga::make_backend()),
        allocator_(platf::rga::make_cma_dma_allocator()) {
      if (!backend_ || !allocator_) {
        throw std::runtime_error("Vulkan UI failed to initialize RGA resources");
      }
      rebuild_surface(platf::ui::ui_size_e::standard, false);
  #ifdef SUNSHINE_BUILD_VULKAN
      BOOST_LOG(info) << "RKMPP Vulkan UI initialized on " << surface_->renderer->device_name()
                      << ": hidden until Start is held and Select/Back is pressed; standard="
                      << metrics_.standard_panel.width << 'x' << metrics_.standard_panel.height
                      << " profile=" << metrics_.profile_panel.width << 'x' << metrics_.profile_panel.height
                      << " body-font=" << metrics_.body_font_pixels
                      << " title-font=" << metrics_.title_font_pixels
                      << " size=standard"
                      << " ROI=bottom-center margin=" << metrics_.panel_margin;
      platf::ui::global_controller().attach_backend();
      backend_attached_ = true;
  #else
      throw std::runtime_error("Sunshine was built without Vulkan UI support");
  #endif
    }

    /** @brief Report the successfully exercised live-buffer path at teardown. */
    ~rkmpp_vulkan_ui_session_t() {
      disable();
      if (frames_composed_ != 0) {
        BOOST_LOG(info) << "RKMPP Vulkan UI stopped: composed=" << frames_composed_
                        << " capture_generations=" << capture_generations_
                        << " bgr_imported_slots=" << bgr_imported_slots_
                        << " nv12_in_place=" << nv12_frames_composed_;
      }
    }

    /** @brief Return the shared native RGA backend used by UI source imports. */
    platf::rga::backend_t &rga_backend() noexcept {
      return *backend_;
    }

    /** @brief Return the allocator that must outlive UI and converted targets. */
    platf::rga::dma_allocator_t &rga_allocator() noexcept {
      return *allocator_;
    }

    /** @brief Stop presenting this backend and immediately release modal input interception. */
    void disable() noexcept {
      if (!backend_attached_) {
        return;
      }
      backend_attached_ = false;
      platf::ui::global_controller().detach_backend();
      // Release optional CMA surfaces and their DMA-BUF imports before retrying video.
      surface_.reset();
    }

    /** @brief Advance UI state and report whether the current page is visible. */
    bool visible_for_frame(platf::hdmirx::hdmirx_img_t &image) {
      if (!backend_attached_) {
        return false;
      }
      observe_capture_generation(image);
      return prepare_visible_panel(image) != nullptr;
    }

    /** @brief Cover a visible page directly into an exclusively leased BGR capture frame. */
    bool compose_capture_bgr(platf::hdmirx::hdmirx_img_t &image) {
      if (!image.frame || !image.capture_format || image.frame->planes().size() != 1 || image.capture_format->planes.size() != 1) {
        throw std::runtime_error("Vulkan UI requires one live HDMI RX plane");
      }
      const auto &format = *image.capture_format;
      const auto &plane = image.frame->planes().front();
      if (format.mpp_format != MPP_FMT_BGR888) {
        throw std::runtime_error("direct Vulkan UI requires a BGR888 HDMI RX frame");
      }
      if (plane.data_offset != 0) {
        throw std::runtime_error("Vulkan UI does not support a nonzero HDMI RX DMA-BUF data offset");
      }
      const auto minimum_stride = static_cast<std::uint64_t>(format.width) * 3U;
      if (minimum_stride > std::numeric_limits<std::uint32_t>::max() || plane.bytesperline < minimum_stride || plane.bytesperline % 3U != 0 || static_cast<std::uint64_t>(format.height) > std::numeric_limits<std::uint64_t>::max() / plane.bytesperline) {
        throw std::runtime_error("Vulkan UI BGR888 HDMI RX stride is invalid");
      }
      const auto required_size = static_cast<std::uint64_t>(format.height) * plane.bytesperline;
      if (plane.payload_bytes < required_size || plane.sizeimage < required_size || plane.allocation_size < required_size) {
        throw std::runtime_error("Vulkan UI HDMI RX DMA-BUF is smaller than its BGR888 layout");
      }
      return compose_bgr888(
        {.dma_buf_fd = plane.dma_buf_fd,
         .allocation_size = plane.allocation_size,
         .width = format.width,
         .height = format.height,
         .stride = plane.bytesperline,
         .generation = image.frame->generation(),
         .slot = image.frame->buffer_index()},
        image
      );
    }

    /** @brief Synchronously convert the published panel into a leased NV12 ROI. */
    bool compose_capture_nv12(platf::hdmirx::hdmirx_img_t &image) {
      if (!image.direct_ui_write_safe || !image.frame || !image.capture_format || image.frame->planes().size() != 1 || !platf::hdmirx::supports_nv12_ui_cover(*image.capture_format, image.frame->planes().front())) {
        throw std::runtime_error("NV12 UI cover requires an exclusive, complete BT.709 limited capture plane");
      }
      observe_capture_generation(image);
      auto *surface = prepare_visible_panel(image);
      if (!surface) {
        return false;
      }
      const auto &format = *image.capture_format;
      const auto &plane = image.frame->planes().front();
      const auto &panel = surface->panel;
      if (panel.width > format.width || panel.height > format.height || metrics_.panel_margin > format.height - panel.height) {
        throw std::runtime_error("NV12 UI panel exceeds the capture frame");
      }
      const platf::rga::rectangle_t roi {
        ((format.width - panel.width) / 2U) & ~1U,
        (format.height - panel.height - metrics_.panel_margin) & ~1U,
        panel.width,
        panel.height
      };
      if (image.frame_profile) {
        image.frame_profile->ui_compose_begin = std::chrono::steady_clock::now();
      }
  #ifdef SUNSHINE_BUILD_VULKAN
      // publish() waits for Vulkan and releases the linear panel to external devices.
      (void) surface->renderer->publish();
  #endif
      {
        // Do not retain an RGA destination attachment across QBUF or a capture
        // generation change. The caller retains the DQBUF lease through MPP.
        auto destination = platf::rga::imported_buffer_t::import(*backend_, {plane.dma_buf_fd, format.width, format.height, plane.bytesperline, plane.allocation_size, platf::rga::pixel_format_e::nv12});
        platf::rga::process(surface->source.rga_buffer(), {0, 0, panel.width, panel.height}, destination, roi, platf::rga::color_space_e::rgb_to_yuv_bt709_limited);
      }
      if (image.frame_profile) {
        image.frame_profile->ui_compose_end = std::chrono::steady_clock::now();
      }
      ++frames_composed_;
      if (++nv12_frames_composed_ == 1) {
        BOOST_LOG(info) << "RKMPP Vulkan UI first in-place NV12 RGA cover completed synchronously";
      }
      return true;
    }

    /** @brief Cover a visible page directly into a session-private BGR target. */
    bool compose_private_bgr(platf::rga::target_buffer_t &target, std::uint64_t generation, std::uint32_t slot, platf::hdmirx::hdmirx_img_t &image) {
      const auto &layout = target.layout();
      if (layout.format != platf::rga::pixel_format_e::bgr888) {
        throw std::runtime_error("Vulkan UI private destination is not BGR888");
      }
      return compose_bgr888(
        {.dma_buf_fd = layout.dma_buf_fd,
         .allocation_size = layout.allocation_size,
         .width = layout.width,
         .height = layout.height,
         .stride = layout.stride,
         .generation = generation,
         .slot = slot},
        image
      );
    }

  private:
    /** @brief Publish and copy the selected panel into one BGR DMA-BUF ROI. */
    bool compose_bgr888(const platf::vulkan_ui::bgr888_dma_buf_t &target, platf::hdmirx::hdmirx_img_t &image) {
      if (!backend_attached_) {
        return false;
      }
      observe_capture_generation(image);
      auto *surface = prepare_visible_panel(image);
      if (!surface) {
        return false;
      }
      if (image.frame_profile) {
        image.frame_profile->ui_compose_begin = std::chrono::steady_clock::now();
      }
  #ifdef SUNSHINE_BUILD_VULKAN
      (void) surface->renderer->publish();
      if (surface->renderer->cover_bgr888(target, metrics_.panel_margin)) {
        ++bgr_imported_slots_;
      }
  #endif
      if (image.frame_profile) {
        image.frame_profile->ui_compose_end = std::chrono::steady_clock::now();
      }
      ++frames_composed_;
      if (frames_composed_ == 1) {
        BOOST_LOG(info) << "RKMPP Vulkan UI first direct BGR DMA-BUF cover completed synchronously"sv;
      }
      return true;
    }

    /** @brief One page-family surface with matching RGA and Vulkan resources. */
    struct surface_t {
      platf::vulkan_ui::panel_layout_t panel;
      platf::rga::target_buffer_t source;
  #ifdef SUNSHINE_BUILD_VULKAN
      std::unique_ptr<platf::vulkan_ui::renderer_t> renderer;
  #endif
    };

    /**
     * @brief Allocate one stable page-family surface for this encoded output.
     *
     * @param panel Visible panel dimensions.
     * @param metrics Typography and spacing matching @p panel.
     * @return Fully initialized RGA and Vulkan surface.
     */
    std::unique_ptr<surface_t> make_surface(
      const platf::vulkan_ui::panel_layout_t &panel,
      const platf::vulkan_ui::layout_metrics_t &metrics
    ) {
      auto surface = std::make_unique<surface_t>();
      surface->panel = panel;
      surface->source = platf::rga::target_buffer_t::allocate_bgr888(
        *backend_,
        *allocator_,
        panel.width,
        panel.height,
        platf::vulkan_ui::make_bgr888_panel_stride(panel.width)
      );
  #ifdef SUNSHINE_BUILD_VULKAN
      const auto &layout = surface->source.layout();
      surface->renderer = platf::vulkan_ui::renderer_t::create(
        layout.dma_buf_fd,
        layout.allocation_size,
        layout.width,
        layout.height,
        layout.stride,
        metrics
      );
  #endif
      return surface;
    }

    /**
     * @brief Keep only the active page-family surface within the CMA budget.
     *
     * Release the old surface and renderer imports before changing page family
     * or size. A failed change recreates the last working layout for rendering.
     *
     * @param size Requested UI size tier.
     * @param profile Whether the visible page needs the wider Profile layout.
     */
    void rebuild_surface(platf::ui::ui_size_e size, bool profile) {
      const auto request = std::pair {size, profile};
      if (failed_surface_request_ && *failed_surface_request_ != request) {
        failed_surface_request_.reset();
      }
      if (surface_ && ((rendered_ui_size_ == size && rendered_profile_ == profile) || failed_surface_request_ == request)) {
        return;
      }
      const auto next_metrics = platf::vulkan_ui::make_layout_metrics(output_width_, output_height_, size);
      const auto previous_panel = surface_ ? std::make_optional(surface_->panel) : std::nullopt;
      try {
        platf::vulkan_ui::replace_surface_without_overlap(
          surface_,
          [&] {
            return make_surface(profile ? next_metrics.profile_panel : next_metrics.standard_panel, next_metrics);
          },
          [&] {
            return previous_panel ? make_surface(*previous_panel, metrics_) : nullptr;
          }
        );
      } catch (const std::exception &e) {
        if (!surface_) {
          throw;
        }
        failed_surface_request_ = request;
        BOOST_LOG(error) << "RKMPP Vulkan UI surface change failed; restored the last working layout: " << e.what();
        return;
      }
      metrics_ = next_metrics;
      rendered_ui_size_ = size;
      rendered_profile_ = profile;
      failed_surface_request_.reset();
      BOOST_LOG(info) << "RKMPP Vulkan UI active surface: size=" << static_cast<unsigned int>(size)
                      << " profile=" << profile
                      << " dimensions=" << surface_->panel.width << 'x' << surface_->panel.height
                      << " body-font=" << metrics_.body_font_pixels;
    }

    /** @brief Convert one collector metric to the renderer-independent UI form. */
    static platf::ui::profile_metric_status_t make_profile_metric(const frame_profile_metric_snapshot_t &metric) noexcept {
      return {
        .count = metric.count,
        .missing = metric.missing,
        .invalid = metric.invalid,
        .p50_us = metric.p50_us,
        .p95_us = metric.p95_us,
        .p99_us = metric.p99_us
      };
    }

    /** @brief Select the completed-window fields retained by the Vulkan UI page. */
    static platf::ui::profile_status_t make_profile_status(const frame_profile_snapshot_t &snapshot) noexcept {
      constexpr std::array metrics {
        frame_profile_metric_e::rx_ready_wait,
        frame_profile_metric_e::rx_dequeue,
        frame_profile_metric_e::capture_queue,
        frame_profile_metric_e::rga,
        frame_profile_metric_e::prepared_queue,
        frame_profile_metric_e::mpp_encode,
        frame_profile_metric_e::encoded_queue,
        frame_profile_metric_e::packetize_send,
        frame_profile_metric_e::protocol_host,
        frame_profile_metric_e::host_send
      };
      platf::ui::profile_status_t status {
        .captured_frames = snapshot.captured_frames,
        .placeholder_frames = snapshot.placeholder_frames,
        .repeated_frames = snapshot.repeated_frames,
        .rga_bypass_frames = snapshot.rga_bypass_frames,
        .freshness_drops = snapshot.freshness_drops,
        .raw_replaced = snapshot.raw_replaced,
        .prepared_replaced = snapshot.prepared_replaced,
        .target_waits = snapshot.target_waits,
        .sticky_idr_transfers = snapshot.sticky_idr_transfers,
        .dropped_samples = snapshot.dropped_samples,
        .hdmirx_width = snapshot.hdmirx_width,
        .hdmirx_height = snapshot.hdmirx_height,
        .moonlight_width = snapshot.moonlight_width,
        .moonlight_height = snapshot.moonlight_height,
        .available = true
      };
      for (std::size_t index = 0; index < metrics.size(); ++index) {
        status.metrics[index] = make_profile_metric(snapshot.metrics[static_cast<std::size_t>(metrics[index])]);
      }
      return status;
    }

    /** @brief Invalidate renderer-side capture identities before checking UI visibility. */
    void observe_capture_generation(const platf::hdmirx::hdmirx_img_t &image) {
      if (!image.frame || !image.capture_format || image.frame->planes().size() != 1 || image.capture_format->planes.size() != 1) {
        return;
      }
      const auto generation = image.frame->generation();
      if (capture_generation_ && *capture_generation_ == generation) {
        return;
      }
  #ifdef SUNSHINE_BUILD_VULKAN
      if (surface_) {
        surface_->renderer->invalidate_capture_targets();
      }
  #endif
      capture_generation_ = generation;
      ++capture_generations_;
      const auto &format = *image.capture_format;
      const auto &plane = image.frame->planes().front();
      BOOST_LOG(info) << "RKMPP Vulkan UI capture generation=" << generation
                      << " dimensions=" << format.width << 'x' << format.height
                      << " stride=" << plane.bytesperline
                      << " allocation=" << plane.allocation_size
                      << "; invalidated direct-BGR imports";
    }

    /** @brief Render the current controller snapshot once and report visibility. */
    surface_t *prepare_visible_panel(platf::hdmirx::hdmirx_img_t &image) {
      auto &controller = platf::ui::global_controller();
      const auto now = platf::ui::controller_t::clock_t::now();
      if (!last_gamepad_poll_ || now - *last_gamepad_poll_ >= 100ms) {
        gamepad_status_ = input::xbox_remote_status();
        last_gamepad_poll_ = now;
      }
      if (!last_profile_poll_ || now - *last_profile_poll_ >= 100ms) {
        bool profile_changed = false;
        frame_profile_snapshot_t profile;
        if (frame_profile_snapshot_store().read_newer(profile_generation_, profile)) {
          auto statistics = make_profile_status(profile);
          statistics.timeline = profile_status_.timeline;
          profile_status_ = std::move(statistics);
          profile_changed = true;
        }
        frame_profile_timeline_snapshot_t timeline;
        if (frame_profile_timeline_store().read_newer(timeline_generation_, timeline)) {
          profile_status_.timeline = std::move(timeline);
          profile_changed = true;
        }
        if (profile_changed) {
          controller.update_profile(profile_status_);
        }
        last_profile_poll_ = now;
      }
      const auto &gamepad = gamepad_status_;
      const bool gamepad_required = gamepad.selected;
      controller.update_connection({.video_state = std::string {platf::input_sm::state_name(image.connection_state)}, .gamepad_state = gamepad_required ? gamepad.state : "not_required", .gamepad_stage = gamepad.stage, .failure_kind = gamepad.failure_kind, .moonlight_width = image.moonlight_width, .moonlight_height = image.moonlight_height, .moonlight_fps_x100 = output_fps_x100_, .input_width = image.input_width, .input_height = image.input_height, .video_ready = platf::input_sm::is_streaming_state(image.connection_state), .gamepad_ready = !gamepad_required || gamepad.state == "ready"}, now);
      const auto transition = controller.tick(now);
      if (transition.visibility_changed) {
        BOOST_LOG(info) << "RKMPP Vulkan UI " << (transition.visible ? "opened" : "closed")
                        << " after ordered controller UI chord";
      }
      const auto snapshot = controller.snapshot();
      if (!snapshot.visible) {
        return nullptr;
      }
  #ifdef SUNSHINE_BUILD_VULKAN
      rebuild_surface(snapshot.ui_size, snapshot.page == platf::ui::page_e::profile);
      auto &surface = *surface_;
      const auto model = platf::vulkan_ui::make_render_model(surface.panel.width, surface.panel.height, snapshot);
      const auto ui_render_begin = std::chrono::steady_clock::now();
      if (surface.renderer->render(model)) {
        if (image.frame_profile) {
          image.frame_profile->ui_render_begin = ui_render_begin;
          image.frame_profile->ui_render_end = std::chrono::steady_clock::now();
        }
        BOOST_LOG(info) << "RKMPP Vulkan UI rendered revision=" << snapshot.revision
                        << " page=" << static_cast<unsigned int>(snapshot.page)
                        << " focus=" << static_cast<unsigned int>(snapshot.focus);
      }
      return &surface;
  #else
      return nullptr;
  #endif
    }

    std::uint32_t output_width_ {};
    std::uint32_t output_height_ {};
    std::uint32_t output_fps_x100_ {};
    platf::vulkan_ui::layout_metrics_t metrics_;
    std::unique_ptr<platf::rga::backend_t> backend_;
    std::unique_ptr<platf::rga::dma_allocator_t> allocator_;
    std::unique_ptr<surface_t> surface_;  ///< Only the visible page family owns a CMA surface.
    std::optional<platf::ui::ui_size_e> rendered_ui_size_;  ///< Size tier of the active surface.
    bool rendered_profile_ {};  ///< Whether the active surface has the wider Profile layout.
    std::optional<std::pair<platf::ui::ui_size_e, bool>> failed_surface_request_;  ///< Suppress a failed request until selection changes.
    std::optional<std::uint64_t> capture_generation_;
    input::xbox_remote_status_t gamepad_status_;  ///< Last sanitized gamepad lifecycle poll.
    std::optional<std::chrono::steady_clock::time_point> last_gamepad_poll_;  ///< Limits lifecycle polling on the frame path.
    std::uint64_t profile_generation_ {};  ///< Last completed-window publication copied for the UI.
    std::uint64_t timeline_generation_ {};  ///< Last completed-frame Timeline publication copied for the UI.
    std::optional<std::chrono::steady_clock::time_point> last_profile_poll_;  ///< Limits profile-store polling on the frame path.
    platf::ui::profile_status_t profile_status_;  ///< Locally merged statistics and Timeline snapshots.
    std::uint64_t frames_composed_ {};
    std::uint64_t capture_generations_ {};
    std::uint64_t bgr_imported_slots_ {};
    std::uint64_t nv12_frames_composed_ {};  ///< Successful synchronous capture ROI conversions.
    bool backend_attached_ {};  ///< Whether controller input may currently be intercepted for this renderer.
  };

  /** @brief Session-private RGA/Vulkan stage owned by the preprocess worker. */
  class rkmpp_preprocessor_t: public std::enable_shared_from_this<rkmpp_preprocessor_t> {
  public:
    using prepared_t = platf::rkmpp::prepared_frame_t;
    using reclaim_callback_t = std::function<void(platf::hdmirx::hdmirx_img_t &)>;

    /**
     * @brief Create the session UI/RGA state on its permanent worker thread.
     *
     * @param config Negotiated Moonlight encode configuration.
     */
    explicit rkmpp_preprocessor_t(const config_t &config):
        target_resolution_ {static_cast<std::uint32_t>(config.width), static_cast<std::uint32_t>(config.height)} {
      if (::config::video.vulkan_ui) {
        try {
          vulkan_ui_ = std::make_shared<rkmpp_vulkan_ui_session_t>(
            target_resolution_.width,
            target_resolution_.height,
            static_cast<std::uint32_t>(config.framerateX100 > 0 ? config.framerateX100 : config.framerate * 100)
          );
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "RKMPP Vulkan UI initialization failed; continuing without overlay: " << e.what();
        }
      }
      if (vulkan_ui_) {
        rga_backend_ = &vulkan_ui_->rga_backend();
        rga_allocator_ = &vulkan_ui_->rga_allocator();
      } else {
        owned_rga_backend_ = platf::rga::make_backend();
        owned_rga_allocator_ = platf::rga::make_cma_dma_allocator();
        rga_backend_ = owned_rga_backend_.get();
        rga_allocator_ = owned_rga_allocator_.get();
      }
      if (!rga_backend_ || !rga_allocator_) {
        throw std::runtime_error("Failed to initialize RKMPP preprocess RGA resources");
      }
    }

    /** @brief Report bounded target-pool usage after every holder is released. */
    ~rkmpp_preprocessor_t() {
      BOOST_LOG(info) << "RKMPP preprocess stopped: targets_peak=" << target_peak_leases_ << "/2"
                      << " target_waits=" << target_waits_;
    }

    /**
     * @brief Prepare one raw image without invoking MPP.
     *
     * @param raw Shared capture fan-out object.
     * @param stop_token Worker cancellation token used by target waits.
     * @param reclaim_oldest Callback that may discard one stale prepared frame.
     * @return Immutable prepared input, or empty after cancellation/failure.
     */
    std::optional<prepared_t> prepare(
      const std::shared_ptr<platf::img_t> &raw,
      std::stop_token stop_token,
      const reclaim_callback_t &reclaim_oldest
    ) {
      auto image = std::dynamic_pointer_cast<platf::hdmirx::hdmirx_img_t>(raw);
      if (!image) {
        throw std::runtime_error("RKMPP preprocess received a non-HDMI image");
      }
      if (image->frame_profile) {
        image->frame_profile->capture_queue_exit = std::chrono::steady_clock::now();
      }
      return platf::rkmpp::prepare_with_optional_ui(
        prepare_ui_visibility(*image),
        [&](bool ui_visible) -> std::optional<prepared_t> {
          if (image->placeholder) {
            return prepare_placeholder(*image, stop_token, reclaim_oldest, ui_visible);
          }
          if (!image->frame || !image->capture_format || image->frame->planes().size() != 1) {
            throw std::runtime_error("RKMPP preprocess received incomplete HDMI frame metadata");
          }

          const auto &plane = image->frame->planes().front();
          const auto live_layout = platf::rkmpp::make_input_layout_from_plane(
            image->capture_format->width,
            image->capture_format->height,
            image->capture_format->mpp_format,
            plane.bytesperline,
            plane.sizeimage
          );
          if (!live_layout) {
            throw std::runtime_error("RKMPP preprocess source frame has an invalid post-recovery layout");
          }
          const platf::hdmirx::resolution_t input_resolution {live_layout->visible_width, live_layout->visible_height};
          const bool dimensions_match = !platf::hdmirx::needs_conversion(input_resolution, target_resolution_);
          const auto ui_path = platf::rkmpp::select_ui_preprocess_path(
            live_layout->format,
            dimensions_match,
            ui_visible,
            image->direct_ui_write_safe,
            platf::hdmirx::supports_nv12_ui_cover(*image->capture_format, plane)
          );
          if (!dimensions_match || ui_path == platf::rkmpp::ui_preprocess_path_e::private_bgr) {
            return prepare_rga(
              *image,
              *live_layout,
              stop_token,
              reclaim_oldest,
              platf::rkmpp::prepared_route_e::rga,
              ui_path == platf::rkmpp::ui_preprocess_path_e::private_bgr
            );
          }
          if (ui_path == platf::rkmpp::ui_preprocess_path_e::direct_bgr) {
            compose_direct_bgr(*image);
          } else if (ui_path == platf::rkmpp::ui_preprocess_path_e::direct_nv12) {
            compose_direct_nv12(*image);
          }

          if (image->frame_profile) {
            image->frame_profile->rga_used = false;
            image->frame_profile->preprocess_end = std::chrono::steady_clock::now();
          }
          const auto generation = image->frame->generation();
          const auto index = image->frame->buffer_index();
          prepared_t prepared {
            .layout = *live_layout,
            .dma_buf_fd = plane.dma_buf_fd,
            .allocation_size = plane.allocation_size,
            .pts = static_cast<std::int64_t>(image->frame->timestamp().time_since_epoch().count()),
            .generation = generation,
            .cache_key = {generation, index},
            .holder = std::move(image->frame),
            .route = platf::rkmpp::prepared_route_e::direct,
            .request_idr = image->request_idr,
            .profile = std::move(image->frame_profile),
            .frame_timestamp = image->frame_timestamp
          };
          image->capture_format.reset();
          return prepared;
        },
        [&](const std::system_error &error) {
          vulkan_ui_enabled_ = false;
          if (vulkan_ui_) {
            vulkan_ui_->disable();
          }
          BOOST_LOG(warning) << "RKMPP UI buffer allocation failed; continuing this session without overlay: " << error.what();
        }
      );
    }

  private:
    /** @brief One private target and its stable import-cache slot. */
    struct target_slot_t {
      std::shared_ptr<platf::rga::target_buffer_t> buffer;
      std::uint32_t index {};
    };

    /** @brief Derive the exact MPP input layout for one private RGA target. */
    static platf::rkmpp::input_layout_t encoder_layout_for(const platf::rga::target_buffer_t &target) {
      const auto &layout = target.layout();
      const auto mpp_format = layout.format == platf::rga::pixel_format_e::bgr888 ? MPP_FMT_BGR888 : MPP_FMT_YUV420SP;
      const auto encoder_layout = platf::rkmpp::make_input_layout_from_plane(
        layout.width,
        layout.height,
        mpp_format,
        layout.stride,
        layout.allocation_size
      );
      if (!encoder_layout) {
        throw std::runtime_error("Failed to derive RKMPP preprocess target layout");
      }
      return *encoder_layout;
    }

    /** @brief Allocate one target matching the required safe UI path. */
    std::shared_ptr<platf::rga::target_buffer_t> allocate_target(platf::rga::pixel_format_e format) {
      if (format == platf::rga::pixel_format_e::bgr888) {
        return std::make_shared<platf::rga::target_buffer_t>(
          platf::rga::target_buffer_t::allocate_bgr888(
            *rga_backend_,
            *rga_allocator_,
            target_resolution_.width,
            target_resolution_.height
          )
        );
      }
      if (format == platf::rga::pixel_format_e::nv12) {
        return std::make_shared<platf::rga::target_buffer_t>(
          platf::rga::target_buffer_t::allocate_nv12(
            *rga_backend_,
            *rga_allocator_,
            target_resolution_.width,
            target_resolution_.height
          )
        );
      }
      throw std::runtime_error("Unsupported RKMPP preprocess target format");
    }

    /**
     * @brief Allocate or safely rebuild the fixed two-target pool.
     *
     * A visibility transition can change the required target from NV12 to BGR
     * or back. The stale prepared frame is reclaimed first, then any target
     * currently owned by MPP is returned before the two-buffer pool is rebuilt.
     */
    bool ensure_targets(
      platf::rga::pixel_format_e format,
      platf::hdmirx::hdmirx_img_t &image,
      std::stop_token stop_token,
      const reclaim_callback_t &reclaim_oldest
    ) {
      std::unique_lock lock(pool_mutex_);
      if (targets_allocated_ && target_format_ == format) {
        return true;
      }
      if (targets_allocated_) {
        lock.unlock();
        reclaim_oldest(image);
        lock.lock();
        if (target_leases_ != 0) {
          ++target_waits_;
          if (image.frame_profile) {
            ++image.frame_profile->target_waits;
          }
          if (!pool_cv_.wait(lock, stop_token, [this] {
                return target_leases_ == 0;
              })) {
            return false;
          }
        }
        std::queue<target_slot_t> empty;
        free_targets_.swap(empty);
        targets_allocated_ = false;
        target_format_.reset();
      }
      lock.unlock();

      std::array<std::shared_ptr<platf::rga::target_buffer_t>, 2> replacements {
        allocate_target(format),
        allocate_target(format)
      };

      lock.lock();
      for (std::uint32_t index = 0; index < replacements.size(); ++index) {
        free_targets_.push({std::move(replacements[index]), index});
      }
      target_generation_ = next_target_generation_.fetch_add(1, std::memory_order_relaxed);
      target_format_ = format;
      targets_allocated_ = true;
      return true;
    }

    /**
     * @brief Lease a target, first reclaiming an obsolete prepared frame.
     *
     * @param image Current raw image that inherits displaced control state.
     * @param stop_token Cancellation token for a genuine two-target wait.
     * @param reclaim_oldest Prepared-frame reclamation callback.
     * @param format Required format of every target in the active pool.
     * @return Target lease, or empty when cancellation wins.
     */
    std::optional<target_slot_t> take_target(
      platf::hdmirx::hdmirx_img_t &image,
      std::stop_token stop_token,
      const reclaim_callback_t &reclaim_oldest,
      platf::rga::pixel_format_e format
    ) {
      if (!ensure_targets(format, image, stop_token, reclaim_oldest)) {
        return std::nullopt;
      }
      std::unique_lock lock(pool_mutex_);
      if (free_targets_.empty()) {
        lock.unlock();
        reclaim_oldest(image);
        lock.lock();
      }
      if (free_targets_.empty()) {
        ++target_waits_;
        if (image.frame_profile) {
          ++image.frame_profile->target_waits;
        }
        if (!pool_cv_.wait(lock, stop_token, [this] {
              return !free_targets_.empty();
            })) {
          return std::nullopt;
        }
      }
      auto target = std::move(free_targets_.front());
      free_targets_.pop();
      ++target_leases_;
      target_peak_leases_ = std::max(target_peak_leases_, target_leases_);
      return target;
    }

    /** @brief Create a holder that returns one target from the encode worker. */
    platf::rkmpp::input_holder_t retain_target(target_slot_t target) {
      auto self = shared_from_this();
      return {target.buffer.get(), [self = std::move(self), target = std::move(target)](void *) mutable {
                {
                  std::lock_guard lock(self->pool_mutex_);
                  self->free_targets_.push(std::move(target));
                  --self->target_leases_;
                }
                self->pool_cv_.notify_one();
              }};
    }

    /** @brief Fill and optionally decorate the private no-signal target. */
    std::optional<prepared_t> prepare_placeholder(
      platf::hdmirx::hdmirx_img_t &image,
      std::stop_token stop_token,
      const reclaim_callback_t &reclaim_oldest,
      bool ui_visible
    ) {
      const auto target_format = ui_visible ? platf::rga::pixel_format_e::bgr888 : platf::rga::pixel_format_e::nv12;
      auto target = take_target(image, stop_token, reclaim_oldest, target_format);
      if (!target) {
        return std::nullopt;
      }
      const auto target_index = target->index;
      auto target_buffer = target->buffer;
      const auto target_generation = target_generation_;
      const auto encoder_layout = encoder_layout_for(*target_buffer);
      auto holder = retain_target(std::move(*target));
      if (image.frame_profile) {
        image.frame_profile->rga_used = true;
        image.frame_profile->rga_begin = std::chrono::steady_clock::now();
      }
      platf::rga::fill(target_buffer->rga_buffer(), {0, 0, target_resolution_.width, target_resolution_.height}, 0xff00ff00U);
      if (image.frame_profile) {
        image.frame_profile->rga_end = std::chrono::steady_clock::now();
      }
      if (ui_visible) {
        compose_private_bgr(*target_buffer, target_generation, target_index, image);
      }
      if (image.frame_profile) {
        image.frame_profile->preprocess_end = std::chrono::steady_clock::now();
      }
      return prepared_t {
        .layout = encoder_layout,
        .dma_buf_fd = target_buffer->layout().dma_buf_fd,
        .allocation_size = target_buffer->layout().allocation_size,
        .pts = image.frame_timestamp ? static_cast<std::int64_t>(image.frame_timestamp->time_since_epoch().count()) : 0,
        .generation = target_generation,
        .cache_key = {target_generation, target_index},
        .holder = std::move(holder),
        .route = platf::rkmpp::prepared_route_e::placeholder,
        .request_idr = image.request_idr,
        .profile = std::move(image.frame_profile),
        .frame_timestamp = image.frame_timestamp
      };
    }

    /** @brief Convert a real frame into the private NV12 or BGR target selected for this route. */
    std::optional<prepared_t> prepare_rga(
      platf::hdmirx::hdmirx_img_t &image,
      const platf::rkmpp::input_layout_t &source_layout,
      std::stop_token stop_token,
      const reclaim_callback_t &reclaim_oldest,
      platf::rkmpp::prepared_route_e route,
      bool ui_visible
    ) {
      const auto source_format = rga_format_from_mpp(source_layout.format);
      if (!source_format) {
        throw std::runtime_error("Unsupported V4L2 format for RKMPP RGA preprocessing");
      }
      const auto target_format = ui_visible ? platf::rga::pixel_format_e::bgr888 : platf::rga::pixel_format_e::nv12;
      auto target = take_target(image, stop_token, reclaim_oldest, target_format);
      if (!target) {
        return std::nullopt;
      }
      const auto target_index = target->index;
      auto target_buffer = target->buffer;
      const auto target_generation = target_generation_;
      const auto encoder_layout = encoder_layout_for(*target_buffer);
      auto holder = retain_target(std::move(*target));
      const auto &plane = image.frame->planes().front();
      platf::rga::image_layout_t source {
        .dma_buf_fd = plane.dma_buf_fd,
        .width = source_layout.visible_width,
        .height = source_layout.visible_height,
        .stride = plane.bytesperline,
        .allocation_size = plane.allocation_size,
        .format = *source_format
      };
      if (image.frame_profile) {
        image.frame_profile->rga_used = true;
        image.frame_profile->rga_begin = std::chrono::steady_clock::now();
      }
      auto imported_source = platf::rga::imported_buffer_t::import(*rga_backend_, source);
      const auto viewport = platf::hdmirx::make_viewport(
        {source.width, source.height},
        target_resolution_,
        target_format == platf::rga::pixel_format_e::nv12 ? platf::hdmirx::pixel_format_e::nv12 : platf::hdmirx::pixel_format_e::generic
      );
      if (!viewport) {
        throw std::runtime_error("Failed to calculate RKMPP preprocess viewport");
      }
      if (!platf::hdmirx::viewport_covers_target(*viewport, target_resolution_)) {
        platf::rga::fill(target_buffer->rga_buffer(), {0, 0, target_resolution_.width, target_resolution_.height}, 0xff000000U);
      }
      platf::rga::process(
        imported_source,
        {viewport->source.left, viewport->source.top, viewport->source.width, viewport->source.height},
        target_buffer->rga_buffer(),
        {viewport->destination.left, viewport->destination.top, viewport->destination.width, viewport->destination.height}
      );
      if (image.frame_profile) {
        image.frame_profile->rga_end = std::chrono::steady_clock::now();
      }
      if (ui_visible) {
        compose_private_bgr(*target_buffer, target_generation, target_index, image);
      }
      if (image.frame_profile) {
        image.frame_profile->preprocess_end = std::chrono::steady_clock::now();
      }
      const auto pts = static_cast<std::int64_t>(image.frame->timestamp().time_since_epoch().count());
      image.frame.reset();
      image.capture_format.reset();
      return prepared_t {
        .layout = encoder_layout,
        .dma_buf_fd = target_buffer->layout().dma_buf_fd,
        .allocation_size = target_buffer->layout().allocation_size,
        .pts = pts,
        .generation = target_generation,
        .cache_key = {target_generation, target_index},
        .holder = std::move(holder),
        .route = route,
        .request_idr = image.request_idr,
        .profile = std::move(image.frame_profile),
        .frame_timestamp = image.frame_timestamp
      };
    }

    /** @brief Advance controller/UI state while keeping failures session-local. */
    bool prepare_ui_visibility(platf::hdmirx::hdmirx_img_t &image) {
      if (!vulkan_ui_ || !vulkan_ui_enabled_) {
        return false;
      }
      try {
        return vulkan_ui_->visible_for_frame(image);
      } catch (const std::exception &e) {
        vulkan_ui_->disable();
        vulkan_ui_enabled_ = false;
        BOOST_LOG(error) << "RKMPP Vulkan UI preparation failed; disabling overlay for this session: " << e.what();
      }
      return false;
    }

    /** @brief Cover a matching, exclusively published BGR capture frame in place. */
    void compose_direct_bgr(platf::hdmirx::hdmirx_img_t &image) {
      if (!vulkan_ui_ || !vulkan_ui_enabled_) {
        return;
      }
      try {
        vulkan_ui_->compose_capture_bgr(image);
      } catch (const std::exception &e) {
        vulkan_ui_->disable();
        vulkan_ui_enabled_ = false;
        BOOST_LOG(error) << "RKMPP direct BGR Vulkan UI composition failed; disabling overlay for this session: " << e.what();
      }
    }

    /** @brief Cover a validated exclusive NV12 capture frame before MPP submission. */
    void compose_direct_nv12(platf::hdmirx::hdmirx_img_t &image) {
      if (!vulkan_ui_ || !vulkan_ui_enabled_) {
        return;
      }
      try {
        vulkan_ui_->compose_capture_nv12(image);
      } catch (const std::exception &e) {
        vulkan_ui_->disable();
        vulkan_ui_enabled_ = false;
        // A failed DMA write may have partially modified this frame. Let the
        // existing session recovery discard it rather than encoding it.
        throw std::runtime_error(std::string("RKMPP in-place NV12 UI composition failed: ") + e.what());
      }
    }

    /** @brief Cover a session-private BGR target without an RGA UI copy. */
    void compose_private_bgr(platf::rga::target_buffer_t &target, std::uint64_t generation, std::uint32_t slot, platf::hdmirx::hdmirx_img_t &image) {
      if (!vulkan_ui_ || !vulkan_ui_enabled_) {
        return;
      }
      try {
        vulkan_ui_->compose_private_bgr(target, generation, slot, image);
      } catch (const std::exception &e) {
        vulkan_ui_->disable();
        vulkan_ui_enabled_ = false;
        BOOST_LOG(error) << "RKMPP private BGR Vulkan UI composition failed; disabling overlay for this session: " << e.what();
      }
    }

    std::shared_ptr<rkmpp_vulkan_ui_session_t> vulkan_ui_;  ///< Declared before targets so its allocator outlives them.
    std::unique_ptr<platf::rga::backend_t> owned_rga_backend_;
    std::unique_ptr<platf::rga::dma_allocator_t> owned_rga_allocator_;
    platf::rga::backend_t *rga_backend_ {};
    platf::rga::dma_allocator_t *rga_allocator_ {};
    platf::hdmirx::resolution_t target_resolution_;
    std::mutex pool_mutex_;
    std::condition_variable_any pool_cv_;
    std::queue<target_slot_t> free_targets_;
    std::uint64_t target_generation_ {};
    std::uint32_t target_leases_ {};
    std::uint32_t target_peak_leases_ {};
    std::uint64_t target_waits_ {};
    bool targets_allocated_ {};
    std::optional<platf::rga::pixel_format_e> target_format_;  ///< Format shared by both live targets.
    bool vulkan_ui_enabled_ {true};
    inline static std::atomic<std::uint64_t> next_target_generation_ {1};
  };

  /** @brief RKMPP encode session with an internal latest-only preprocess worker. */
  class rkmpp_encode_session_t final: public encode_session_t {
  public:
    using prepared_ptr_t = std::shared_ptr<platf::rkmpp::prepared_frame_t>;

    rkmpp_encode_session_t(const config_t &config, const platf::rkmpp::input_layout_t &):
        config_(config),
        video_format_(config.videoFormat) {}

    ~rkmpp_encode_session_t() override {
      current_prepared_.reset();
      stop_preprocess();
      synchronous_preprocessor_.reset();
    }

    /** @brief Synchronously prepare one probe input outside the streaming pipeline. */
    int convert(platf::img_t &img) override {
      try {
        if (!synchronous_preprocessor_) {
          synchronous_preprocessor_ = std::make_shared<rkmpp_preprocessor_t>(config_);
        }
        auto alias = std::shared_ptr<platf::img_t>(&img, [](platf::img_t *) {
        });
        std::stop_source stop;
        auto prepared = synchronous_preprocessor_->prepare(alias, stop.get_token(), [](platf::hdmirx::hdmirx_img_t &) {
        });
        if (!prepared) {
          return -1;
        }
        current_prepared_ = std::make_shared<platf::rkmpp::prepared_frame_t>(std::move(*prepared));
        return 0;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "RKMPP synchronous preprocess failed: " << e.what();
        return -1;
      }
    }

    /**
     * @brief Start the per-session worker that exclusively owns RGA and Vulkan.
     *
     * @param images Shared raw latest-only capture event.
     * @return True after worker initialization succeeds.
     */
    bool start_preprocess(const img_event_t &images) {
      prepared_event_ = std::make_shared<safe::event_t<prepared_ptr_t>>();
      {
        std::lock_guard lock(worker_mutex_);
        worker_initialized_ = false;
        worker_error_.clear();
      }
      preprocess_thread_ = std::jthread {[this, images](std::stop_token stop_token) {
        try {
          platf::set_thread_name("video::preprocess");
          platf::adjust_thread_priority(platf::thread_priority_e::high);
          auto preprocessor = std::make_shared<rkmpp_preprocessor_t>(config_);
          signal_worker_initialized({});
          while (!stop_token.stop_requested() && images->running()) {
            auto raw = images->pop(stop_token);
            if (!raw || stop_token.stop_requested()) {
              break;
            }
            auto prepared = preprocessor->prepare(raw, stop_token, [this](platf::hdmirx::hdmirx_img_t &replacement) {
              auto displaced = prepared_event_->try_pop();
              if (displaced) {
                transfer_displaced(*displaced, replacement);
              }
            });
            if (!prepared || stop_token.stop_requested()) {
              break;
            }
            auto next = std::make_shared<platf::rkmpp::prepared_frame_t>(std::move(*prepared));
            prepared_event_->raise_latest(next, [](prepared_ptr_t &displaced, prepared_ptr_t &replacement) {
              merge_displaced(*displaced, *replacement);
            });
          }
          if (auto pending = prepared_event_->try_pop()) {
            pending.reset();
          }
          prepared_event_->stop();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "RKMPP preprocess worker failed: " << e.what();
          signal_worker_initialized(e.what());
          if (prepared_event_) {
            if (auto pending = prepared_event_->try_pop()) {
              pending.reset();
            }
            prepared_event_->stop();
          }
        }
      }};
      std::unique_lock lock(worker_mutex_);
      worker_cv_.wait(lock, [this] {
        return worker_initialized_;
      });
      if (!worker_error_.empty()) {
        BOOST_LOG(error) << "RKMPP preprocess worker initialization failed: " << worker_error_;
        lock.unlock();
        stop_preprocess();
        return false;
      }
      return true;
    }

    /** @brief Stop and join the preprocess worker without stopping the reusable raw event. */
    void stop_preprocess() noexcept {
      current_prepared_.reset();
      if (!preprocess_thread_.joinable()) {
        return;
      }
      preprocess_thread_.request_stop();
      preprocess_thread_.join();
      prepared_event_.reset();
    }

    /** @brief Pop the newest prepared input with bounded lifecycle polling. */
    prepared_ptr_t pop_prepared(std::chrono::milliseconds wait) {
      return prepared_event_ ? prepared_event_->pop(wait) : nullptr;
    }

    /** @brief Return whether the preprocess stage can still publish frames. */
    bool preprocess_running() const noexcept {
      return prepared_event_ && prepared_event_->running();
    }

    /** @brief Install the only prepared input eligible for the next encode call. */
    void set_prepared(prepared_ptr_t prepared) {
      current_prepared_ = std::move(prepared);
      if (current_prepared_ && current_prepared_->profile) {
        current_prepared_->profile->prepared_queue_exit = std::chrono::steady_clock::now();
      }
    }

    /** @brief Return the capture timestamp of the pending prepared input. */
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp() const noexcept {
      return current_prepared_ ? current_prepared_->frame_timestamp : std::nullopt;
    }

    void request_idr_frame() override {
      force_idr_ = true;
    }

    void request_normal_frame() override {}

    void invalidate_ref_frames(int64_t, int64_t) override {}

    /** @brief Submit the pending immutable input exactly once on the encode worker. */
    platf::rkmpp::encoded_packet_t encode() {
      if (!current_prepared_) {
        throw std::runtime_error("RKMPP encode called without a prepared frame");
      }
      const bool layout_changed = !encoder_layout_ || *encoder_layout_ != current_prepared_->layout;
      if (layout_changed) {
        auto replacement = platf::rkmpp::encoder_t::create(make_rkmpp_encoder_config(config_, current_prepared_->layout));
        encoder_ = std::move(replacement);
        encoder_layout_ = current_prepared_->layout;
        input_generation_.reset();
        input_route_.reset();
        force_idr_ = true;
      }
      if (!input_generation_ || *input_generation_ != current_prepared_->generation || !input_route_ || *input_route_ != current_prepared_->route) {
        encoder_.clear_input_cache();
        input_generation_ = current_prepared_->generation;
        input_route_ = current_prepared_->route;
        force_idr_ = true;
      }
      if (force_idr_ || current_prepared_->request_idr) {
        encoder_.request_idr();
        force_idr_ = false;
      }
      auto input = current_prepared_->input_frame();
      auto packet = encoder_.encode_packet(input);
      encoded_profile_ = std::move(current_prepared_->profile);
      current_prepared_.reset();
      return packet;
    }

    int video_format() const noexcept {
      return video_format_;
    }

    /** @brief Move the profile completed by the most recent MPP submission. */
    std::optional<frame_profile_t> take_frame_profile() {
      return std::exchange(encoded_profile_, std::nullopt);
    }

  private:
    /** @brief Transfer a stale prepared frame into a raw replacement before RGA. */
    static void transfer_displaced(platf::rkmpp::prepared_frame_t &displaced, platf::hdmirx::hdmirx_img_t &replacement) {
      if (replacement.frame_profile) {
        ++replacement.frame_profile->prepared_replaced;
        if (displaced.profile) {
          replacement.frame_profile->raw_replaced += displaced.profile->raw_replaced;
          replacement.frame_profile->prepared_replaced += displaced.profile->prepared_replaced;
          replacement.frame_profile->target_waits += displaced.profile->target_waits;
          replacement.frame_profile->sticky_idr_transfers += displaced.profile->sticky_idr_transfers;
        }
      }
      if (displaced.request_idr) {
        replacement.request_idr = true;
        if (replacement.frame_profile) {
          ++replacement.frame_profile->sticky_idr_transfers;
        }
      }
    }

    /** @brief Merge a displaced prepared frame into its latest-only replacement. */
    static void merge_displaced(platf::rkmpp::prepared_frame_t &displaced, platf::rkmpp::prepared_frame_t &replacement) {
      if (replacement.profile) {
        ++replacement.profile->prepared_replaced;
        if (displaced.profile) {
          replacement.profile->raw_replaced += displaced.profile->raw_replaced;
          replacement.profile->prepared_replaced += displaced.profile->prepared_replaced;
          replacement.profile->target_waits += displaced.profile->target_waits;
          replacement.profile->sticky_idr_transfers += displaced.profile->sticky_idr_transfers;
        }
      }
      if (displaced.request_idr) {
        replacement.request_idr = true;
        if (replacement.profile) {
          ++replacement.profile->sticky_idr_transfers;
        }
      }
    }

    /** @brief Publish one initialization result exactly once. */
    void signal_worker_initialized(std::string error) {
      std::lock_guard lock(worker_mutex_);
      if (worker_initialized_) {
        return;
      }
      worker_error_ = std::move(error);
      worker_initialized_ = true;
      worker_cv_.notify_all();
    }

    config_t config_;
    platf::rkmpp::encoder_t encoder_;
    std::optional<platf::rkmpp::input_layout_t> encoder_layout_;
    std::optional<std::uint64_t> input_generation_;
    std::optional<platf::rkmpp::prepared_route_e> input_route_;
    prepared_ptr_t current_prepared_;
    std::optional<frame_profile_t> encoded_profile_;
    std::shared_ptr<rkmpp_preprocessor_t> synchronous_preprocessor_;
    std::shared_ptr<safe::event_t<prepared_ptr_t>> prepared_event_;
    std::jthread preprocess_thread_;
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::string worker_error_;
    bool worker_initialized_ {};
    bool force_idr_ {};
    int video_format_ {};
  };

#endif

  int encode_avcodec(int64_t frame_nr, avcodec_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto &frame = session.device->frame;
    frame->pts = frame_nr;

    auto &ctx = session.avcodec_ctx;

    auto &sps = session.sps;
    auto &vps = session.vps;

    // send the frame to the encoder
    auto ret = avcodec_send_frame(ctx.get(), frame);
    if (ret < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not send a frame for encoding: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, ret);

      return -1;
    }

    while (ret >= 0) {
      auto packet = std::make_unique<packet_raw_avcodec>();
      auto av_packet = packet.get()->av_packet;

      ret = avcodec_receive_packet(ctx.get(), av_packet);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
      } else if (ret < 0) {
        return ret;
      }

      if (av_packet->flags & AV_PKT_FLAG_KEY) {
        BOOST_LOG(debug) << "Frame "sv << frame_nr << ": IDR Keyframe (AV_FRAME_FLAG_KEY)"sv;
      }

      if ((frame->flags & AV_FRAME_FLAG_KEY) && !(av_packet->flags & AV_PKT_FLAG_KEY)) {
        BOOST_LOG(error) << "Encoder did not produce IDR frame when requested!"sv;
      }

      if (session.inject) {
        if (session.inject == 1) {
          auto h264 = cbs::make_sps_h264(ctx.get(), av_packet);

          sps = std::move(h264.sps);
        } else {
          auto hevc = cbs::make_sps_hevc(ctx.get(), av_packet);

          sps = std::move(hevc.sps);
          vps = std::move(hevc.vps);

          session.replacements.emplace_back(
            std::string_view(reinterpret_cast<const char *>(std::begin(vps.old)), vps.old.size()),
            std::string_view(reinterpret_cast<const char *>(std::begin(vps._new)), vps._new.size())
          );
        }

        session.inject = 0;

        session.replacements.emplace_back(
          std::string_view(reinterpret_cast<const char *>(std::begin(sps.old)), sps.old.size()),
          std::string_view(reinterpret_cast<const char *>(std::begin(sps._new)), sps._new.size())
        );
      }

      if (av_packet && av_packet->pts == frame_nr) {
        packet->frame_timestamp = frame_timestamp;
      }

      packet->replacements = &session.replacements;
      packet->channel_data = channel_data;
      packets->raise(std::move(packet));
    }

    return 0;
  }

  /**
   * @brief Encode one frame through NVENC and queue the resulting packet.
   *
   * @param frame_nr Monotonic frame index assigned by the video pipeline.
   * @param session Active NVENC encoder session.
   * @param packets Output queue that receives the encoded packet.
   * @param channel_data Platform or protocol state attached to the packet.
   * @param frame_timestamp Capture timestamp associated with the encoded frame.
   * @return 0 when packets are queued; nonzero when NVENC encoding fails.
   */
  int encode_nvenc(int64_t frame_nr, nvenc_encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    auto encoded_frame = session.encode_frame(frame_nr);
    if (encoded_frame.data.empty()) {
      BOOST_LOG(error) << "NvENC returned empty packet";
      return -1;
    }

    if (frame_nr != encoded_frame.frame_index) {
      BOOST_LOG(error) << "NvENC frame index mismatch " << frame_nr << " " << encoded_frame.frame_index;
    }

    auto packet = std::make_unique<packet_raw_generic>(std::move(encoded_frame.data), encoded_frame.frame_index, encoded_frame.idr);
    packet->channel_data = channel_data;
    packet->after_ref_frame_invalidation = encoded_frame.after_ref_frame_invalidation;
    packet->frame_timestamp = frame_timestamp;
    packets->raise(std::move(packet));

    return 0;
  }

  /**
   * @brief Encode one captured frame and queue packets for transmission.
   *
   * @param frame_nr Frame nr.
   * @param session Active streaming or pairing session for the request.
   * @param packets Packets queued or emitted by the stream.
   * @param channel_data Channel data.
   * @param frame_timestamp Frame timestamp.
   * @return 0 when the frame is encoded and queued; nonzero on encoder failure.
   */
  int encode(int64_t frame_nr, encode_session_t &session, safe::mail_raw_t::queue_t<packet_t> &packets, void *channel_data, std::optional<std::chrono::steady_clock::time_point> frame_timestamp) {
    if (auto avcodec_session = dynamic_cast<avcodec_encode_session_t *>(&session)) {
      return encode_avcodec(frame_nr, *avcodec_session, packets, channel_data, frame_timestamp);
    } else if (auto nvenc_session = dynamic_cast<nvenc_encode_session_t *>(&session)) {
      return encode_nvenc(frame_nr, *nvenc_session, packets, channel_data, frame_timestamp);
#ifdef SUNSHINE_BUILD_RKMPP
    } else if (auto rkmpp_session = dynamic_cast<rkmpp_encode_session_t *>(&session)) {
      try {
        auto t0 = std::chrono::steady_clock::now();
        auto encoded = rkmpp_session->encode();
        auto t1 = std::chrono::steady_clock::now();
        if (frame_timestamp) {
          BOOST_LOG(debug) << "RKMPP capture-to-encode-output latency: " << std::chrono::duration_cast<std::chrono::microseconds>(t1 - *frame_timestamp).count() << " us";
        }
        BOOST_LOG(debug) << "MPP encode latency: " << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() << " us";
        if (!encoded) {
          return -1;
        }
        const bool idr = platf::rkmpp::detail::output_is_idr(
          encoded.output_intra(),
          encoded.data(),
          encoded.size(),
          rkmpp_session->video_format() == 0 ? platf::rkmpp::codec_e::h264 : platf::rkmpp::codec_e::h265
        );
        auto packet = std::make_unique<packet_raw_rkmpp>(std::move(encoded), frame_nr, idr);
        packet->channel_data = channel_data;
        packet->frame_timestamp = frame_timestamp;
        packet->frame_profile = rkmpp_session->take_frame_profile();
        if (packet->frame_profile) {
          packet->frame_profile->frame_index = frame_nr;
        }
        packets->raise(std::move(packet));
        return 0;
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "RKMPP encode failed: " << e.what();
        return -1;
      }
#endif
    }

    return -1;
  }

  /**
   * @brief Create an AVCodec encode session.
   *
   * @param disp Display being encoded.
   * @param encoder Selected encoder.
   * @param config Video configuration.
   * @param width Encoded frame width.
   * @param height Encoded frame height.
   * @param encode_device AVCodec encode device.
   * @return AVCodec encode session, or nullptr on failure.
   */
  std::unique_ptr<avcodec_encode_session_t> make_avcodec_encode_session(
    platf::display_t *disp,
    const encoder_t &encoder,
    const config_t &config,
    int width,
    int height,
    std::unique_ptr<platf::avcodec_encode_device_t> encode_device
  ) {
    auto platform_formats = dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get());
    if (!platform_formats) {
      return nullptr;
    }

    bool hardware = platform_formats->avcodec_base_dev_type != AV_HWDEVICE_TYPE_NONE;

    auto &video_format = encoder.codec_from_config(config);
    if (!video_format[encoder_t::PASSED] || !disp->is_codec_supported(video_format.name, config)) {
      BOOST_LOG(error) << encoder.name << ": "sv << video_format.name << " mode not supported"sv;
      return nullptr;
    }

    if (config.chromaSamplingType == 1) {
      if (!video_format[encoder_t::YUV444]) {
        BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 not supported"sv;
        return nullptr;
      }

      if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(error) << video_format.name << ": YUV 4:4:4 dynamic range not supported"sv;
        return nullptr;
      }

    } else {
      if (config.dynamicRange && !video_format[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(error) << video_format.name << ": dynamic range not supported"sv;
        return nullptr;
      }
    }

    auto codec = avcodec_find_encoder_by_name(video_format.name.c_str());
    if (!codec) {
      BOOST_LOG(error) << "Couldn't open ["sv << video_format.name << ']';

      return nullptr;
    }

    auto colorspace = encode_device->colorspace;
    auto sw_fmt = (colorspace.bit_depth == 8 && config.chromaSamplingType == 0)  ? platform_formats->avcodec_pix_fmt_8bit :
                  (colorspace.bit_depth == 8 && config.chromaSamplingType == 1)  ? platform_formats->avcodec_pix_fmt_yuv444_8bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 0) ? platform_formats->avcodec_pix_fmt_10bit :
                  (colorspace.bit_depth == 10 && config.chromaSamplingType == 1) ? platform_formats->avcodec_pix_fmt_yuv444_10bit :
                                                                                   AV_PIX_FMT_NONE;

    // Allow up to 1 retry to apply the set of fallback options.
    //
    // Note: If we later end up needing multiple sets of
    // fallback options, we may need to allow more retries
    // to try applying each set.
    avcodec_ctx_t ctx;
    for (int retries = 0; retries < 2; retries++) {
      ctx.reset(avcodec_alloc_context3(codec));
      ctx->width = config.width;
      ctx->height = config.height;
      const AVRational fps = video::framerate_to_rational(config);
      ctx->framerate = fps;
      ctx->time_base = AVRational {fps.den, fps.num};

      switch (config.videoFormat) {
        case 0:
          // 10-bit h264 encoding is not supported by our streaming protocol
          assert(!config.dynamicRange);
          ctx->profile = select_h264_profile(video_format.name, config, config::video.amd.amd_coder);
          break;

        case 1:
          if (config.chromaSamplingType == 1) {
            // HEVC uses the same RExt profile for both 8 and 10 bit YUV 4:4:4 encoding
            ctx->profile = AV_PROFILE_HEVC_REXT;
          } else {
            ctx->profile = config.dynamicRange ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
          }
          break;

        case 2:
          // AV1 supports both 8 and 10 bit encoding with the same Main profile
          // but YUV 4:4:4 sampling requires High profile
          ctx->profile = (config.chromaSamplingType == 1) ? AV_PROFILE_AV1_HIGH : AV_PROFILE_AV1_MAIN;
          break;
      }

      // B-frames delay decoder output, so never use them
      ctx->max_b_frames = 0;

      // Use an infinite GOP length since I-frames are generated on demand
      // Exception: encoders with FIXED_GOP_SIZE flag don't support on-demand IDR
      if (encoder.flags & FIXED_GOP_SIZE) {
        // Fixed GOP for encoders that don't support on-demand IDR (e.g. Media Foundation)
        ctx->gop_size = 120;  // ~2 seconds at 60 FPS - larger to reduce oversized IDR frame frequency
        ctx->keyint_min = 120;
      } else {
        ctx->gop_size = encoder.flags & LIMITED_GOP_SIZE ?
                          std::numeric_limits<std::int16_t>::max() :
                          std::numeric_limits<int>::max();
        ctx->keyint_min = std::numeric_limits<int>::max();
      }

      // Some client decoders have limits on the number of reference frames
      if (config.numRefFrames) {
        if (video_format[encoder_t::REF_FRAMES_RESTRICT]) {
          ctx->refs = config.numRefFrames;
        } else {
          BOOST_LOG(warning) << "Client requested reference frame limit, but encoder doesn't support it!"sv;
        }
      }

      // We forcefully reset the flags to avoid clash on reuse of AVCodecContext
      ctx->flags = 0;
      ctx->flags |= AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY;

      ctx->flags2 |= AV_CODEC_FLAG2_FAST;

      auto avcodec_colorspace = avcodec_colorspace_from_sunshine_colorspace(colorspace);

      ctx->color_range = avcodec_colorspace.range;
      ctx->color_primaries = avcodec_colorspace.primaries;
      ctx->color_trc = avcodec_colorspace.transfer_function;
      ctx->colorspace = avcodec_colorspace.matrix;

      // Used by cbs::make_sps_hevc
      ctx->sw_pix_fmt = sw_fmt;

      if (hardware) {
        avcodec_buffer_t encoding_stream_context;

        ctx->pix_fmt = platform_formats->avcodec_dev_pix_fmt;

        // Create the base hwdevice context
        auto buf_or_error = platform_formats->init_avcodec_hardware_input_buffer(encode_device.get());
        if (buf_or_error.has_right()) {
          return nullptr;
        }
        encoding_stream_context = std::move(buf_or_error.left());

        // If this encoder requires derivation from the base, derive the desired type
        if (platform_formats->avcodec_derived_dev_type != AV_HWDEVICE_TYPE_NONE) {
          avcodec_buffer_t derived_context;

          // Allow the hwdevice to prepare for this type of context to be derived
          if (encode_device->prepare_to_derive_context(platform_formats->avcodec_derived_dev_type)) {
            return nullptr;
          }

          auto err = av_hwdevice_ctx_create_derived(&derived_context, platform_formats->avcodec_derived_dev_type, encoding_stream_context.get(), 0);
          if (err) {
            char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
            BOOST_LOG(error) << "Failed to derive device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

            return nullptr;
          }

          encoding_stream_context = std::move(derived_context);
        }

        // Initialize avcodec hardware frames
        {
          avcodec_buffer_t frame_ref {av_hwframe_ctx_alloc(encoding_stream_context.get())};

          auto frame_ctx = (AVHWFramesContext *) frame_ref->data;
          frame_ctx->format = ctx->pix_fmt;
          frame_ctx->sw_format = sw_fmt;
          frame_ctx->height = ctx->height;
          frame_ctx->width = ctx->width;
          frame_ctx->initial_pool_size = 0;

          // Allow the hwdevice to modify hwframe context parameters
          encode_device->init_hwframes(frame_ctx);

          if (auto err = av_hwframe_ctx_init(frame_ref.get()); err < 0) {
            return nullptr;
          }

          ctx->hw_frames_ctx = av_buffer_ref(frame_ref.get());
        }

        ctx->slices = config.slicesPerFrame;
      } else /* software */ {
        ctx->pix_fmt = sw_fmt;

        // Clients will request for the fewest slices per frame to get the
        // most efficient encode, but we may want to provide more slices than
        // requested to ensure we have enough parallelism for good performance.
        ctx->slices = std::max(config.slicesPerFrame, config::video.min_threads);
      }

      if (encoder.flags & SINGLE_SLICE_ONLY) {
        ctx->slices = 1;
      }

      ctx->thread_type = FF_THREAD_SLICE;
      ctx->thread_count = ctx->slices;

      AVDictionary *options {nullptr};
      auto handle_option = [&options, &config](const encoder_t::option_t &option) {
        std::visit(
          util::overloaded {
            [&](int v) {
              av_dict_set_int(&options, option.name.c_str(), v, 0);
            },
            [&](int *v) {
              av_dict_set_int(&options, option.name.c_str(), *v, 0);
            },
            [&](std::optional<int> *v) {
              if (*v) {
                av_dict_set_int(&options, option.name.c_str(), **v, 0);
              }
            },
            [&](const std::function<int()> &v) {
              av_dict_set_int(&options, option.name.c_str(), v(), 0);
            },
            [&](const std::string &v) {
              av_dict_set(&options, option.name.c_str(), v.c_str(), 0);
            },
            [&](std::string *v) {
              if (!v->empty()) {
                av_dict_set(&options, option.name.c_str(), v->c_str(), 0);
              }
            },
            [&](const std::function<const std::string(const config_t &cfg)> &v) {
              av_dict_set(&options, option.name.c_str(), v(config).c_str(), 0);
            }
          },
          option.value
        );
      };

      // Apply common options, then format-specific overrides
      for (auto &option : video_format.common_options) {
        handle_option(option);
      }
      for (auto &option : (config.dynamicRange ? video_format.hdr_options : video_format.sdr_options)) {
        handle_option(option);
      }
      if (config.chromaSamplingType == 1) {
        for (auto &option : (config.dynamicRange ? video_format.hdr444_options : video_format.sdr444_options)) {
          handle_option(option);
        }
      }
      if (retries > 0) {
        for (auto &option : video_format.fallback_options) {
          handle_option(option);
        }
      }

      auto bitrate = ((config::video.max_bitrate > 0) ? std::min(config.bitrate, config::video.max_bitrate) : config.bitrate) * 1000;
      BOOST_LOG(info) << "Streaming bitrate is " << bitrate;
      ctx->rc_max_rate = bitrate;
      ctx->bit_rate = bitrate;

      if (encoder.flags & CBR_WITH_VBR) {
        // Ensure rc_max_bitrate != bit_rate to force VBR mode
        ctx->bit_rate--;
      } else {
        ctx->rc_min_rate = bitrate;
      }

      if (encoder.flags & RELAXED_COMPLIANCE) {
        ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
      }

      if (!(encoder.flags & NO_RC_BUF_LIMIT)) {
        if (!hardware && (ctx->slices > 1 || config.videoFormat == 1)) {
          // Use a larger rc_buffer_size for software encoding when slices are enabled,
          // because libx264 can severely degrade quality if the buffer is too small.
          // libx265 encounters this issue more frequently, so always scale the
          // buffer by 1.5x for software HEVC encoding.
          ctx->rc_buffer_size = bitrate / ((config.framerate * 10) / 15);
        } else {
          ctx->rc_buffer_size = bitrate / config.framerate;

#ifndef __APPLE__
          if (encoder.name == "nvenc" && config::video.nv_legacy.vbv_percentage_increase > 0) {
            ctx->rc_buffer_size += ctx->rc_buffer_size * config::video.nv_legacy.vbv_percentage_increase / 100;
          }
#endif
        }
      }

      // Allow the encoding device a final opportunity to set/unset or override any options
      encode_device->init_codec_options(ctx.get(), &options);

      if (auto status = avcodec_open2(ctx.get(), codec, &options)) {
        char err_str[AV_ERROR_MAX_STRING_SIZE] {0};

        if (!video_format.fallback_options.empty() && retries == 0) {
          BOOST_LOG(info)
            << "Retrying with fallback configuration options for ["sv << video_format.name << "] after error: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          continue;
        } else {
          BOOST_LOG(error)
            << "Could not open codec ["sv
            << video_format.name << "]: "sv
            << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, status);

          return nullptr;
        }
      }

      // Successfully opened the codec
      break;
    }

    avcodec_frame_t frame {av_frame_alloc()};
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    frame->color_range = ctx->color_range;
    frame->color_primaries = ctx->color_primaries;
    frame->color_trc = ctx->color_trc;
    frame->colorspace = ctx->colorspace;
    frame->chroma_location = ctx->chroma_sample_location;

    // Attach HDR metadata to the AVFrame
    if (colorspace_is_hdr(colorspace)) {
      SS_HDR_METADATA hdr_metadata;
      if (disp->get_hdr_metadata(hdr_metadata)) {
        auto mdm = av_mastering_display_metadata_create_side_data(frame.get());

        mdm->display_primaries[0][0] = av_make_q(hdr_metadata.displayPrimaries[0].x, 50000);
        mdm->display_primaries[0][1] = av_make_q(hdr_metadata.displayPrimaries[0].y, 50000);
        mdm->display_primaries[1][0] = av_make_q(hdr_metadata.displayPrimaries[1].x, 50000);
        mdm->display_primaries[1][1] = av_make_q(hdr_metadata.displayPrimaries[1].y, 50000);
        mdm->display_primaries[2][0] = av_make_q(hdr_metadata.displayPrimaries[2].x, 50000);
        mdm->display_primaries[2][1] = av_make_q(hdr_metadata.displayPrimaries[2].y, 50000);

        mdm->white_point[0] = av_make_q(hdr_metadata.whitePoint.x, 50000);
        mdm->white_point[1] = av_make_q(hdr_metadata.whitePoint.y, 50000);

        mdm->min_luminance = av_make_q(hdr_metadata.minDisplayLuminance, 10000);
        mdm->max_luminance = av_make_q(hdr_metadata.maxDisplayLuminance, 1);

        mdm->has_luminance = hdr_metadata.maxDisplayLuminance != 0 ? 1 : 0;
        mdm->has_primaries = hdr_metadata.displayPrimaries[0].x != 0 ? 1 : 0;

        if (hdr_metadata.maxContentLightLevel != 0 || hdr_metadata.maxFrameAverageLightLevel != 0) {
          auto clm = av_content_light_metadata_create_side_data(frame.get());

          clm->MaxCLL = hdr_metadata.maxContentLightLevel;
          clm->MaxFALL = hdr_metadata.maxFrameAverageLightLevel;
        }
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }

    std::unique_ptr<platf::avcodec_encode_device_t> encode_device_final;

    if (!encode_device->data) {
      auto software_encode_device = std::make_unique<avcodec_software_encode_device_t>();

      if (software_encode_device->init(width, height, frame.get(), sw_fmt, hardware)) {
        return nullptr;
      }
      software_encode_device->colorspace = colorspace;

      encode_device_final = std::move(software_encode_device);
    } else {
      encode_device_final = std::move(encode_device);
    }

    if (encode_device_final->set_frame(frame.release(), ctx->hw_frames_ctx)) {
      return nullptr;
    }

    encode_device_final->apply_colorspace();

    auto session = std::make_unique<avcodec_encode_session_t>(
      std::move(ctx),
      std::move(encode_device_final),

      // 0 ==> don't inject, 1 ==> inject for h264, 2 ==> inject for hevc
      config.videoFormat <= 1 ? (1 - static_cast<int>(video_format[encoder_t::VUI_PARAMETERS])) * (1 + config.videoFormat) : 0
    );

    return session;
  }

  /**
   * @brief Create NVENC encode session.
   *
   * @param client_config Client stream configuration negotiated for this session.
   * @param encode_device Encode device.
   * @return Constructed NVENC encode session object.
   */
  std::unique_ptr<nvenc_encode_session_t> make_nvenc_encode_session(const config_t &client_config, std::unique_ptr<platf::nvenc_encode_device_t> encode_device) {
    if (!encode_device->init_encoder(client_config, encode_device->colorspace)) {
      return nullptr;
    }

    return std::make_unique<nvenc_encode_session_t>(std::move(encode_device));
  }

  /**
   * @brief Create encode session.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param encode_device Encode device.
   * @return Constructed encode session object.
   */
  std::unique_ptr<encode_session_t> make_encode_session(platf::display_t *disp, const encoder_t &encoder, const config_t &config, int width, int height, std::unique_ptr<platf::encode_device_t> encode_device) {
    if (dynamic_cast<platf::avcodec_encode_device_t *>(encode_device.get())) {
      auto avcodec_encode_device = boost::dynamic_pointer_cast<platf::avcodec_encode_device_t>(std::move(encode_device));
      return make_avcodec_encode_session(disp, encoder, config, width, height, std::move(avcodec_encode_device));
    } else if (dynamic_cast<platf::nvenc_encode_device_t *>(encode_device.get())) {
      auto nvenc_encode_device = boost::dynamic_pointer_cast<platf::nvenc_encode_device_t>(std::move(encode_device));
      return make_nvenc_encode_session(config, std::move(nvenc_encode_device));
#ifdef SUNSHINE_BUILD_RKMPP
    } else if (auto rkmpp_device = dynamic_cast<platf::rkmpp_encode_device_t *>(encode_device.get())) {
      if (config.videoFormat > 1 || config.dynamicRange || config.chromaSamplingType) {
        BOOST_LOG(error) << "RKMPP supports only 8-bit SDR 4:2:0 H.264 or HEVC";
        return nullptr;
      }
      if (config.numRefFrames != 0) {
        BOOST_LOG(error) << "RKMPP does not support a requested reference-frame count";
        return nullptr;
      }
      if (config.slicesPerFrame > 1) {
        BOOST_LOG(error) << "RKMPP does not support more than one slice per frame";
        return nullptr;
      }
      auto input_layout = static_cast<const platf::rkmpp::input_layout_t *>(rkmpp_device->input_layout());
      if (!input_layout) {
        BOOST_LOG(error) << "RKMPP display did not provide an input layout";
        return nullptr;
      }
      const auto layout_check = make_rkmpp_encoder_config(config, *input_layout);
      const auto layout_status = platf::rkmpp::validate_encoder_config(layout_check);
      if (layout_status == platf::rkmpp::encoder_config_status_e::converter_required) {
        BOOST_LOG(info) << "RKMPP initial HDMI RX " << input_layout->visible_width << "x" << input_layout->visible_height
                        << " does not match requested coded size " << config.width << "x" << config.height
                        << "; using adaptive RGA fallback until matching input is observed.";
      } else if (layout_status != platf::rkmpp::encoder_config_status_e::ok) {
        BOOST_LOG(error) << "RKMPP input layout or encoder configuration is invalid (status=" << static_cast<int>(layout_status) << ')';
        return nullptr;
      }
      return std::make_unique<rkmpp_encode_session_t>(config, *input_layout);
#endif
    }

    return nullptr;
  }

  /**
   * @brief Run one encode loop for a display capture stream.
   *
   * @param frame_nr Frame counter updated as frames are encoded.
   * @param mail Session mail bus.
   * @param images Captured image event source.
   * @param config Video configuration.
   * @param disp Display being encoded.
   * @param encode_device Platform encode device.
   * @param reinit_event Signal raised while the encoder/display is reinitializing.
   * @param encoder Selected encoder.
   * @param channel_data Opaque channel data passed to packets.
   */
  void encode_run(
    int &frame_nr,  // Store progress of the frame number
    safe::mail_t mail,
    img_event_t images,
    config_t config,
    std::shared_ptr<platf::display_t> disp,
    std::unique_ptr<platf::encode_device_t> encode_device,
    safe::signal_t &reinit_event,
    const encoder_t &encoder,
    void *channel_data
  ) {
    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return;
    }

    // As a workaround for NVENC hangs and to generally speed up encoder reinit,
    // we will complete the encoder teardown in a separate thread if supported.
    // This will move expensive processing off the encoder thread to allow us
    // to restart encoding as soon as possible. For cases where the NVENC driver
    // hang occurs, this thread may probably never exit, but it will allow
    // streaming to continue without requiring a full restart of Sunshine.
    auto fail_guard = util::fail_guard([&encoder, &session] {
      if (encoder.flags & ASYNC_TEARDOWN) {
        std::jthread encoder_teardown_thread {[session = std::move(session)]() mutable {
          BOOST_LOG(info) << "Starting async encoder teardown";
          session.reset();
          BOOST_LOG(info) << "Async encoder teardown complete";
        }};
        encoder_teardown_thread.detach();
      }
    });

    // set max frame time based on client-requested target framerate.
    double minimum_fps_target = (config::video.minimum_fps_target > 0.0) ? config::video.minimum_fps_target : (config.framerate / 2);
    std::chrono::duration<double, std::milli> max_frametime {1000.0 / minimum_fps_target};
    BOOST_LOG(info) << "Minimum FPS target set to ~"sv << minimum_fps_target << "fps ("sv << max_frametime.count() << "ms)"sv;

    auto shutdown_event = mail->event<bool>(mail::shutdown);
    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    auto idr_events = mail->event<bool>(mail::idr);
    auto invalidate_ref_frames_events = mail->event<std::pair<int64_t, int64_t>>(mail::invalidate_ref_frames);

#ifdef SUNSHINE_BUILD_RKMPP
    if (auto *rkmpp_session = dynamic_cast<rkmpp_encode_session_t *>(session.get())) {
      if (!rkmpp_session->start_preprocess(images)) {
        shutdown_event->raise(true);
        return;
      }
      auto preprocess_guard = util::fail_guard([rkmpp_session] {
        rkmpp_session->stop_preprocess();
      });
      while (true) {
        while (invalidate_ref_frames_events->peek()) {
          if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
            rkmpp_session->invalidate_ref_frames(frames->first, frames->second);
          }
        }
        if (idr_events->peek()) {
          idr_events->pop();
          rkmpp_session->request_idr_frame();
        }
        if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
          break;
        }

        auto prepared = rkmpp_session->pop_prepared(20ms);
        if (!prepared) {
          // A capture stop or display reinit can race with the blocking pop.
          if (shutdown_event->peek() || !images->running() || reinit_event.peek()) {
            break;
          }
          if (!rkmpp_session->preprocess_running()) {
            BOOST_LOG(error) << "RKMPP preprocess worker stopped before the stream ended; ending session"sv;
            shutdown_event->raise(true);
            return;
          }
          continue;
        }
        if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
          break;
        }
        rkmpp_session->set_prepared(std::move(prepared));
        const auto frame_timestamp = rkmpp_session->frame_timestamp();
        if (encode(frame_nr++, *rkmpp_session, packets, channel_data, frame_timestamp)) {
          BOOST_LOG(error) << "Could not encode RKMPP video packet"sv;
          shutdown_event->raise(true);
          return;
        }
        rkmpp_session->request_normal_frame();
        platf::enable_mouse_keys();
      }
      return;
    }
#endif

    // convert() may retain references into its input until the following encode().
    // Keep the dummy image alive for that interval, just like captured images in
    // the main loop below.
    auto dummy_img = disp->alloc_img();
    if (!dummy_img || disp->dummy_img(dummy_img.get()) || session->convert(*dummy_img)) {
      return;
    }

    bool initial_input_pending = true;
    while (true) {
      bool requested_idr_frame = false;
      bool converted_input = std::exchange(initial_input_pending, false);
      std::shared_ptr<platf::img_t> current_img;

      while (invalidate_ref_frames_events->peek()) {
        if (auto frames = invalidate_ref_frames_events->pop(0ms)) {
          session->invalidate_ref_frames(frames->first, frames->second);
        }
      }

      if (idr_events->peek()) {
        requested_idr_frame = true;
        idr_events->pop();
      }

      if (requested_idr_frame) {
        session->request_idr_frame();
      }

      std::optional<std::chrono::steady_clock::time_point> frame_timestamp;

      // Encode at a minimum FPS to avoid image quality issues with static content
      if (!requested_idr_frame || images->peek()) {
        if (auto img = images->pop(max_frametime)) {
          current_img = std::move(img);
          frame_timestamp = current_img->frame_timestamp;
          if (current_img->frame_profile) {
            current_img->frame_profile->capture_queue_exit = std::chrono::steady_clock::now();
          }
          if (session->convert(*current_img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            return;
          }
          converted_input = true;
        } else if (!images->running()) {
          break;
        }
      }

      // Break out of the encoding loop if any of the following are true:
      // a) The stream is ending
      // b) Sunshine is quitting
      // c) The capture side is waiting to reinit and we've encoded at least one frame
      //
      // If we have to reinit before we have received any captured frames, we will encode
      // the blank dummy frame just to let Moonlight know that we're alive.
      //
      // Ensure that this check occurs as close as possible to the encode call to prevent packets
      // in flight after encoder teardown.
      if (shutdown_event->peek() || !images->running() || (reinit_event.peek() && frame_nr > 1)) {
        break;
      }

      // RKMPP releases its imported DMA-BUF or reusable RGA target after each
      // submission. Wait for the next captured frame instead of resubmitting
      // an already-consumed input when the minimum-FPS timer expires.
      if ((encoder.flags & SINGLE_USE_INPUT) && !converted_input) {
        continue;
      }

      if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp)) {
        BOOST_LOG(error) << "Could not encode video packet"sv;
        return;
      }

      session->request_normal_frame();

      // While streaming check to see if the mouse is present and enable Mouse Keys to force the cursor to appear
      // This is useful for KVM switch scenarios where mouse may disappear during streaming
      platf::enable_mouse_keys();
    }
  }

  /**
   * @brief Create a port object or message.
   *
   * @param display Display object or identifier associated with the operation.
   * @param config Configuration values to apply.
   * @return Constructed port object.
   */
  input::touch_port_t make_port(platf::display_t *display, const config_t &config) {
    float wd = display->width;
    float hd = display->height;

    float wt = config.width;
    float ht = config.height;

    auto scalar = std::fminf(wt / wd, ht / hd);

    // we initialize scalar_tpcoords and logical dimensions to default values in case they are not set (non-KMS)
    float scalar_tpcoords = 1.0f;
    int display_env_logical_width = 0;
    int display_env_logical_height = 0;
    if (display->logical_width > 0 && display->logical_height > 0 && display->env_logical_width > 0 && display->env_logical_height > 0) {
      float lwd = display->logical_width;
      float lhd = display->logical_height;
      scalar_tpcoords = std::fminf(wd / lwd, hd / lhd);
      display_env_logical_width = display->env_logical_width;
      display_env_logical_height = display->env_logical_height;
    }

    auto w2 = scalar * wd;
    auto h2 = scalar * hd;

    auto offsetX = (config.width - w2) * 0.5f;
    auto offsetY = (config.height - h2) * 0.5f;

    return input::touch_port_t {
      {
        display->offset_x,
        display->offset_y,
        config.width,
        config.height,
      },
      display->env_width,
      display->env_height,
      offsetX,
      offsetY,
      1.0f / scalar,
      scalar_tpcoords,
      display_env_logical_width,
      display_env_logical_height
    };
  }

  /**
   * @brief Create encode device.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @return Constructed encode device object.
   */
  std::unique_ptr<platf::encode_device_t> make_encode_device(platf::display_t &disp, const encoder_t &encoder, const config_t &config) {
    std::unique_ptr<platf::encode_device_t> result;

    auto colorspace = colorspace_from_client_config(config, disp.is_hdr());

    platf::pix_fmt_e pix_fmt;
    if (config.chromaSamplingType == 1) {
      // YUV 4:4:4
      if (!(encoder.flags & YUV444_SUPPORT)) {
        // Encoder can't support YUV 4:4:4 regardless of hardware capabilities
        return {};
      }
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_yuv444_10bit :
                  encoder.platform_formats->pix_fmt_yuv444_8bit;
    } else {
      // YUV 4:2:0
      pix_fmt = (colorspace.bit_depth == 10) ?
                  encoder.platform_formats->pix_fmt_10bit :
                  encoder.platform_formats->pix_fmt_8bit;
    }

    {
      auto encoder_name = encoder.codec_from_config(config).name;

      BOOST_LOG(info) << "Creating encoder " << logging::bracket(encoder_name);

      auto color_coding = colorspace.colorspace == colorspace_e::bt2020    ? "HDR (Rec. 2020 + SMPTE 2084 PQ)" :
                          colorspace.colorspace == colorspace_e::rec601    ? "SDR (Rec. 601)" :
                          colorspace.colorspace == colorspace_e::rec709    ? "SDR (Rec. 709)" :
                          colorspace.colorspace == colorspace_e::bt2020sdr ? "SDR (Rec. 2020)" :
                                                                             "unknown";

      BOOST_LOG(info) << "Color coding: " << color_coding;
      BOOST_LOG(info) << "Color depth: " << colorspace.bit_depth << "-bit";
      BOOST_LOG(info) << "Color range: " << (colorspace.full_range ? "JPEG" : "MPEG");
    }

    if (dynamic_cast<const encoder_platform_formats_avcodec *>(encoder.platform_formats.get())) {
      result = disp.make_avcodec_encode_device(pix_fmt);
    } else if (dynamic_cast<const encoder_platform_formats_nvenc *>(encoder.platform_formats.get())) {
      result = disp.make_nvenc_encode_device(pix_fmt);
#ifdef SUNSHINE_BUILD_RKMPP
    } else if (dynamic_cast<const encoder_platform_formats_rkmpp *>(encoder.platform_formats.get())) {
      if (config.videoFormat > 1 || config.dynamicRange || config.chromaSamplingType) {
        return {};
      }
      result = disp.make_rkmpp_encode_device();
#endif
    }

    if (result) {
      result->colorspace = colorspace;
    }

    return result;
  }

  /**
   * @brief Create synced session.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param img Image or frame object to read from or populate.
   * @param ctx Native context object used by the operation or callback.
   * @return Constructed synced session object.
   */
  std::optional<sync_session_t> make_synced_session(platf::display_t *disp, const encoder_t &encoder, platf::img_t &img, sync_session_ctx_t &ctx) {
    sync_session_t encode_session;

    encode_session.ctx = &ctx;

    auto encode_device = make_encode_device(*disp, encoder, ctx.config);
    if (!encode_device) {
      return std::nullopt;
    }

    // absolute mouse coordinates require that the dimensions of the screen are known
    ctx.touch_port_events->raise(make_port(disp, ctx.config));

    // Update client with our current HDR display state
    hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
    if (colorspace_is_hdr(encode_device->colorspace)) {
      if (disp->get_hdr_metadata(hdr_info->metadata)) {
        hdr_info->enabled = true;
      } else {
        BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
      }
    }
    ctx.hdr_events->raise(std::move(hdr_info));

    auto session = make_encode_session(disp, encoder, ctx.config, img.width, img.height, std::move(encode_device));
    if (!session) {
      return std::nullopt;
    }

    // Load the initial image to prepare for encoding
    if (session->convert(img)) {
      BOOST_LOG(error) << "Could not convert initial image"sv;
      return std::nullopt;
    }

    encode_session.session = std::move(session);

    return encode_session;
  }

  /**
   * @brief Run synchronized capture and encoding.
   *
   * @param synced_session_ctxs Active synchronized session contexts.
   * @param encode_session_ctx_queue Pending synchronized session context queue.
   * @param display_names Cached display names.
   * @param display_p Active display index.
   * @return Encoder loop result.
   */
  encode_e encode_run_sync(
    std::vector<std::unique_ptr<sync_session_ctx_t>> &synced_session_ctxs,
    encode_session_ctx_queue_t &encode_session_ctx_queue,
    std::vector<std::string> &display_names,
    int &display_p
  ) {
    const auto &encoder = *chosen_encoder;

    std::shared_ptr<platf::display_t> disp;

    auto switch_display_event = mail::man->event<int>(mail::switch_display);

    if (synced_session_ctxs.empty()) {
      auto ctx = encode_session_ctx_queue.pop();
      if (!ctx) {
        return encode_e::ok;
      }

      synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*ctx)));
    }

    while (encode_session_ctx_queue.running()) {
      // Refresh display names since a display removal might have caused the reinitialization
      refresh_displays(encoder.platform_formats->dev_type, display_names, display_p);

      // Process any pending display switch with the new list of displays
      if (switch_display_event->peek()) {
        display_p = std::clamp(*switch_display_event->pop(), 0, static_cast<int>(display_names.size()) - 1);
      }

      // reset_display() will sleep between retries
      reset_display(disp, encoder.platform_formats->dev_type, display_names[display_p], synced_session_ctxs.front()->config);
      if (disp) {
        break;
      }
#ifdef SUNSHINE_BUILD_RKMPP
      // HDMI RX cannot scale. A display-open failure therefore includes a
      // client request whose dimensions do not match the active input. Do not
      // keep retrying after that client has disconnected: the synchronous
      // session's join event is only raised when this function returns.
      if (encoder.name == "rkmpp") {
        BOOST_LOG(error) << "RKMPP/HDMI RX capture setup failed; ending the streaming session";
        break;
      }
#endif
    }

    if (!disp) {
      return encode_e::error;
    }

    auto img = disp->alloc_img();
    if (!img || disp->dummy_img(img.get())) {
      return encode_e::error;
    }

    std::vector<sync_session_t> synced_sessions;
    for (auto &ctx : synced_session_ctxs) {
      auto synced_session = make_synced_session(disp.get(), encoder, *img, *ctx);
      if (!synced_session) {
        return encode_e::error;
      }

      synced_sessions.emplace_back(std::move(*synced_session));
    }

    auto ec = platf::capture_e::ok;
    while (encode_session_ctx_queue.running()) {
      auto push_captured_image_callback = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) -> bool {
        while (encode_session_ctx_queue.peek()) {
#ifdef SUNSHINE_BUILD_RKMPP
          if (encoder.name == "rkmpp") {
            BOOST_LOG(error) << "RKMPP/HDMI RX currently supports one streaming session at a time";
            auto rejected = encode_session_ctx_queue.pop();
            if (rejected) {
              rejected->shutdown_event->raise(true);
              rejected->join_event->raise(true);
            }
            continue;
          }
#endif
          auto encode_session_ctx = encode_session_ctx_queue.pop();
          if (!encode_session_ctx) {
            return false;
          }

          synced_session_ctxs.emplace_back(std::make_unique<sync_session_ctx_t>(std::move(*encode_session_ctx)));

          auto encode_session = make_synced_session(disp.get(), encoder, *img, *synced_session_ctxs.back());
          if (!encode_session) {
            ec = platf::capture_e::error;
            return false;
          }

          synced_sessions.emplace_back(std::move(*encode_session));
        }

        KITTY_WHILE_LOOP(auto pos = std::begin(synced_sessions), pos != std::end(synced_sessions), {
          auto ctx = pos->ctx;
          if (ctx->shutdown_event->peek()) {
            // Let waiting thread know it can delete shutdown_event
            ctx->join_event->raise(true);

            pos = synced_sessions.erase(pos);
            synced_session_ctxs.erase(std::find_if(std::begin(synced_session_ctxs), std::end(synced_session_ctxs), [&ctx_p = ctx](auto &ctx) {
              return ctx.get() == ctx_p;
            }));

            if (synced_sessions.empty()) {
              return false;
            }

            continue;
          }

          if (ctx->idr_events->peek()) {
            pos->session->request_idr_frame();
            ctx->idr_events->pop();
          }

          if (frame_captured && img->frame_profile)
            img->frame_profile->capture_queue_exit = std::chrono::steady_clock::now();
          if (frame_captured && pos->session->convert(*img)) {
            BOOST_LOG(error) << "Could not convert image"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
          if (img) {
            frame_timestamp = img->frame_timestamp;
          }

          if (encode(ctx->frame_nr++, *pos->session, ctx->packets, ctx->channel_data, frame_timestamp)) {
            BOOST_LOG(error) << "Could not encode video packet"sv;
            ctx->shutdown_event->raise(true);

            continue;
          }

          pos->session->request_normal_frame();

          ++pos;
        })

        if (switch_display_event->peek()) {
          ec = platf::capture_e::reinit;
          return false;
        }

        return true;
      };

      auto pull_free_image_callback = [&img](std::shared_ptr<platf::img_t> &img_out) -> bool {
        img_out = img;
        img_out->frame_timestamp.reset();
        img_out->frame_profile.reset();
        return true;
      };

      auto status = disp->capture(push_captured_image_callback, pull_free_image_callback, &display_cursor);
      switch (status) {
        case platf::capture_e::reinit:
        case platf::capture_e::error:
        case platf::capture_e::ok:
        case platf::capture_e::timeout:
        case platf::capture_e::interrupted:
          return ec != platf::capture_e::ok ? ec : status;
      }
    }

    return encode_e::ok;
  }

  /**
   * @brief Run synchronous capture and encode work on the capture thread.
   */
  void captureThreadSync() {
    auto ref = capture_thread_sync.ref();

    std::vector<std::unique_ptr<sync_session_ctx_t>> synced_session_ctxs;

    auto &ctx = ref->encode_session_ctx_queue;
    auto lg = util::fail_guard([&]() {
      ctx.stop();

      for (auto &ctx : synced_session_ctxs) {
        ctx->shutdown_event->raise(true);
        ctx->join_event->raise(true);
      }

      for (auto &ctx : ctx.unsafe()) {
        ctx.shutdown_event->raise(true);
        ctx.join_event->raise(true);
      }
    });

    // Encoding and capture takes place on this thread
    platf::set_thread_name("video::capture_sync");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    std::vector<std::string> display_names;
    int display_p = -1;
    while (encode_run_sync(synced_session_ctxs, ctx, display_names, display_p) == encode_e::reinit) {
#ifdef SUNSHINE_BUILD_RKMPP
      // HDMI source-change notifications can arrive before new DV timings are
      // stable. Avoid reopening the V4L2 device in a tight loop.
      if (chosen_encoder && chosen_encoder->name == "rkmpp") {
        std::this_thread::sleep_for(200ms);
      }
#endif
    }
  }

  /**
   * @brief Capture and encode video using the asynchronous capture thread.
   *
   * @param mail Session mail bus.
   * @param config Video configuration.
   * @param channel_data Opaque channel data passed to packets.
   */
  void capture_async(
    safe::mail_t mail,
    config_t &config,
    void *channel_data
  ) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);

    auto images = std::make_shared<img_event_t::element_type>();
    auto lg = util::fail_guard([&]() {
      images->stop();
      shutdown_event->raise(true);
    });

    auto ref = capture_thread_async.ref();
    if (!ref) {
      return;
    }

    ref->capture_ctx_queue->raise(capture_ctx_t {images, config});

    if (!ref->capture_ctx_queue->running()) {
      return;
    }

    int frame_nr = 1;

    auto touch_port_event = mail->event<input::touch_port_t>(mail::touch_port);
    auto hdr_event = mail->event<hdr_info_t>(mail::hdr);

    // Encoding takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    while (!shutdown_event->peek() && images->running()) {
      // Wait for the main capture event when the display is being reinitialized
      if (ref->reinit_event.peek()) {
        std::this_thread::sleep_for(20ms);
        continue;
      }
      // Wait for the display to be ready
      std::shared_ptr<platf::display_t> display;
      {
        auto lg = ref->display_wp.lock();
        if (ref->display_wp->expired()) {
          continue;
        }

        display = ref->display_wp->lock();
      }

      auto &encoder = *chosen_encoder;

      auto encode_device = make_encode_device(*display, encoder, config);
      if (!encode_device) {
        return;
      }

      // absolute mouse coordinates require that the dimensions of the screen are known
      touch_port_event->raise(make_port(display.get(), config));

      // Update client with our current HDR display state
      hdr_info_t hdr_info = std::make_unique<hdr_info_raw_t>(false);
      if (colorspace_is_hdr(encode_device->colorspace)) {
        if (display->get_hdr_metadata(hdr_info->metadata)) {
          hdr_info->enabled = true;
        } else {
          BOOST_LOG(error) << "Couldn't get display hdr metadata when colorspace selection indicates it should have one";
        }
      }
      hdr_event->raise(std::move(hdr_info));

      encode_run(
        frame_nr,
        mail,
        images,
        config,
        display,
        std::move(encode_device),
        ref->reinit_event,
        *ref->encoder_p,
        channel_data
      );
    }
  }

  /**
   * @brief Capture and encode video for a streaming session.
   *
   * @param mail Session mail bus.
   * @param config Video configuration.
   * @param channel_data Opaque channel data passed to packets.
   */
  void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data
  ) {
    auto idr_events = mail->event<bool>(mail::idr);

    idr_events->raise(true);
    if (chosen_encoder->flags & PARALLEL_ENCODING) {
      capture_async(std::move(mail), config, channel_data);
    } else {
      safe::signal_t join_event;
      auto ref = capture_thread_sync.ref();
      ref->encode_session_ctx_queue.raise(sync_session_ctx_t {
        &join_event,
        mail->event<bool>(mail::shutdown),
        mail::man->queue<packet_t>(mail::video_packets),
        std::move(idr_events),
        mail->event<hdr_info_t>(mail::hdr),
        mail->event<input::touch_port_t>(mail::touch_port),
        config,
        1,
        channel_data,
      });

      // Wait for join signal
      join_event.view();
    }
  }

  /**
   * @brief Enumerates supported validate flag options.
   */
  enum validate_flag_e {
    VUI_PARAMS = 0x01,  ///< VUI parameters
  };

  /**
   * @brief Validate config before it is used.
   *
   * @param disp Display connection or display handle.
   * @param encoder Encoder configuration or encoder instance.
   * @param config Configuration values to apply.
   * @return 0 when the selected encoder/device accepts the configuration; nonzero otherwise.
   */
  int validate_config(std::shared_ptr<platf::display_t> disp, const encoder_t &encoder, const config_t &config) {
    auto encode_device = make_encode_device(*disp, encoder, config);
    if (!encode_device) {
      return -1;
    }

    auto session = make_encode_session(disp.get(), encoder, config, disp->width, disp->height, std::move(encode_device));
    if (!session) {
      return -1;
    }

    // The encoder contract permits convert() to retain references into the input
    // until encode() consumes it. Keep the probe image alive through that call.
    auto img = disp->alloc_img();
    if (!img || disp->dummy_img(img.get()) || session->convert(*img)) {
      return -1;
    }

    session->request_idr_frame();

    auto packets = mail::man->queue<packet_t>(mail::video_packets);
    while (!packets->peek()) {
      if (encode(1, *session, packets, nullptr, {})) {
        return -1;
      }
    }

    auto packet = packets->pop();
    if (!packet->is_idr()) {
      BOOST_LOG(error) << "First packet type is not an IDR frame"sv;

      return -1;
    }

#ifdef SUNSHINE_BUILD_RKMPP
    if (encoder.name == "rkmpp") {
      if (!platf::rkmpp::detail::annexb_first_vcl_is_idr(
            packet->data(),
            packet->data_size(),
            config.videoFormat == 0 ? platf::rkmpp::codec_e::h264 : platf::rkmpp::codec_e::h265,
            true
          )) {
        BOOST_LOG(error) << "RKMPP probe packet is missing required parameter sets or a real IDR NAL"sv;
        return -1;
      }
    }
#endif

    int flag = 0;

    // This check only applies for H.264 and HEVC
    if (config.videoFormat <= 1) {
      if (auto packet_avcodec = dynamic_cast<packet_raw_avcodec *>(packet.get())) {
        if (cbs::validate_sps(packet_avcodec->av_packet, config.videoFormat ? AV_CODEC_ID_H265 : AV_CODEC_ID_H264)) {
          flag |= VUI_PARAMS;
        }
      } else {
        // Don't check it for non-avcodec encoders.
        flag |= VUI_PARAMS;
      }
    }

    return flag;
  }

  /**
   * @brief Validate encoder before it is used.
   */
  bool validate_encoder(encoder_t &encoder, bool expect_failure) {
    auto output_name {display_device::map_output_name(config::video.output_name)};
    // HDMI RX has one logical display. During startup validation the normal
    // output setting is commonly empty, but unlike the desktop backends the
    // HDMI RX display requires its enumerated name.
    if (config::video.capture == "hdmirx" && output_name.empty()) {
      const auto display_names = platf::display_names(encoder.platform_formats->dev_type);
      if (!display_names.empty()) {
        output_name = display_names.front();
      }
    }
    std::shared_ptr<platf::display_t> disp;

    BOOST_LOG(info) << "Trying encoder ["sv << encoder.name << ']';
    auto fg = util::fail_guard([&]() {
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] failed"sv;
    });

    auto test_hevc = active_hevc_mode >= 2 || (active_hevc_mode == 0 && !(encoder.flags & H264_ONLY));
    auto test_av1 = active_av1_mode >= 2 || (active_av1_mode == 0 && !(encoder.flags & H264_ONLY));

    encoder.h264.capabilities.set();
    encoder.hevc.capabilities.set();
    encoder.av1.capabilities.set();

    // First, test encoder viability
    config_t config_max_ref_frames {1920, 1080, 60, 6000, 1000, 1, 1, 1, 0, 0, 0};
    config_t config_autoselect {1920, 1080, 60, 6000, 1000, 1, 0, 1, 0, 0, 0};

    // If the encoder isn't supported at all (not even H.264), bail early
    reset_display(disp, encoder.platform_formats->dev_type, output_name, config_autoselect, platf::display_purpose_e::encoder_probe);
    if (!disp) {
      return false;
    }
    if (!disp->is_codec_supported(encoder.h264.name, config_autoselect)) {
      fg.disable();
      BOOST_LOG(info) << "Encoder ["sv << encoder.name << "] is not supported on this GPU"sv;
      return false;
    }

    // If we're expecting failure, use the autoselect ref config first since that will always succeed
    // if the encoder is available.
    auto max_ref_frames_h264 = expect_failure ? -1 : validate_config(disp, encoder, config_max_ref_frames);
    auto autoselect_h264 = max_ref_frames_h264 >= 0 ? max_ref_frames_h264 : validate_config(disp, encoder, config_autoselect);
    if (autoselect_h264 < 0) {
      return false;
    } else if (expect_failure) {
      // We expected failure, but actually succeeded. Do the max_ref_frames probe we skipped.
      max_ref_frames_h264 = validate_config(disp, encoder, config_max_ref_frames);
    }

    std::vector<std::pair<validate_flag_e, encoder_t::flag_e>> packet_deficiencies {
      {VUI_PARAMS, encoder_t::VUI_PARAMETERS},
    };

    for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
      encoder.h264[encoder_flag] = (max_ref_frames_h264 & validate_flag && autoselect_h264 & validate_flag);
    }

    encoder.h264[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_h264 >= 0;
    encoder.h264[encoder_t::PASSED] = true;

    if (test_hevc) {
      config_max_ref_frames.videoFormat = 1;
      config_autoselect.videoFormat = 1;

      if (disp->is_codec_supported(encoder.hevc.name, config_autoselect)) {
        auto max_ref_frames_hevc = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // HEVC to also succeed with max ref frames specified if HEVC is supported.
        auto autoselect_hevc = (max_ref_frames_hevc >= 0 || max_ref_frames_h264 >= 0) ?
                                 max_ref_frames_hevc :
                                 validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.hevc[encoder_flag] = (max_ref_frames_hevc & validate_flag && autoselect_hevc & validate_flag);
        }

        encoder.hevc[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_hevc >= 0;
        encoder.hevc[encoder_t::PASSED] = max_ref_frames_hevc >= 0 || autoselect_hevc >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.hevc.name << "] is not supported on this GPU"sv;
        encoder.hevc.capabilities.reset();
      }
    } else {
      // Clear all cap bits for HEVC if we didn't probe it
      encoder.hevc.capabilities.reset();
    }

    if (test_av1) {
      config_max_ref_frames.videoFormat = 2;
      config_autoselect.videoFormat = 2;

      if (disp->is_codec_supported(encoder.av1.name, config_autoselect)) {
        auto max_ref_frames_av1 = validate_config(disp, encoder, config_max_ref_frames);

        // If H.264 succeeded with max ref frames specified, assume that we can count on
        // AV1 to also succeed with max ref frames specified if AV1 is supported.
        auto autoselect_av1 = (max_ref_frames_av1 >= 0 || max_ref_frames_h264 >= 0) ?
                                max_ref_frames_av1 :
                                validate_config(disp, encoder, config_autoselect);

        for (auto [validate_flag, encoder_flag] : packet_deficiencies) {
          encoder.av1[encoder_flag] = (max_ref_frames_av1 & validate_flag && autoselect_av1 & validate_flag);
        }

        encoder.av1[encoder_t::REF_FRAMES_RESTRICT] = max_ref_frames_av1 >= 0;
        encoder.av1[encoder_t::PASSED] = max_ref_frames_av1 >= 0 || autoselect_av1 >= 0;
      } else {
        BOOST_LOG(info) << "Encoder ["sv << encoder.av1.name << "] is not supported on this GPU"sv;
        encoder.av1.capabilities.reset();
      }
    } else {
      // Clear all cap bits for AV1 if we didn't probe it
      encoder.av1.capabilities.reset();
    }

    // Test HDR and YUV444 support
    {
      auto test_yuv444 = [&](auto &flag_map, auto video_format) {
        const config_t config = {1920, 1080, 60, 6000, 1000, 1, 0, 1, video_format, 0, 1};
        reset_display(disp, encoder.platform_formats->dev_type, output_name, config, platf::display_purpose_e::encoder_probe);
        if (!disp) {
          return;
        }
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if ((encoder.flags & YUV444_SUPPORT) && disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::YUV444] = true;
        } else {
          flag_map[encoder_t::YUV444] = false;
        }
      };

      auto test_yuv420_hdr = [&](auto &flag_map, auto video_format) {
        const config_t config = {1920, 1080, 60, 6000, 1000, 1, 0, 3, video_format, 1, 0};
        reset_display(disp, encoder.platform_formats->dev_type, output_name, config, platf::display_purpose_e::encoder_probe);
        if (!disp) {
          return;
        }
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if (disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE] = true;
        } else {
          flag_map[encoder_t::DYNAMIC_RANGE] = false;
        }
      };

      auto test_yuv444_hdr = [&](auto &flag_map, auto video_format) {
        const config_t config = {1920, 1080, 60, 6000, 1000, 1, 0, 3, video_format, 1, 1};
        reset_display(disp, encoder.platform_formats->dev_type, output_name, config, platf::display_purpose_e::encoder_probe);
        if (!disp) {
          return;
        }
        if (!flag_map[encoder_t::PASSED]) {
          return;
        }

        auto encoder_codec_name = encoder.codec_from_config(config).name;

        if ((encoder.flags & YUV444_SUPPORT) && disp->is_codec_supported(encoder_codec_name, config) && validate_config(disp, encoder, config) >= 0) {
          flag_map[encoder_t::DYNAMIC_RANGE_YUV444] = true;
        } else {
          flag_map[encoder_t::DYNAMIC_RANGE_YUV444] = false;
        }
      };

      test_yuv444(encoder.h264, 0);
      // HDR is not supported with H.264. Don't bother even trying it.
      encoder.h264[encoder_t::DYNAMIC_RANGE] = false;
      encoder.h264[encoder_t::DYNAMIC_RANGE_YUV444] = false;

      test_yuv444(encoder.hevc, 1);
      test_yuv420_hdr(encoder.hevc, 1);
      test_yuv444_hdr(encoder.hevc, 1);
      test_yuv444(encoder.av1, 2);
      test_yuv420_hdr(encoder.av1, 2);
      test_yuv444_hdr(encoder.av1, 2);
    }

    encoder.h264[encoder_t::VUI_PARAMETERS] = encoder.h264[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];
    encoder.hevc[encoder_t::VUI_PARAMETERS] = encoder.hevc[encoder_t::VUI_PARAMETERS] && !config::sunshine.flags[config::flag::FORCE_VIDEO_HEADER_REPLACE];

    if (!encoder.h264[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": h264 missing sps->vui parameters"sv;
    }
    if (encoder.hevc[encoder_t::PASSED] && !encoder.hevc[encoder_t::VUI_PARAMETERS]) {
      BOOST_LOG(warning) << encoder.name << ": hevc missing sps->vui parameters"sv;
    }

    fg.disable();
    return true;
  }

  int probe_encoders() {
    if (!allow_encoder_probing()) {
      // Error already logged
      return -1;
    }

    auto encoder_list = encoders;

    // If we already have a good encoder, check to see if another probe is required
    if (chosen_encoder && !(chosen_encoder->flags & ALWAYS_REPROBE) && !platf::needs_encoder_reenumeration()) {
      return 0;
    }

    // Restart encoder selection
    auto previous_encoder = chosen_encoder;
    chosen_encoder = nullptr;
    active_hevc_mode = config::video.hevc_mode;
    active_av1_mode = config::video.av1_mode;
    last_encoder_probe_supported_ref_frames_invalidation = false;

    auto adjust_encoder_constraints_hevc = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_hevc_mode == 5 && !encoder->hevc[encoder_t::DYNAMIC_RANGE] && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 Rext10_444 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 4 && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Rext10_444 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC Main10 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 2 && !encoder->hevc[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support HEVC on this system"sv;
        active_hevc_mode = 0;
      }
    };

    auto adjust_encoder_constraints_av1 = [&](encoder_t *encoder) {
      // If we can't satisfy both the encoder and codec requirement, prefer the encoder over codec support
      if (active_av1_mode == 5 && !encoder->av1[encoder_t::DYNAMIC_RANGE] && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 Rext10_444 on this system"sv;
        active_av1_mode = 0;
      } else if (active_hevc_mode == 4 && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Rext10_444 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 Main10 on this system"sv;
        active_hevc_mode = 0;
      } else if (active_av1_mode == 2 && !encoder->av1[encoder_t::PASSED]) {
        BOOST_LOG(warning) << "Encoder ["sv << encoder->name << "] does not support AV1 on this system"sv;
        active_av1_mode = 0;
      }
    };

    if (!config::video.encoder.empty()) {
      // If there is a specific encoder specified, use it if it passes validation
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        if (encoder->name == config::video.encoder) {
          // Remove the encoder from the list entirely if it fails validation
          if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
            pos = encoder_list.erase(pos);
            break;
          }

          // We will return an encoder here even if it fails one of the codec requirements specified by the user
          adjust_encoder_constraints_hevc(encoder);
          adjust_encoder_constraints_av1(encoder);

          chosen_encoder = encoder;
          break;
        }

        pos++;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder matching ["sv << config::video.encoder << ']';
      }
    }

    BOOST_LOG(info) << "// Testing for available encoders, this may generate errors. You can safely ignore those errors. //"sv;

    // If we haven't found an encoder yet, but we want one with specific codec support, search for that now.
    if (chosen_encoder == nullptr && (active_hevc_mode >= 2 || active_av1_mode >= 2)) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // Remove the encoder from the list entirely if it fails validation
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // Skip it if it doesn't support the specified codec at all
        if ((active_hevc_mode >= 2 && !encoder->hevc[encoder_t::PASSED]) || (active_av1_mode >= 2 && !encoder->av1[encoder_t::PASSED])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 5 && !encoder->hevc[encoder_t::DYNAMIC_RANGE] && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) || (active_av1_mode == 5 && !encoder->av1[encoder_t::DYNAMIC_RANGE] && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 4 && !encoder->hevc[encoder_t::DYNAMIC_RANGE_YUV444]) || (active_av1_mode == 4 && !encoder->av1[encoder_t::DYNAMIC_RANGE_YUV444])) {
          pos++;
          continue;
        }

        // Skip it if it doesn't support HDR on the specified codec
        if ((active_hevc_mode == 3 && !encoder->hevc[encoder_t::DYNAMIC_RANGE]) || (active_av1_mode == 3 && !encoder->av1[encoder_t::DYNAMIC_RANGE])) {
          pos++;
          continue;
        }

        chosen_encoder = encoder;
        break;
      });

      if (chosen_encoder == nullptr) {
        BOOST_LOG(error) << "Couldn't find any working encoder that meets HEVC/AV1 requirements"sv;
      }
    }

    // If no encoder was specified or the specified encoder was unusable, keep trying
    // the remaining encoders until we find one that passes validation.
    if (chosen_encoder == nullptr) {
      KITTY_WHILE_LOOP(auto pos = std::begin(encoder_list), pos != std::end(encoder_list), {
        auto encoder = *pos;

        // If we've used a previous encoder and it's not this one, we expect this encoder to
        // fail to validate. It will use a slightly different order of checks to more quickly
        // eliminate failing encoders.
        if (!validate_encoder(*encoder, previous_encoder && previous_encoder != encoder)) {
          pos = encoder_list.erase(pos);
          continue;
        }

        // We will return an encoder here even if it fails one of the codec requirements specified by the user
        adjust_encoder_constraints_hevc(encoder);
        adjust_encoder_constraints_av1(encoder);

        chosen_encoder = encoder;
        break;
      });
    }

    if (chosen_encoder == nullptr) {
      const auto output_name {display_device::map_output_name(config::video.output_name)};
      BOOST_LOG(fatal) << "Unable to find display or encoder during startup."sv;
      if (!config::video.adapter_name.empty() || !output_name.empty()) {
        BOOST_LOG(fatal) << "Please ensure your manually chosen GPU and monitor are connected and powered on."sv;
      } else {
        BOOST_LOG(fatal) << "Please check that a display is connected and powered on."sv;
      }
      return -1;
    }

    BOOST_LOG(info);
    BOOST_LOG(info) << "// Ignore any errors mentioned above, they are not relevant. //"sv;
    BOOST_LOG(info);

    auto &encoder = *chosen_encoder;

    last_encoder_probe_supported_ref_frames_invalidation = (encoder.flags & REF_FRAMES_INVALIDATION);
    last_encoder_probe_supported_yuv444_for_codec[0] = encoder.h264[encoder_t::PASSED] &&
                                                       encoder.h264[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[1] = encoder.hevc[encoder_t::PASSED] &&
                                                       encoder.hevc[encoder_t::YUV444];
    last_encoder_probe_supported_yuv444_for_codec[2] = encoder.av1[encoder_t::PASSED] &&
                                                       encoder.av1[encoder_t::YUV444];

    BOOST_LOG(debug) << "------  h264 ------"sv;
    for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
      auto flag = static_cast<encoder_t::flag_e>(x);
      BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.h264[flag] ? ": supported"sv : ": unsupported"sv);
    }
    BOOST_LOG(debug) << "-------------------"sv;
    BOOST_LOG(info) << "Found H.264 encoder: "sv << encoder.h264.name << " ["sv << encoder.name << ']';

    if (encoder.hevc[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  hevc ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = static_cast<encoder_t::flag_e>(x);
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.hevc[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found HEVC encoder: "sv << encoder.hevc.name << " ["sv << encoder.name << ']';
    }

    if (encoder.av1[encoder_t::PASSED]) {
      BOOST_LOG(debug) << "------  av1 ------"sv;
      for (int x = 0; x < encoder_t::MAX_FLAGS; ++x) {
        auto flag = static_cast<encoder_t::flag_e>(x);
        BOOST_LOG(debug) << encoder_t::from_flag(flag) << (encoder.av1[flag] ? ": supported"sv : ": unsupported"sv);
      }
      BOOST_LOG(debug) << "-------------------"sv;

      BOOST_LOG(info) << "Found AV1 encoder: "sv << encoder.av1.name << " ["sv << encoder.name << ']';
    }

    // 2 - passed
    // 3 - HDR yuv420
    // 4 - HDR yuv444
    // 5 - HDR yuv420 & HDR yuv444

    if (active_hevc_mode == 0) {
      active_hevc_mode = 1;
      if (encoder.hevc[encoder_t::PASSED]) {
        active_hevc_mode = 2;
        if (encoder.hevc[encoder_t::DYNAMIC_RANGE]) {
          active_hevc_mode += 1;
        }
        if (encoder.hevc[encoder_t::DYNAMIC_RANGE_YUV444]) {
          active_hevc_mode += 2;
        }
      }
      BOOST_LOG(debug) << "ENCODER STATUS ACTIVE_HEVC_MODE: "sv << active_hevc_mode;
    }

    if (active_av1_mode == 0) {
      active_av1_mode = 1;
      if (encoder.av1[encoder_t::PASSED]) {
        active_av1_mode = 2;
        if (encoder.av1[encoder_t::DYNAMIC_RANGE]) {
          active_av1_mode += 1;
        }
        if (encoder.av1[encoder_t::DYNAMIC_RANGE_YUV444]) {
          active_av1_mode += 2;
        }
      }
      BOOST_LOG(debug) << "ENCODER STATUS ACTIVE_AV1_MODE: "sv << active_av1_mode;
    }

    return 0;
  }

  // Linux only declaration
  /**
   * @brief Callback signature for VA-API AVCodec hardware input initialization.
   */
  typedef int (*vaapi_init_avcodec_hardware_input_buffer_fn)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  /**
   * @brief Initialize AVCodec hardware input buffers for VA-API.
   */
  util::Either<avcodec_buffer_t, int> vaapi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    // If an egl hwdevice
    if (encode_device->data) {
      if (((vaapi_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }

      return hw_device_buf;
    }

    auto render_device = platf::resolve_render_device();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VAAPI, render_device.empty() ? nullptr : render_device.c_str(), nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VAAPI device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef SUNSHINE_BUILD_VULKAN
  using vulkan_init_avcodec_hardware_input_buffer_fn = int (*)(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  util::Either<avcodec_buffer_t, int> vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    if (encode_device && encode_device->data) {
      if (((vulkan_init_avcodec_hardware_input_buffer_fn) encode_device->data)(encode_device, &hw_device_buf)) {
        return -1;
      }
      return hw_device_buf;
    }

    // Try render device path first, auto-detecting the GPU with a connected display
    auto render_device = platf::resolve_render_device();

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, render_device.c_str(), nullptr, 0);
    if (status >= 0) {
      BOOST_LOG(info) << "Using Vulkan device: "sv << render_device;
      return hw_device_buf;
    }

    // Fallback: try device indices for multi-GPU systems
    const std::array<const char *, 4> devices = {"1", "0", "2", "3"};
    for (auto device : devices) {
      status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VULKAN, device, nullptr, 0);
      if (status >= 0) {
        BOOST_LOG(info) << "Using Vulkan device index: "sv << device;
        return hw_device_buf;
      }
    }

    BOOST_LOG(error) << "Failed to create a Vulkan device"sv;
    return -1;
  }
#endif

  /**
   * @brief Initialize AVCodec hardware input buffers for CUDA.
   */
  util::Either<avcodec_buffer_t, int> cuda_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1 /* AV_CUDA_USE_PRIMARY_CONTEXT */);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a CUDA device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

  /**
   * @brief Initialize AVCodec hardware input buffers for VideoToolbox.
   */
  util::Either<avcodec_buffer_t, int> vt_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t hw_device_buf;

    auto status = av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (status < 0) {
      char string[AV_ERROR_MAX_STRING_SIZE];
      BOOST_LOG(error) << "Failed to create a VideoToolbox device: "sv << av_make_error_string(string, AV_ERROR_MAX_STRING_SIZE, status);
      return -1;
    }

    return hw_device_buf;
  }

#ifdef _WIN32
}

/**
 * @brief No-op lock callback used when FFmpeg requires a D3D11VA lock function.
 */
void do_nothing(void *) {
}

namespace video {
  /**
   * @brief Create an FFmpeg D3D11VA hardware device from Sunshine's DXGI device.
   */
  util::Either<avcodec_buffer_t, int> dxgi_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device) {
    avcodec_buffer_t ctx_buf {av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA)};
    auto ctx = (AVD3D11VADeviceContext *) ((AVHWDeviceContext *) ctx_buf->data)->hwctx;

    std::fill_n((std::uint8_t *) ctx, sizeof(AVD3D11VADeviceContext), 0);

    auto device = static_cast<ID3D11Device *>(encode_device->data);

    device->AddRef();
    ctx->device = device;

    ctx->lock_ctx = (void *) 1;
    ctx->lock = do_nothing;
    ctx->unlock = do_nothing;

    auto err = av_hwdevice_ctx_init(ctx_buf.get());
    if (err) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Failed to create FFMpeg hardware device context: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return err;
    }

    return ctx_buf;
  }
#endif

  /**
   * @brief Start capture async.
   */
  int start_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.encoder_p = chosen_encoder;
    capture_thread_ctx.reinit_event.reset();

    capture_thread_ctx.capture_ctx_queue = std::make_shared<safe::queue_t<capture_ctx_t>>(30);

    capture_thread_ctx.capture_thread = std::jthread {
      captureThread,
      capture_thread_ctx.capture_ctx_queue,
      std::ref(capture_thread_ctx.display_wp),
      std::ref(capture_thread_ctx.reinit_event),
      std::ref(*capture_thread_ctx.encoder_p)
    };

    return 0;
  }

  /**
   * @brief Stop capture async processing.
   */
  void end_capture_async(capture_thread_async_ctx_t &capture_thread_ctx) {
    capture_thread_ctx.capture_ctx_queue->stop();

    capture_thread_ctx.capture_thread.join();
  }

  /**
   * @brief Start capture sync.
   */
  int start_capture_sync(capture_thread_sync_ctx_t &ctx) {
    std::jthread {&captureThreadSync}.detach();
    return 0;
  }

  /**
   * @brief Stop capture sync processing.
   */
  void end_capture_sync(capture_thread_sync_ctx_t &ctx) {
  }

  /**
   * @brief Map base dev type values.
   */
  platf::mem_type_e map_base_dev_type(AVHWDeviceType type) {
    switch (type) {
      case AV_HWDEVICE_TYPE_D3D11VA:
        return platf::mem_type_e::dxgi;
      case AV_HWDEVICE_TYPE_VAAPI:
        return platf::mem_type_e::vaapi;
#ifdef SUNSHINE_BUILD_VULKAN
      case AV_HWDEVICE_TYPE_VULKAN:
        return platf::mem_type_e::vulkan;
#endif
      case AV_HWDEVICE_TYPE_CUDA:
        return platf::mem_type_e::cuda;
      case AV_HWDEVICE_TYPE_NONE:
        return platf::mem_type_e::system;
      case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
        return platf::mem_type_e::videotoolbox;
      default:
        return platf::mem_type_e::unknown;
    }

    return platf::mem_type_e::unknown;
  }

  /**
   * @brief Map pix fmt values.
   */
  platf::pix_fmt_e map_pix_fmt(AVPixelFormat fmt) {
    switch (fmt) {
      case AV_PIX_FMT_VUYX:
        return platf::pix_fmt_e::ayuv;
      case AV_PIX_FMT_XV30:
        return platf::pix_fmt_e::y410;
      case AV_PIX_FMT_YUV420P10:
        return platf::pix_fmt_e::yuv420p10;
      case AV_PIX_FMT_YUV420P:
        return platf::pix_fmt_e::yuv420p;
      case AV_PIX_FMT_NV12:
        return platf::pix_fmt_e::nv12;
      case AV_PIX_FMT_P010:
        return platf::pix_fmt_e::p010;
      case AV_PIX_FMT_YUV444P:
        return platf::pix_fmt_e::yuv444p;
      case AV_PIX_FMT_YUV444P16:
        return platf::pix_fmt_e::yuv444p16;
      default:
        return platf::pix_fmt_e::unknown;
    }

    return platf::pix_fmt_e::unknown;
  }

}  // namespace video
