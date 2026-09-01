/**
 * @file src/platform/linux/vulkan_ui.cpp
 * @brief Vulkan UI rendering into an external RGBA DMA-BUF for RKMPP.
 */

// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// system includes
#include <unistd.h>
#include <vulkan/vulkan.h>

// Dear ImGui is intentionally consumed from the pinned source submodule.
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

// local includes
#include "vulkan_ui.h"

namespace platf::vulkan_ui {
  namespace {
    constexpr VkFormat panel_format = VK_FORMAT_B8G8R8_UNORM;

    /** @brief Close one duplicated descriptor during Vulkan import failure. */
    class fd_t {
    public:
      explicit fd_t(int value = -1) noexcept:
          value_(value) {}

      ~fd_t() {
        if (value_ >= 0) {
          (void) ::close(value_);
        }
      }

      fd_t(const fd_t &) = delete;
      fd_t &operator=(const fd_t &) = delete;

      int get() const noexcept {
        return value_;
      }

      int release() noexcept {
        return std::exchange(value_, -1);
      }

    private:
      int value_;
    };

    /** @brief Convert a Vulkan error into a stable C++ exception. */
    void require_vk(VkResult result, std::string_view operation) {
      if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result));
      }
    }

    /** @brief Test for one device extension by exact name. */
    bool has_extension(const std::vector<VkExtensionProperties> &extensions, std::string_view name) {
      return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties &extension) {
        return name == extension.extensionName;
      });
    }

    /** @brief Enumerate extensions supported by one physical device. */
    std::vector<VkExtensionProperties> device_extensions(VkPhysicalDevice device) {
      std::uint32_t count = 0;
      require_vk(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties(count)");
      std::vector<VkExtensionProperties> extensions(count);
      require_vk(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()), "vkEnumerateDeviceExtensionProperties(list)");
      extensions.resize(count);
      return extensions;
    }

    /** @brief Selected non-CPU device and its graphics/transfer queue family. */
    struct selected_device_t {
      VkPhysicalDevice device {VK_NULL_HANDLE};
      VkPhysicalDeviceProperties properties {};
      std::uint32_t queue_family {};
    };

    /** @brief Select the first hardware device satisfying the UI import contract. */
    selected_device_t select_device(VkInstance instance) {
      std::uint32_t count = 0;
      require_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
      std::vector<VkPhysicalDevice> devices(count);
      require_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices(list)");
      devices.resize(count);
      for (const auto device : devices) {
        VkPhysicalDeviceProperties properties {};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
          continue;
        }
        const auto extensions = device_extensions(device);
        if (!has_extension(extensions, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) || !has_extension(extensions, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
          continue;
        }
        VkFormatProperties format_properties {};
        vkGetPhysicalDeviceFormatProperties(device, panel_format, &format_properties);
        constexpr auto required_format_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((format_properties.optimalTilingFeatures & required_format_features) != required_format_features) {
          continue;
        }
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
        for (std::uint32_t family = 0; family < family_count; ++family) {
          if (families[family].queueCount != 0 && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && (families[family].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
            return {device, properties, family};
          }
        }
      }
      throw std::runtime_error("no hardware Vulkan device satisfies the external DMA-BUF UI contract");
    }

    /** @brief Choose a compatible Vulkan memory type, preferring requested properties. */
    std::uint32_t choose_memory_type(VkPhysicalDevice device, std::uint32_t mask, VkMemoryPropertyFlags preferred = 0) {
      VkPhysicalDeviceMemoryProperties properties {};
      vkGetPhysicalDeviceMemoryProperties(device, &properties);
      for (std::uint32_t pass = 0; pass < 2; ++pass) {
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
          const bool allowed = (mask & (1U << index)) != 0;
          const bool preferred_match = (properties.memoryTypes[index].propertyFlags & preferred) == preferred;
          if (allowed && (pass != 0 || preferred_match)) {
            return index;
          }
        }
      }
      throw std::runtime_error("no compatible Vulkan memory type was found");
    }

    /** @brief Convert a model color into a Vulkan clear color. */
    VkClearColorValue clear_color(const color_t &color) noexcept {
      VkClearColorValue value {};
      value.float32[0] = color.red;
      value.float32[1] = color.green;
      value.float32[2] = color.blue;
      value.float32[3] = color.alpha;
      return value;
    }

    /** @brief Validate one finite normalized color. */
    bool valid_color(const color_t &color) noexcept {
      const std::array components {color.red, color.green, color.blue, color.alpha};
      return std::all_of(components.begin(), components.end(), [](float component) {
               return std::isfinite(component) && component >= 0.0F && component <= 1.0F;
             }) &&
             color.alpha == 1.0F;
    }

    /**
     * @brief Draw one opaque card in the current fixed-layout page.
     *
     * @param id Stable Dear ImGui child identifier.
     * @param label Card heading.
     * @param description Card value or action hint.
     * @param focused Whether to draw the modal focus highlight.
     */
    void draw_menu_card(const char *id, const char *label, const char *description, bool focused, float height = 74.0F) {
      const auto card = focused ? ImVec4 {0.055F, 0.330F, 0.720F, 1.0F} : ImVec4 {0.075F, 0.095F, 0.130F, 1.0F};
      ImGui::PushStyleColor(ImGuiCol_ChildBg, card);
      ImGui::BeginChild(id, ImVec2 {0.0F, height}, ImGuiChildFlags_Borders);
      ImGui::TextUnformatted(label);
      ImGui::Spacing();
      ImGui::TextUnformatted(description);
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }

    /**
     * @brief Format one known or unavailable video resolution.
     *
     * @param width Resolution width, or zero when unavailable.
     * @param height Resolution height, or zero when unavailable.
     * @return Fixed display text without external identifiers.
     */
    std::string resolution_text(std::uint32_t width, std::uint32_t height) {
      return width != 0 && height != 0 ? std::to_string(width) + "x" + std::to_string(height) : "UNKNOWN";
    }

    /**
     * @brief Select the most actionable sanitized gamepad status text.
     *
     * @param status Sanitized selected-output lifecycle snapshot.
     * @return Reference to failure, stage, or state text owned by @p status.
     */
    const std::string &gamepad_status_text(const platf::ui::connection_status_t &status) noexcept {
      if (!status.failure_kind.empty()) {
        return status.failure_kind;
      }
      return status.gamepad_stage.empty() ? status.gamepad_state : status.gamepad_stage;
    }

    /** @brief Format one completed-window metric for a compact two-line card. */
    std::string profile_metric_text(const platf::ui::profile_metric_status_t &metric) {
      char text[96] {};
      if (metric.count == 0) {
        std::snprintf(text, sizeof(text), "N 0  M %u  I %u\nNO SAMPLES", metric.missing, metric.invalid);
      } else {
        std::snprintf(
          text,
          sizeof(text),
          "N %u  M %u  I %u\n50 %.1f  95 %.1f  99 %.1f MS",
          metric.count,
          metric.missing,
          metric.invalid,
          metric.p50_us / 1000.0,
          metric.p95_us / 1000.0,
          metric.p99_us / 1000.0
        );
      }
      return text;
    }

    /** @brief Return a stable color for one concrete Timeline stage. */
    ImU32 timeline_stage_color(video::frame_profile_timeline_stage_e stage) noexcept {
      constexpr std::array colors {
        IM_COL32(80, 170, 255, 230),
        IM_COL32(65, 125, 220, 230),
        IM_COL32(75, 205, 145, 230),
        IM_COL32(195, 105, 255, 230),
        IM_COL32(150, 85, 235, 230),
        IM_COL32(255, 190, 75, 230),
        IM_COL32(245, 155, 55, 230),
        IM_COL32(230, 125, 45, 230),
        IM_COL32(255, 95, 75, 230),
        IM_COL32(220, 70, 70, 230),
        IM_COL32(70, 205, 205, 230),
        IM_COL32(70, 165, 175, 230)
      };
      return colors[static_cast<std::size_t>(stage)];
    }

    /** @brief Draw recent completed spans on stable resource lanes. */
    void draw_profile_timeline(const platf::ui::profile_status_t &profile) {
      const auto geometry = make_timeline_geometry(profile.timeline);
      const auto origin = ImGui::GetCursorScreenPos();
      const auto available = ImGui::GetContentRegionAvail();
      constexpr float label_width = 82.0F;
      constexpr float axis_height = 14.0F;
      constexpr std::array lane_names {"CAPTURE", "RGA", "VULKAN UI", "MPP", "NETWORK"};
      const auto chart_left = origin.x + label_width;
      const auto chart_width = std::max(1.0F, available.x - label_width);
      const auto chart_top = origin.y + axis_height;
      const auto lane_height = std::max(16.0F, (available.y - axis_height) / static_cast<float>(lane_names.size()));
      const auto chart_height = lane_height * lane_names.size();
      auto *draw = ImGui::GetWindowDrawList();

      for (std::size_t lane = 0; lane < lane_names.size(); ++lane) {
        const auto top = chart_top + lane_height * static_cast<float>(lane);
        draw->AddRectFilled(
          {chart_left, top},
          {chart_left + chart_width, top + lane_height - 1.0F},
          lane % 2U == 0U ? IM_COL32(21, 29, 43, 255) : IM_COL32(25, 35, 51, 255)
        );
        draw->AddText({origin.x, top + 2.0F}, IM_COL32(185, 202, 225, 255), lane_names[lane]);
      }
      for (int tick = 0; tick <= 4; ++tick) {
        const auto ratio = static_cast<float>(tick) / 4.0F;
        const auto x = chart_left + chart_width * ratio;
        draw->AddLine({x, chart_top}, {x, chart_top + chart_height}, IM_COL32(78, 92, 112, 120));
        const auto tick_us = geometry.view_start_us + static_cast<std::int64_t>((geometry.view_end_us - geometry.view_start_us) * ratio);
        char label[32] {};
        std::snprintf(label, sizeof(label), "%.0f ms", tick_us / 1000.0);
        draw->AddText({x + 2.0F, origin.y}, IM_COL32(145, 160, 180, 210), label);
      }
      const auto view_duration = std::max<std::int64_t>(1, geometry.view_end_us - geometry.view_start_us);
      for (std::size_t index = 0; index < profile.timeline.frame_count; ++index) {
        const auto &frame = profile.timeline.frames[index];
        if (frame.origin_offset_us < geometry.view_start_us || frame.origin_offset_us > geometry.view_end_us) {
          continue;
        }
        const auto ratio = static_cast<float>(frame.origin_offset_us - geometry.view_start_us) / static_cast<float>(view_duration);
        const auto x = chart_left + chart_width * ratio;
        draw->AddLine({x, chart_top}, {x, chart_top + chart_height}, IM_COL32(220, 230, 245, 85), 1.0F);
      }
      for (std::size_t index = 0; index < geometry.bar_count; ++index) {
        const auto &bar = geometry.bars[index];
        const auto lane = static_cast<std::size_t>(bar.lane);
        const auto top = chart_top + lane_height * static_cast<float>(lane);
        const auto half_height = std::max(5.0F, (lane_height - 5.0F) / 2.0F);
        const auto track = static_cast<float>(static_cast<std::uint64_t>(bar.frame_index) & 1U);
        const auto y0 = top + 2.0F + track * half_height;
        const auto y1 = std::min(top + lane_height - 2.0F, y0 + half_height - 1.0F);
        const auto x0 = chart_left + chart_width * bar.left;
        const auto x1 = std::max(x0 + 1.5F, chart_left + chart_width * bar.right);
        draw->AddRectFilled({x0, y0}, {x1, y1}, timeline_stage_color(bar.stage), 2.0F);
        if (bar.frame_index == geometry.latest_frame_index) {
          draw->AddRect({x0, y0}, {x1, y1}, IM_COL32(245, 250, 255, 245), 2.0F);
        }
        if (x1 - x0 >= 58.0F) {
          char label[64] {};
          std::snprintf(
            label,
            sizeof(label),
            "%.*s +%.1f-%.1f",
            static_cast<int>(video::frame_profile_timeline_stage_name(bar.stage).size()),
            video::frame_profile_timeline_stage_name(bar.stage).data(),
            bar.start_us / 1000.0,
            bar.end_us / 1000.0
          );
          draw->AddText({x0 + 3.0F, y0}, IM_COL32(250, 252, 255, 255), label);
        }
      }
      ImGui::Dummy({available.x, axis_height + chart_height});
    }

    /** @brief Build the current modal page entirely through Dear ImGui. */
    void build_modal_imgui_page(const render_model_t &model) {
      auto &io = ImGui::GetIO();
      io.DisplaySize = {static_cast<float>(model.width), static_cast<float>(model.height)};
      ImGui_ImplVulkan_NewFrame();
      ImGui::NewFrame();

      const auto background = ImVec4 {model.background.red, model.background.green, model.background.blue, model.background.alpha};
      ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4 {0.940F, 0.965F, 1.0F, 1.0F});
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2 {20.0F, 14.0F});
      ImGui::SetNextWindowPos({0.0F, 0.0F});
      ImGui::SetNextWindowSize(io.DisplaySize);
      constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
      ImGui::Begin("Sunshine Vulkan UI", nullptr, flags);
      if (model.page == platf::ui::page_e::profile && model.profile.timeline.frame_count != 0) {
        const auto &latest = model.profile.timeline.frames[model.profile.timeline.frame_count - 1U];
        ImGui::Text(
          "Profile Timeline  FRAME %lld  RX EOF +0.0 ms  SEND +%.1f ms  SPANS %u  MISSING 0x%03x  INVALID 0x%03x",
          static_cast<long long>(latest.frame_index),
          latest.end_us / 1000.0,
          latest.span_count,
          latest.missing_stage_mask,
          latest.invalid_stage_mask
        );
      } else if (model.page == platf::ui::page_e::profile && model.profile.available) {
        ImGui::Text(
          "Profile Statistics  C %u  P %u  R %u  BYPASS %u  FRESH-DROP %llu  SAMPLE-DROP %u  %ux%u -> %ux%u",
          model.profile.captured_frames,
          model.profile.placeholder_frames,
          model.profile.repeated_frames,
          model.profile.rga_bypass_frames,
          static_cast<unsigned long long>(model.profile.freshness_drops),
          model.profile.dropped_samples,
          model.profile.hdmirx_width,
          model.profile.hdmirx_height,
          model.profile.moonlight_width,
          model.profile.moonlight_height
        );
      } else {
        ImGui::TextUnformatted(model.page == platf::ui::page_e::main_menu ? "Sunshine" : model.page == platf::ui::page_e::connection_status ? "Connection Status" : "Profile");
      }
      ImGui::Separator();
      if (model.page == platf::ui::page_e::main_menu) {
        if (ImGui::BeginTable("main-menu", 3, ImGuiTableFlags_SizingStretchSame)) {
          constexpr std::array<const char *, 3> labels {"CONNECTION", "PROFILE", "EXIT UI"};
          constexpr std::array<const char *, 3> descriptions {"VIEW STATUS", "VIEW METRICS", "CLOSE OVERLAY"};
          for (std::size_t index = 0; index < labels.size(); ++index) {
            ImGui::TableNextColumn();
            draw_menu_card(labels[index], labels[index], descriptions[index], index == model.focus);
          }
          ImGui::EndTable();
        }
      } else if (model.page == platf::ui::page_e::connection_status) {
        if (ImGui::BeginTable("connection-status", 4, ImGuiTableFlags_SizingStretchSame)) {
          const auto &gamepad = gamepad_status_text(model.connection);
          const auto moonlight = resolution_text(model.connection.moonlight_width, model.connection.moonlight_height);
          const auto input = resolution_text(model.connection.input_width, model.connection.input_height);
          const std::array labels {"VIDEO", "GAMEPAD", "MOONLIGHT", "HDMI INPUT"};
          const std::array values {model.connection.video_state.c_str(), gamepad.c_str(), moonlight.c_str(), input.c_str()};
          for (std::size_t index = 0; index < labels.size(); ++index) {
            ImGui::TableNextColumn();
            draw_menu_card(labels[index], labels[index], values[index], false);
          }
          ImGui::EndTable();
        }
      } else if (model.profile.timeline.frame_count != 0) {
        draw_profile_timeline(model.profile);
      } else if (!model.profile.available) {
        draw_menu_card("profile-waiting", "COLLECTING", "WAITING FOR FIRST 5S WINDOW", false);
      } else if (ImGui::BeginTable("profile-metrics", 4, ImGuiTableFlags_SizingStretchSame)) {
        constexpr std::array labels {
          "RX EOF-DQ",
          "CAP QUEUE",
          "RGA",
          "MPP ENCODE",
          "ENC QUEUE",
          "PACKET-SEND",
          "HOST-PACKET",
          "HOST-SEND"
        };
        for (std::size_t index = 0; index < labels.size(); ++index) {
          const auto description = profile_metric_text(model.profile.metrics[index]);
          ImGui::TableNextColumn();
          draw_menu_card(labels[index], labels[index], description.c_str(), false, 58.0F);
        }
        ImGui::EndTable();
      }
      ImGui::End();
      ImGui::PopStyleVar(3);
      ImGui::PopStyleColor(2);
      ImGui::Render();
    }
  }  // namespace

  bgr888_copy_region_t make_bgr888_copy_region(const bgr888_dma_buf_t &target, std::uint32_t panel_width, std::uint32_t panel_height, std::uint32_t panel_margin) {
    if (target.dma_buf_fd < 0 || target.width == 0 || target.height == 0 || panel_width == 0 || panel_height == 0) {
      throw std::runtime_error("Vulkan UI BGR888 DMA-BUF dimensions or descriptor are invalid");
    }
    const auto minimum_stride = static_cast<std::uint64_t>(target.width) * 3U;
    if (minimum_stride > std::numeric_limits<std::uint32_t>::max() || target.stride < minimum_stride || target.stride % 3U != 0) {
      throw std::runtime_error("Vulkan UI BGR888 DMA-BUF stride is invalid");
    }
    if (target.width < panel_width || target.height < panel_height || panel_margin > target.height - panel_height) {
      throw std::runtime_error("Vulkan UI ROI exceeds the BGR888 DMA-BUF");
    }
    if (static_cast<std::uint64_t>(target.stride) > std::numeric_limits<std::uint64_t>::max() / target.height || target.allocation_size < static_cast<std::uint64_t>(target.stride) * target.height) {
      throw std::runtime_error("Vulkan UI BGR888 DMA-BUF allocation is too small");
    }

    const auto panel_top = target.height - panel_height - panel_margin;
    auto panel_left = (target.width - panel_width) / 2U;
    const auto row_offset = static_cast<std::uint64_t>(panel_top) * target.stride;
    bool aligned = false;
    for (std::uint32_t adjustment = 0; adjustment < 4U && adjustment <= panel_left; ++adjustment) {
      const auto candidate = panel_left - adjustment;
      if ((row_offset + static_cast<std::uint64_t>(candidate) * 3U) % 4U == 0) {
        panel_left = candidate;
        aligned = true;
        break;
      }
    }
    if (!aligned) {
      throw std::runtime_error("Vulkan UI BGR888 ROI cannot satisfy Vulkan buffer offset alignment");
    }
    const auto buffer_offset = row_offset + static_cast<std::uint64_t>(panel_left) * 3U;
    const auto last_row_offset = buffer_offset + static_cast<std::uint64_t>(panel_height - 1U) * target.stride;
    const auto row_bytes = static_cast<std::uint64_t>(panel_width) * 3U;
    if (last_row_offset > std::numeric_limits<std::uint64_t>::max() - row_bytes || last_row_offset + row_bytes > target.allocation_size) {
      throw std::runtime_error("Vulkan UI BGR888 ROI exceeds the DMA-BUF allocation");
    }
    return {
      .buffer_offset = buffer_offset,
      .buffer_row_length = target.stride / 3U,
      .buffer_image_height = target.height,
      .panel_left = panel_left,
      .panel_top = panel_top
    };
  }

  timeline_geometry_t make_timeline_geometry(const video::frame_profile_timeline_snapshot_t &timeline) {
    timeline_geometry_t geometry;
    if (timeline.frame_count == 0 || timeline.frame_count > timeline.frames.size()) {
      return geometry;
    }
    const auto &latest = timeline.frames[timeline.frame_count - 1U];
    const auto latest_end = latest.origin_offset_us + latest.end_us;
    constexpr std::int64_t minimum_window_us = 100000;
    geometry.view_end_us = std::max<std::int64_t>(minimum_window_us, latest_end);
    geometry.view_start_us = std::max<std::int64_t>(0, geometry.view_end_us - std::max(minimum_window_us, latest.end_us));
    geometry.latest_frame_index = latest.frame_index;
    geometry.latest_frame_end_us = latest.end_us;
    const auto view_duration = std::max<std::int64_t>(1, geometry.view_end_us - geometry.view_start_us);

    for (std::size_t frame_index = 0; frame_index < timeline.frame_count; ++frame_index) {
      const auto &frame = timeline.frames[frame_index];
      const auto span_count = std::min<std::size_t>(frame.span_count, frame.spans.size());
      for (std::size_t span_index = 0; span_index < span_count; ++span_index) {
        const auto &span = frame.spans[span_index];
        const auto absolute_start = frame.origin_offset_us + span.start_us;
        const auto absolute_end = frame.origin_offset_us + span.end_us;
        if (absolute_end < geometry.view_start_us || absolute_start > geometry.view_end_us || geometry.bar_count == geometry.bars.size()) {
          continue;
        }
        geometry.bars[geometry.bar_count++] = {
          .stage = span.stage,
          .lane = span.lane,
          .frame_index = frame.frame_index,
          .start_us = span.start_us,
          .end_us = span.end_us,
          .left = std::clamp(static_cast<float>(absolute_start - geometry.view_start_us) / static_cast<float>(view_duration), 0.0F, 1.0F),
          .right = std::clamp(static_cast<float>(absolute_end - geometry.view_start_us) / static_cast<float>(view_duration), 0.0F, 1.0F)
        };
      }
    }
    return geometry;
  }

  std::optional<std::string> validate_render_model(const render_model_t &model) {
    if (model.width == 0 || model.height == 0 || model.revision == 0) {
      return "Vulkan UI model dimensions and revision must be nonzero";
    }
    if (!valid_color(model.background)) {
      return "Vulkan UI background must be finite, normalized, and opaque";
    }
    if (model.focus >= 3) {
      return "Vulkan UI focus exceeds the main menu item count";
    }
    if (model.connection.video_state.size() > 64 || model.connection.gamepad_state.size() > 64 || model.connection.gamepad_stage.size() > 64 || model.connection.failure_kind.size() > 64) {
      return "Vulkan UI connection text exceeds the sanitized display bound";
    }
    for (const auto &metric : model.profile.metrics) {
      if (metric.count != 0 && (metric.p50_us < 0 || metric.p50_us > metric.p95_us || metric.p95_us > metric.p99_us)) {
        return "Vulkan UI profile percentiles are invalid";
      }
    }
    if (model.profile.timeline.frame_count > model.profile.timeline.frames.size()) {
      return "Vulkan UI Timeline frame count exceeds its fixed capacity";
    }
    std::int64_t previous_origin = std::numeric_limits<std::int64_t>::min();
    for (std::size_t frame_index = 0; frame_index < model.profile.timeline.frame_count; ++frame_index) {
      const auto &frame = model.profile.timeline.frames[frame_index];
      if (frame.origin_offset_us < previous_origin || frame.end_us < 0 || frame.span_count > frame.spans.size()) {
        return "Vulkan UI Timeline frame bounds are invalid";
      }
      previous_origin = frame.origin_offset_us;
      for (std::size_t span_index = 0; span_index < frame.span_count; ++span_index) {
        const auto &span = frame.spans[span_index];
        if (span.start_us < 0 || span.end_us < span.start_us || span.end_us > frame.end_us ||
            static_cast<std::size_t>(span.stage) >= static_cast<std::size_t>(video::frame_profile_timeline_stage_e::count) ||
            static_cast<std::size_t>(span.lane) >= static_cast<std::size_t>(video::frame_profile_timeline_lane_e::count)) {
          return "Vulkan UI Timeline span bounds are invalid";
        }
      }
    }
    return std::nullopt;
  }

  render_model_t make_render_model(std::uint32_t width, std::uint32_t height, const platf::ui::snapshot_t &snapshot) {
    if (width < 960 || height < 180) {
      throw std::runtime_error("Vulkan UI modal page requires at least a 960x180 panel");
    }
    const color_t background {0.025F, 0.035F, 0.060F, 1.0F};
    render_model_t model {width, height, snapshot.revision, background, snapshot.page, snapshot.focus, snapshot.connection, snapshot.profile};
    if (const auto error = validate_render_model(model)) {
      throw std::runtime_error(*error);
    }
    return model;
  }

  class renderer_t::impl_t {
  public:
    impl_t(int dma_buf_fd, std::uint64_t allocation_size, std::uint32_t width, std::uint32_t height, std::uint32_t stride):
        width_(width),
        height_(height),
        stride_(stride),
        allocation_size_(allocation_size) {
      if (dma_buf_fd < 0 || width == 0 || height == 0 || stride < static_cast<std::uint64_t>(width) * 3U || stride % 3U != 0 || allocation_size < static_cast<std::uint64_t>(stride) * height) {
        throw std::runtime_error("Vulkan UI DMA-BUF layout is invalid");
      }
      try {
        initialize(dma_buf_fd);
      } catch (...) {
        destroy();
        throw;
      }
    }

    ~impl_t() {
      destroy();
    }

    bool render(const render_model_t &model) {
      if (model.width != width_ || model.height != height_) {
        throw std::runtime_error("Vulkan UI model dimensions do not match the imported DMA-BUF");
      }
      if (const auto error = validate_render_model(model)) {
        throw std::runtime_error(*error);
      }
      if (model.revision == rendered_revision_) {
        return false;
      }
      ImGui::SetCurrentContext(imgui_context_);
      build_modal_imgui_page(model);

      require_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(Vulkan UI)");
      require_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(Vulkan UI)");
      require_vk(vkResetCommandBuffer(command_, 0), "vkResetCommandBuffer(Vulkan UI)");
      VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      require_vk(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer(Vulkan UI)");

      VkClearValue clear {};
      clear.color = clear_color(model.background);
      VkRenderPassBeginInfo render_begin {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      render_begin.renderPass = render_pass_;
      render_begin.framebuffer = framebuffer_;
      render_begin.renderArea.extent = {width_, height_};
      render_begin.clearValueCount = 1;
      render_begin.pClearValues = &clear;
      vkCmdBeginRenderPass(command_, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_);
      vkCmdEndRenderPass(command_);

      VkImageMemoryBarrier render_complete {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      render_complete.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      render_complete.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      render_complete.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      render_complete.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      render_complete.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      render_complete.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      render_complete.image = render_image_;
      render_complete.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &render_complete);

      require_vk(vkEndCommandBuffer(command_), "vkEndCommandBuffer(Vulkan UI)");

      VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command_;
      require_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(Vulkan UI)");
      require_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(Vulkan UI completion)");
      rendered_revision_ = model.revision;
      return true;
    }

    bool publish() {
      if (rendered_revision_ == 0) {
        throw std::runtime_error("Vulkan UI panel has not been rendered");
      }
      if (published_revision_ == rendered_revision_) {
        return false;
      }
      submit_copy(output_buffer_, 0, stride_ / 3U, height_);
      published_revision_ = rendered_revision_;
      return true;
    }

    bool cover_bgr888(const bgr888_dma_buf_t &target, std::uint32_t panel_margin) {
      if (rendered_revision_ == 0) {
        throw std::runtime_error("Vulkan UI panel has not been rendered");
      }
      const auto region = make_bgr888_copy_region(target, width_, height_, panel_margin);
      if (!capture_generation_ || *capture_generation_ != target.generation) {
        clear_capture_targets();
        capture_generation_ = target.generation;
      }
      auto destination = capture_targets_.find(target.slot);
      bool imported = false;
      if (destination == capture_targets_.end()) {
        destination = capture_targets_.emplace(target.slot, import_capture_target(target)).first;
        imported = true;
      } else {
        const auto &cached = destination->second.layout;
        if (cached.dma_buf_fd != target.dma_buf_fd || cached.allocation_size != target.allocation_size || cached.width != target.width || cached.height != target.height || cached.stride != target.stride) {
          throw std::runtime_error("Vulkan UI capture slot layout changed within one generation");
        }
      }
      submit_copy(destination->second.buffer, region.buffer_offset, region.buffer_row_length, region.buffer_image_height);
      return imported;
    }

    std::uint64_t rendered_revision() const noexcept {
      return rendered_revision_;
    }

    std::string device_name() const {
      return device_name_;
    }

  private:
    struct imported_capture_target_t {
      VkBuffer buffer {VK_NULL_HANDLE};
      VkDeviceMemory memory {VK_NULL_HANDLE};
      bgr888_dma_buf_t layout;
    };

    imported_capture_target_t import_capture_target(const bgr888_dma_buf_t &target) {
      VkBuffer buffer = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      try {
        VkExternalMemoryBufferCreateInfo external_buffer {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        external_buffer.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkBufferCreateInfo buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.pNext = &external_buffer;
        buffer_info.size = target.allocation_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        require_vk(vkCreateBuffer(device_, &buffer_info, nullptr, &buffer), "vkCreateBuffer(Vulkan UI capture target)");

        VkMemoryRequirements requirements {};
        vkGetBufferMemoryRequirements(device_, buffer, &requirements);
        if (requirements.size > target.allocation_size) {
          throw std::runtime_error("Vulkan UI capture DMA-BUF is smaller than Vulkan buffer requirements");
        }
        const auto get_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
        if (!get_fd_properties) {
          throw std::runtime_error("vkGetMemoryFdPropertiesKHR is unavailable");
        }
        VkMemoryFdPropertiesKHR fd_properties {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        require_vk(get_fd_properties(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, target.dma_buf_fd, &fd_properties), "vkGetMemoryFdPropertiesKHR(Vulkan UI capture target)");
        const auto compatible_types = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
        if (compatible_types == 0) {
          throw std::runtime_error("Vulkan UI capture DMA-BUF has no compatible memory type");
        }
        fd_t import_fd(::dup(target.dma_buf_fd));
        if (import_fd.get() < 0) {
          throw std::runtime_error("dup(Vulkan UI capture DMA-BUF) failed: " + std::string(std::strerror(errno)));
        }
        VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = buffer;
        VkImportMemoryFdInfoKHR import {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        import.pNext = &dedicated;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        import.fd = import_fd.get();
        VkMemoryAllocateInfo allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &import;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = choose_memory_type(physical_device_, compatible_types);
        require_vk(vkAllocateMemory(device_, &allocation, nullptr, &memory), "vkAllocateMemory(Vulkan UI capture target)");
        (void) import_fd.release();
        require_vk(vkBindBufferMemory(device_, buffer, memory, 0), "vkBindBufferMemory(Vulkan UI capture target)");
        return {buffer, memory, target};
      } catch (...) {
        if (memory != VK_NULL_HANDLE) {
          vkFreeMemory(device_, memory, nullptr);
        }
        if (buffer != VK_NULL_HANDLE) {
          vkDestroyBuffer(device_, buffer, nullptr);
        }
        throw;
      }
    }

    void submit_copy(VkBuffer destination, std::uint64_t buffer_offset, std::uint32_t buffer_row_length, std::uint32_t buffer_image_height) {
      require_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(Vulkan UI copy)");
      require_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(Vulkan UI copy)");
      require_vk(vkResetCommandBuffer(command_, 0), "vkResetCommandBuffer(Vulkan UI copy)");
      VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      require_vk(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer(Vulkan UI copy)");

      VkBufferMemoryBarrier acquire {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      acquire.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      acquire.dstQueueFamilyIndex = queue_family_;
      acquire.buffer = destination;
      acquire.offset = 0;
      acquire.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &acquire, 0, nullptr);

      VkBufferImageCopy copy {};
      copy.bufferOffset = buffer_offset;
      copy.bufferRowLength = buffer_row_length;
      copy.bufferImageHeight = buffer_image_height;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {width_, height_, 1};
      vkCmdCopyImageToBuffer(command_, render_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, 1, &copy);

      VkBufferMemoryBarrier release {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      release.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      release.srcQueueFamilyIndex = queue_family_;
      release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      release.buffer = destination;
      release.offset = 0;
      release.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &release, 0, nullptr);
      require_vk(vkEndCommandBuffer(command_), "vkEndCommandBuffer(Vulkan UI copy)");

      VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command_;
      require_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(Vulkan UI copy)");
      require_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(Vulkan UI copy completion)");
    }

    void clear_capture_targets() noexcept {
      if (device_ != VK_NULL_HANDLE) {
        (void) vkDeviceWaitIdle(device_);
        for (auto &[slot, target] : capture_targets_) {
          (void) slot;
          if (target.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device_, target.buffer, nullptr);
          }
          if (target.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device_, target.memory, nullptr);
          }
        }
      }
      capture_targets_.clear();
      capture_generation_.reset();
    }

    void initialize(int dma_buf_fd) {
      VkApplicationInfo application {VK_STRUCTURE_TYPE_APPLICATION_INFO};
      application.pApplicationName = "Sunshine Vulkan UI";
      application.apiVersion = VK_API_VERSION_1_1;
      VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
      instance_info.pApplicationInfo = &application;
      require_vk(vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance(Vulkan UI)");

      const auto selected = select_device(instance_);
      physical_device_ = selected.device;
      queue_family_ = selected.queue_family;
      device_name_ = selected.properties.deviceName;

      VkPhysicalDeviceExternalBufferInfo external_query {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
      external_query.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      external_query.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      VkExternalBufferProperties external_properties {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
      vkGetPhysicalDeviceExternalBufferProperties(physical_device_, &external_query, &external_properties);
      if ((external_properties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
        throw std::runtime_error("Vulkan device cannot import a DMA-BUF transfer destination");
      }

      const float priority = 1.0F;
      VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queue_info.queueFamilyIndex = queue_family_;
      queue_info.queueCount = 1;
      queue_info.pQueuePriorities = &priority;
      constexpr std::array extensions {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      };
      VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      device_info.queueCreateInfoCount = 1;
      device_info.pQueueCreateInfos = &queue_info;
      device_info.enabledExtensionCount = extensions.size();
      device_info.ppEnabledExtensionNames = extensions.data();
      require_vk(vkCreateDevice(physical_device_, &device_info, nullptr, &device_), "vkCreateDevice(Vulkan UI)");
      vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

      VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image_info.imageType = VK_IMAGE_TYPE_2D;
      image_info.format = panel_format;
      image_info.extent = {width_, height_, 1};
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.samples = VK_SAMPLE_COUNT_1_BIT;
      image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
      image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      require_vk(vkCreateImage(device_, &image_info, nullptr, &render_image_), "vkCreateImage(Vulkan UI)");
      VkMemoryRequirements image_requirements {};
      vkGetImageMemoryRequirements(device_, render_image_, &image_requirements);
      VkMemoryAllocateInfo image_allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      image_allocation.allocationSize = image_requirements.size;
      image_allocation.memoryTypeIndex = choose_memory_type(physical_device_, image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      require_vk(vkAllocateMemory(device_, &image_allocation, nullptr, &render_memory_), "vkAllocateMemory(Vulkan UI image)");
      require_vk(vkBindImageMemory(device_, render_image_, render_memory_, 0), "vkBindImageMemory(Vulkan UI)");

      VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.image = render_image_;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = panel_format;
      view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      require_vk(vkCreateImageView(device_, &view_info, nullptr, &image_view_), "vkCreateImageView(Vulkan UI)");

      VkAttachmentDescription attachment {};
      attachment.format = panel_format;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      VkAttachmentReference reference {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass {};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkSubpassDependency dependency {};
      dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
      dependency.dstSubpass = 0;
      dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
      dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo render_pass_info {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      render_pass_info.attachmentCount = 1;
      render_pass_info.pAttachments = &attachment;
      render_pass_info.subpassCount = 1;
      render_pass_info.pSubpasses = &subpass;
      render_pass_info.dependencyCount = 1;
      render_pass_info.pDependencies = &dependency;
      require_vk(vkCreateRenderPass(device_, &render_pass_info, nullptr, &render_pass_), "vkCreateRenderPass(Vulkan UI)");
      VkFramebufferCreateInfo framebuffer_info {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      framebuffer_info.renderPass = render_pass_;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &image_view_;
      framebuffer_info.width = width_;
      framebuffer_info.height = height_;
      framebuffer_info.layers = 1;
      require_vk(vkCreateFramebuffer(device_, &framebuffer_info, nullptr, &framebuffer_), "vkCreateFramebuffer(Vulkan UI)");

      VkExternalMemoryBufferCreateInfo external_buffer {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
      external_buffer.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      VkBufferCreateInfo buffer_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      buffer_info.pNext = &external_buffer;
      buffer_info.size = allocation_size_;
      buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      require_vk(vkCreateBuffer(device_, &buffer_info, nullptr, &output_buffer_), "vkCreateBuffer(Vulkan UI output)");
      VkMemoryRequirements buffer_requirements {};
      vkGetBufferMemoryRequirements(device_, output_buffer_, &buffer_requirements);
      if (buffer_requirements.size > allocation_size_) {
        throw std::runtime_error("Vulkan UI DMA-BUF allocation is smaller than Vulkan buffer requirements");
      }
      const auto get_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
      if (!get_fd_properties) {
        throw std::runtime_error("vkGetMemoryFdPropertiesKHR is unavailable");
      }
      VkMemoryFdPropertiesKHR fd_properties {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
      require_vk(get_fd_properties(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf_fd, &fd_properties), "vkGetMemoryFdPropertiesKHR(Vulkan UI)");
      const auto compatible_types = buffer_requirements.memoryTypeBits & fd_properties.memoryTypeBits;
      if (compatible_types == 0) {
        throw std::runtime_error("Vulkan UI DMA-BUF has no compatible memory type");
      }
      fd_t import_fd(::dup(dma_buf_fd));
      if (import_fd.get() < 0) {
        throw std::runtime_error("dup(Vulkan UI DMA-BUF) failed: " + std::string(std::strerror(errno)));
      }
      VkMemoryDedicatedAllocateInfo dedicated {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
      dedicated.buffer = output_buffer_;
      VkImportMemoryFdInfoKHR import {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
      import.pNext = &dedicated;
      import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      import.fd = import_fd.get();
      VkMemoryAllocateInfo output_allocation {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      output_allocation.pNext = &import;
      output_allocation.allocationSize = buffer_requirements.size;
      output_allocation.memoryTypeIndex = choose_memory_type(physical_device_, compatible_types);
      require_vk(vkAllocateMemory(device_, &output_allocation, nullptr, &output_memory_), "vkAllocateMemory(Vulkan UI DMA-BUF import)");
      (void) import_fd.release();
      require_vk(vkBindBufferMemory(device_, output_buffer_, output_memory_, 0), "vkBindBufferMemory(Vulkan UI)");

      VkCommandPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pool_info.queueFamilyIndex = queue_family_;
      require_vk(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool(Vulkan UI)");
      VkCommandBufferAllocateInfo command_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      command_info.commandPool = command_pool_;
      command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_info.commandBufferCount = 1;
      require_vk(vkAllocateCommandBuffers(device_, &command_info, &command_), "vkAllocateCommandBuffers(Vulkan UI)");
      VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      require_vk(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence(Vulkan UI)");

      IMGUI_CHECKVERSION();
      imgui_context_ = ImGui::CreateContext();
      ImGui::SetCurrentContext(imgui_context_);
      auto &io = ImGui::GetIO();
      io.IniFilename = nullptr;
      io.LogFilename = nullptr;
      io.DisplaySize = {static_cast<float>(width_), static_cast<float>(height_)};
      ImGui::StyleColorsDark();
      ImGui_ImplVulkan_InitInfo imgui_info {};
      imgui_info.ApiVersion = VK_API_VERSION_1_1;
      imgui_info.Instance = instance_;
      imgui_info.PhysicalDevice = physical_device_;
      imgui_info.Device = device_;
      imgui_info.QueueFamily = queue_family_;
      imgui_info.Queue = queue_;
      imgui_info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
      imgui_info.MinImageCount = 2;
      imgui_info.ImageCount = 2;
      imgui_info.PipelineInfoMain.RenderPass = render_pass_;
      if (!ImGui_ImplVulkan_Init(&imgui_info)) {
        throw std::runtime_error("ImGui Vulkan renderer initialization failed");
      }
      imgui_ready_ = true;
    }

    void destroy() noexcept {
      if (device_ != VK_NULL_HANDLE) {
        (void) vkDeviceWaitIdle(device_);
        clear_capture_targets();
        if (imgui_context_) {
          ImGui::SetCurrentContext(imgui_context_);
          if (imgui_ready_) {
            ImGui_ImplVulkan_Shutdown();
          }
          ImGui::DestroyContext(imgui_context_);
          imgui_context_ = nullptr;
        }
        if (fence_ != VK_NULL_HANDLE) {
          vkDestroyFence(device_, fence_, nullptr);
        }
        if (command_pool_ != VK_NULL_HANDLE) {
          vkDestroyCommandPool(device_, command_pool_, nullptr);
        }
        if (framebuffer_ != VK_NULL_HANDLE) {
          vkDestroyFramebuffer(device_, framebuffer_, nullptr);
        }
        if (render_pass_ != VK_NULL_HANDLE) {
          vkDestroyRenderPass(device_, render_pass_, nullptr);
        }
        if (image_view_ != VK_NULL_HANDLE) {
          vkDestroyImageView(device_, image_view_, nullptr);
        }
        if (output_buffer_ != VK_NULL_HANDLE) {
          vkDestroyBuffer(device_, output_buffer_, nullptr);
        }
        if (render_image_ != VK_NULL_HANDLE) {
          vkDestroyImage(device_, render_image_, nullptr);
        }
        if (output_memory_ != VK_NULL_HANDLE) {
          vkFreeMemory(device_, output_memory_, nullptr);
        }
        if (render_memory_ != VK_NULL_HANDLE) {
          vkFreeMemory(device_, render_memory_, nullptr);
        }
        vkDestroyDevice(device_, nullptr);
      }
      if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
      }
      device_ = VK_NULL_HANDLE;
      instance_ = VK_NULL_HANDLE;
    }

    std::uint32_t width_ {};
    std::uint32_t height_ {};
    std::uint32_t stride_ {};
    std::uint64_t allocation_size_ {};
    std::uint64_t rendered_revision_ {};
    std::uint64_t published_revision_ {};
    std::string device_name_;
    VkInstance instance_ {VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_ {VK_NULL_HANDLE};
    VkDevice device_ {VK_NULL_HANDLE};
    VkQueue queue_ {VK_NULL_HANDLE};
    std::uint32_t queue_family_ {};
    VkImage render_image_ {VK_NULL_HANDLE};
    VkDeviceMemory render_memory_ {VK_NULL_HANDLE};
    VkImageView image_view_ {VK_NULL_HANDLE};
    VkRenderPass render_pass_ {VK_NULL_HANDLE};
    VkFramebuffer framebuffer_ {VK_NULL_HANDLE};
    VkBuffer output_buffer_ {VK_NULL_HANDLE};
    VkDeviceMemory output_memory_ {VK_NULL_HANDLE};
    VkCommandPool command_pool_ {VK_NULL_HANDLE};
    VkCommandBuffer command_ {VK_NULL_HANDLE};
    VkFence fence_ {VK_NULL_HANDLE};
    ImGuiContext *imgui_context_ {};
    bool imgui_ready_ {};
    std::unordered_map<std::uint32_t, imported_capture_target_t> capture_targets_;
    std::optional<std::uint64_t> capture_generation_;
  };

  renderer_t::renderer_t(std::unique_ptr<impl_t> impl) noexcept:
      impl_(std::move(impl)) {}

  renderer_t::~renderer_t() = default;

  std::unique_ptr<renderer_t> renderer_t::create(int dma_buf_fd, std::uint64_t allocation_size, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    return std::unique_ptr<renderer_t>(new renderer_t(std::make_unique<impl_t>(dma_buf_fd, allocation_size, width, height, stride)));
  }

  bool renderer_t::render(const render_model_t &model) {
    return impl_->render(model);
  }

  bool renderer_t::publish() {
    return impl_->publish();
  }

  bool renderer_t::cover_bgr888(const bgr888_dma_buf_t &target, std::uint32_t panel_margin) {
    return impl_->cover_bgr888(target, panel_margin);
  }

  std::uint64_t renderer_t::rendered_revision() const noexcept {
    return impl_->rendered_revision();
  }

  std::string renderer_t::device_name() const {
    return impl_->device_name();
  }
}  // namespace platf::vulkan_ui
