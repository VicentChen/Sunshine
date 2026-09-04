/**
 * @file src/platform/linux/ui_controller.h
 * @brief Thread-safe controller routing policy for the RKMPP Vulkan UI.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

// local includes
#include "../common.h"

namespace platf::ui {
  /** @brief One controller navigation action emitted on a digital edge. */
  enum class navigation_e {
    none,  ///< No navigation edge was emitted.
    up,  ///< Move focus upward.
    down,  ///< Move focus downward.
    left,  ///< Move focus left.
    right,  ///< Move focus right.
    confirm,  ///< Activate the focused item.
    back  ///< Return to the previous page.
  };

  /** @brief Modal pages rendered by the RKMPP Vulkan UI. */
  enum class page_e {
    main_menu,  ///< Main menu containing the four first-version entries.
    connection_status,  ///< Read-only connection status page.
    profile,  ///< Read-only frame profile page.
    ui_size  ///< User-selectable UI size page.
  };

  /** @brief User-selectable UI scale while preserving output-resolution adaptation. */
  enum class ui_size_e : std::uint8_t {
    compact,  ///< 85% of the resolution-adaptive baseline.
    standard,  ///< 100% of the resolution-adaptive baseline.
    large  ///< 120% of the resolution-adaptive baseline.
  };

  inline constexpr std::uint16_t profile_scroll_step_limit = 32;  ///< Maximum controller-selected Profile scroll step.

  /** @brief Explicit action requested by one modal UI update. */
  enum class action_e {
    none,  ///< No action was requested.
    close_modal  ///< Close the modal UI and restore normal input routing.
  };

  /** @brief Sanitized video and gamepad readiness published to the UI. */
  struct connection_status_t {
    std::string video_state {"starting"};  ///< Fixed HDMI RX state-machine name.
    std::string gamepad_state {"idle"};  ///< Fixed selected-output lifecycle state.
    std::string gamepad_stage;  ///< Fixed selected-output lifecycle stage.
    std::string failure_kind;  ///< Fixed selected-output failure policy.
    std::uint32_t moonlight_width {};  ///< Requested Moonlight video width.
    std::uint32_t moonlight_height {};  ///< Requested Moonlight video height.
    std::uint32_t moonlight_fps_x100 {};  ///< Requested Moonlight video rate in hundredths of a frame per second.
    std::uint32_t input_width {};  ///< Current HDMI input width, or zero when unknown.
    std::uint32_t input_height {};  ///< Current HDMI input height, or zero when unknown.
    bool video_ready {};  ///< HDMI RX is streaming direct or through RGA.
    bool gamepad_ready {};  ///< Selected gamepad output accepts controller input.

    /** @brief Compare complete sanitized connection snapshots. */
    bool operator==(const connection_status_t &) const = default;

    /** @brief Return whether both connection components are ready. */
    bool complete() const noexcept;
  };

  /** @brief Stable subset and ordering of completed-window metrics shown by the UI. */
  enum class profile_metric_e : std::uint8_t {
    rx_driver_age,  ///< HDMI RX timestamp to dequeue.
    capture_queue,  ///< Dequeue to encoder-thread processing.
    rga,  ///< RGA conversion or placeholder fill.
    mpp_encode,  ///< MPP submit to complete encoded packet.
    encoded_queue,  ///< Encoded packet to network-thread processing.
    packetize_send,  ///< Network-thread processing to final send.
    protocol_host,  ///< HDMI RX timestamp to packetization.
    host_send,  ///< HDMI RX timestamp to final send.
    count  ///< Number of displayed profile metrics.
  };

  /** @brief Sanitized percentiles and sample health for one profile metric. */
  struct profile_metric_status_t {
    std::uint32_t count {};  ///< Valid samples in the completed window.
    std::uint32_t missing {};  ///< Frames missing the required timestamps.
    std::uint32_t invalid {};  ///< Frames whose end timestamp preceded the start.
    std::int64_t p50_us {};  ///< Nearest-rank 50th percentile in microseconds.
    std::int64_t p95_us {};  ///< Nearest-rank 95th percentile in microseconds.
    std::int64_t p99_us {};  ///< Nearest-rank 99th percentile in microseconds.

    /** @brief Compare complete metric snapshots. */
    bool operator==(const profile_metric_status_t &) const = default;
  };

  /** @brief Renderer-independent view of the latest completed profile window. */
  struct profile_status_t {
    static constexpr auto metric_count = static_cast<std::size_t>(profile_metric_e::count);  ///< Displayed metric count.
    std::array<profile_metric_status_t, metric_count> metrics;  ///< Metrics in profile_metric_e order.
    std::uint32_t captured_frames {};  ///< Real HDMI RX frames in the window.
    std::uint32_t placeholder_frames {};  ///< Synthetic placeholder frames in the window.
    std::uint32_t repeated_frames {};  ///< Repeated frames in the window.
    std::uint32_t rga_bypass_frames {};  ///< Real frames that bypassed RGA.
    std::uint64_t freshness_drops {};  ///< Older HDMI frames discarded for freshness.
    std::uint32_t dropped_samples {};  ///< Samples lost after bounded buffers filled.
    std::uint32_t hdmirx_width {};  ///< Latest HDMI RX width in the window.
    std::uint32_t hdmirx_height {};  ///< Latest HDMI RX height in the window.
    std::uint32_t moonlight_width {};  ///< Latest Moonlight target width in the window.
    std::uint32_t moonlight_height {};  ///< Latest Moonlight target height in the window.
    video::frame_profile_timeline_snapshot_t timeline;  ///< Recent completed frames for the real-time Timeline view.
    bool available {};  ///< Whether a completed window has been published.

    /** @brief Compare complete profile snapshots. */
    bool operator==(const profile_status_t &) const = default;
  };

  /** @brief Complete controller state needed by the UI input policy. */
  struct controller_input_t {
    std::uint32_t buttons {};  ///< Moonlight gamepad button mask.
    std::int16_t left_stick_x {};  ///< Left stick horizontal position.
    std::int16_t left_stick_y {};  ///< Left stick vertical position; positive is up.
  };

  /** @brief Input-routing result for one controller packet. */
  struct decision_t {
    bool consume {};  ///< Prevent this packet from reaching the configured gamepad output.
    bool neutralize {};  ///< Send one neutral state to the configured gamepad output.
    bool replay_start_tap {};  ///< Recreate a deferred standalone Start tap on the configured output.
    bool visibility_changed {};  ///< The packet opened or closed the UI.
    bool visible {};  ///< UI visibility after processing the packet.
    navigation_e navigation {navigation_e::none};  ///< Navigation edge emitted by the owner.
    action_e action {action_e::none};  ///< Explicit action emitted by the focused item.
  };

  /** @brief Immutable renderer-facing state published by the controller policy. */
  struct snapshot_t {
    bool visible {};  ///< Whether either automatic or modal UI should be composed.
    bool modal {};  ///< Whether a controller-owned modal page is open.
    page_e page {page_e::main_menu};  ///< Page currently presented by the modal UI.
    std::uint8_t focus {};  ///< Focused item in the four-entry main menu.
    connection_status_t connection;  ///< Sanitized readiness data for the connection page.
    profile_status_t profile;  ///< Latest completed-window metrics for the Profile page.
    ui_size_e ui_size {ui_size_e::standard};  ///< Current user-selected size tier.
    std::uint8_t ui_size_focus {static_cast<std::uint8_t>(ui_size_e::standard)};  ///< Focused size tier on the settings page.
    std::uint16_t profile_scroll_steps {};  ///< Controller-selected vertical offset for the scrollable Profile viewport.
    std::uint64_t revision {1};  ///< Monotonic render revision.
  };

  /** @brief Detect the UI chord, own modal input, and publish navigation state. */
  class controller_t {
  public:
    using clock_t = std::chrono::steady_clock;  ///< Monotonic clock used by the hold detector.

    /** @brief Create an independently usable controller, or a detached process-wide controller. */
    explicit controller_t(bool backend_available = true) noexcept;

    /** @brief Register one live renderer backend that can make controller UI visible. */
    void attach_backend();

    /**
     * @brief Unregister one renderer backend and clear modal routing when the last one leaves.
     *
     * A runtime Vulkan/RGA failure must detach before its encoder continues without UI so
     * an invisible modal cannot keep consuming Moonlight controller packets.
     */
    void detach_backend();

    /** @brief Return whether at least one renderer backend can currently present the UI. */
    bool backend_available() const;

    /**
     * @brief Process one complete Moonlight controller state.
     *
     * @param controller Process-wide Sunshine gamepad slot.
     * @param input Complete buttons and left-stick state.
     * @param now Monotonic event time.
     * @return Routing, cleanup, visibility, and navigation decisions.
     */
    decision_t update(std::uint8_t controller, const controller_input_t &input, clock_t::time_point now);

    /** @brief Return current visibility for the video-frame polling path. */
    decision_t tick(clock_t::time_point now);

    /**
     * @brief Remove one controller and close the UI when it owns the panel.
     *
     * @param controller Process-wide Sunshine gamepad slot being removed.
     * @return Cleanup and visibility decision for the removed controller.
     */
    decision_t disconnect(std::uint8_t controller);

    /** @brief Clear all controller state and hide the UI. */
    void reset();

    /** @brief Return one consistent renderer-facing state snapshot. */
    snapshot_t snapshot() const;

    /**
     * @brief Publish changed video and gamepad readiness for automatic display.
     *
     * @param status Complete sanitized connection snapshot.
     */
    void update_connection(connection_status_t status, clock_t::time_point now = clock_t::now());

    /**
     * @brief Publish a changed completed-window snapshot for the Profile page.
     *
     * Profile changes only invalidate the renderer while the modal Profile
     * page is visible; entering that page already increments the revision.
     *
     * @param status Complete renderer-independent profile snapshot.
     */
    void update_profile(profile_status_t status);

    /** @brief Return whether the modal UI is currently visible. */
    bool visible() const;

    /** @brief Return the process-wide gamepad slot that owns the UI. */
    std::optional<std::uint8_t> owner() const;

  private:
    /** @brief Per-controller edge, axis, and chord state. */
    struct input_state_t {
      bool start_deferred {};  ///< Start is reserved as the ordered UI modifier until released or cancelled.
      bool start_passthrough {};  ///< Start joined another non-UI input and passes through until release.
      std::uint32_t previous_navigation {};  ///< Previous digitalized navigation mask.
      std::int8_t axis_x {};  ///< Hysteretic horizontal axis direction.
      std::int8_t axis_y {};  ///< Hysteretic vertical axis direction.
    };

    static constexpr std::uint32_t start_back_chord_ = platf::START | platf::BACK;  ///< Ordered visibility chord.
    static constexpr std::chrono::seconds ready_hold_time_ {3};  ///< Stable full-link interval before automatic UI dismissal.
    static constexpr std::int16_t axis_press_ = 16000;  ///< Analog navigation press threshold.
    static constexpr std::int16_t axis_release_ = 8000;  ///< Analog navigation release threshold.
    static constexpr std::uint8_t item_count_ = 4;  ///< Items in the first-version main menu.
    static constexpr std::uint8_t ui_size_item_count_ = 3;  ///< Available UI size tiers.

    /** @brief Convert one analog axis into a stable negative, neutral, or positive direction. */
    static std::int8_t axis_direction(std::int16_t value, std::int8_t previous) noexcept;

    /** @brief Convert buttons and left stick into a digital navigation mask. */
    static std::uint32_t navigation_mask(const controller_input_t &input, input_state_t &state) noexcept;

    mutable std::mutex mutex_;  ///< Serializes input updates and renderer snapshots.
    std::uint32_t backend_count_ {};  ///< Live renderers able to present the process-wide UI.
    std::array<input_state_t, platf::MAX_GAMEPADS> inputs_;  ///< State indexed by process-wide gamepad slot.
    std::optional<std::uint8_t> owner_;  ///< Controller that opened and may navigate the UI.
    std::optional<std::uint8_t> release_controller_;  ///< Controller held behind the full-release gate.
    std::uint32_t release_chord_ {};  ///< Triggering chord that must be fully released.
    bool visible_ {};  ///< Current modal visibility.
    page_e page_ {page_e::main_menu};  ///< Current modal page.
    std::uint8_t focus_ {};  ///< Focused item in the first-version main menu.
    ui_size_e ui_size_ {ui_size_e::standard};  ///< User-selected resolution-relative size tier.
    std::uint8_t ui_size_focus_ {static_cast<std::uint8_t>(ui_size_e::standard)};  ///< Focused UI size tier.
    std::uint16_t profile_scroll_steps_ {};  ///< Current controller-selected Profile scroll step.
    connection_status_t connection_;  ///< Latest sanitized connection state.
    profile_status_t profile_;  ///< Latest completed profile window.
    bool connection_initialized_ {};  ///< Whether a video frame has published connection state.
    std::optional<clock_t::time_point> connection_ready_since_;  ///< Start of uninterrupted video and gamepad readiness.
    bool connection_settled_ {};  ///< Whether full readiness has remained stable for ready_hold_time_.
    std::uint64_t revision_ {1};  ///< Monotonic renderer revision.
  };

  /** @brief Return the process-wide UI controller shared by input and video. */
  controller_t &global_controller();
}  // namespace platf::ui
