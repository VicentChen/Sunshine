/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

#ifdef SUNSHINE_TESTS
  #include "xbox_remote/worker.h"
#endif

namespace input {
  struct input_t;

  /**
   * @brief Write a debug log representation of the input packet.
   *
   * @param input Raw input packet to format for logging.
   */
  void print(void *input);
  /**
   * @brief Reset stream input state after a client disconnect or shutdown.
   *
   * @param input Shared stream input state to reset.
   */
  void reset(std::shared_ptr<input_t> &input);

  /**
   * @brief Destroy every retained virtual gamepad session.
   *
   * Retained gamepads survive a paused transport connection so they can be reused on resume. Call this when the
   * streamed application or all streaming sessions are explicitly terminated.
   */
  void terminate_gamepads();

  /**
   * @brief Destroy virtual gamepads retained for one paired client.
   *
   * @param session_id Stable paired-client identity used by alloc().
   */
  void terminate_gamepads(std::string_view session_id);

  /**
   * @brief Queue a raw input message for platform passthrough.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  /**
   * @brief Initialize global input resources and platform backends.
   *
   * @return Cleanup handle for initialized input resources, or null if none are required.
   */
  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Probe whether the configured controller outputs are unavailable.
   *
   * @return True when no configured gamepad output backend is available.
   */
  bool probe_gamepads();

  /**
   * @brief Select the gamepad output route for a launched Sunshine application.
   *
   * Nintendo Switch uses the configured controller output. An explicitly configured
   * Xbox application uses the application-scoped Remote Play worker when enabled.
   * Other applications accept controller input without forwarding it.
   *
   * @param app_name Configured Sunshine application name, or empty when no app is active.
   */
  void select_gamepad_output(std::string_view app_name);

  /**
   * @brief Retain the Xbox worker for the configured grace period after the final stream ends.
   *
   * The controller is neutralized by stream teardown while the Remote Play
   * connection remains available for a fast Moonlight resume. Expiration stops
   * the worker unless a new stream cancels the pending timeout first.
   */
  void suspend_xbox_remote_for_disconnected_stream();

  /**
   * @brief Reuse or recreate the Xbox worker before a Moonlight stream starts.
   *
   * This operation is a no-op for applications that are not routed to Xbox
   * Remote Play. A compatible starting or ready worker cancels its idle timeout
   * and remains attached; failed, stopped, or missing workers are replaced.
   *
   * @param app_name Currently running Sunshine application name.
   */
  void resume_xbox_remote_for_stream(std::string_view app_name);

  /**
   * @brief Sanitized Xbox Remote Play lifecycle snapshot.
   */
  struct xbox_remote_status_t {
    std::string state;  ///< Fixed lifecycle state name.
    std::string stage;  ///< Fixed current connection stage.
    std::string failure_stage;  ///< Fixed failure stage, empty outside failed state.
    std::string failure_kind;  ///< Fixed retry, reauthentication, or permanent policy.
    std::uint64_t epoch = 0;  ///< Monotonic connection generation without remote identifiers.
    bool selected = false;  ///< Whether the active application routes gamepads through Xbox Remote Play.
  };

  /**
   * @brief Return the current application-scoped Xbox Remote Play status.
   *
   * @return Sanitized state and failure stage without credentials or console identifiers.
   */
  xbox_remote_status_t xbox_remote_status();

  /**
   * @brief Recreate the shared libvirtualhid mouse after a license-state change.
   *
   * The work is serialized with streamed input so the mouse backend can switch
   * safely between the Windows HID and SendInput paths.
   */
  void refresh_virtual_mouse();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @param session_id Stable paired-client identity shared by launch and resume connections.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, std::string session_id);

#ifdef SUNSHINE_TESTS
  namespace testing {
    /**
     * @brief Override production Xbox connection creation for lifecycle tests.
     *
     * Passing an empty factory restores production connection creation.
     *
     * @param factory Test connection factory or an empty factory.
     */
    void set_xbox_remote_connection_factory(::xbox_remote::worker::connection_factory_t factory);

    /**
     * @brief Replace the global platform input backend for a unit test.
     *
     * @param input Test-owned platform input backend.
     */
    void set_platform_input(platf::input_t input);

    /**
     * @brief Recreate the gamepad router after a test changes output configuration.
     */
    void reconfigure_gamepad_router();

    /**
     * @brief Allocate a gamepad directly in retained input state for a unit test.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @param metadata Client-reported controller metadata.
     * @return Assigned global gamepad slot, or -1 on failure.
     */
    int alloc_gamepad(std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_arrival_t &metadata);

    /**
     * @brief Return the global gamepad slot stored for a test controller.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @return Assigned global gamepad slot, or -1 when unallocated.
     */
    int gamepad_id(const std::shared_ptr<input_t> &input, std::uint8_t client_index);

    /**
     * @brief Submit a complete gamepad state through a retained test input.
     *
     * @param input Retained input state.
     * @param client_index Client-relative controller index.
     * @param state Complete controller state.
     * @return True when an allocated router accepted the state.
     */
    bool update_gamepad(const std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_state_t &state);

    /**
     * @brief Submit a complete gamepad state through production passthrough routing.
     *
     * @param input Retained test input.
     * @param client_index Client-relative controller index.
     * @param state Complete controller state.
     */
    void passthrough_gamepad(const std::shared_ptr<input_t> &input, std::uint8_t client_index, const platf::gamepad_state_t &state);

    /**
     * @brief Synchronously neutralize retained gamepads for a unit test.
     *
     * @param input Retained input state whose gamepads should be neutralized.
     */
    void neutralize_gamepads(const std::shared_ptr<input_t> &input);
  }  // namespace testing
#endif

  /**
   * @brief Touchscreen coordinate bounds used to scale absolute input.
   */
  struct touch_port_t: public platf::touch_port_t {
    int env_width;  ///< Width of the full capture environment in physical pixels.
    int env_height;  ///< Height of the full capture environment in physical pixels.

    // Offset x and y coordinates of the client
    float client_offsetX;  ///< Horizontal client viewport offset used when scaling touch input.
    float client_offsetY;  ///< Vertical client viewport offset used when scaling touch input.

    float scalar_inv;  ///< Inverse scale factor from client coordinates to display coordinates.
    float scalar_tpcoords;  ///< Scale factor from client coordinates to touch-port coordinates.

    int env_logical_width;  ///< Width of the full capture environment after display scaling.
    int env_logical_height;  ///< Height of the full capture environment after display scaling.

    /**
     * @brief Check whether the touch-port bounds are initialized.
     */
    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);
}  // namespace input
