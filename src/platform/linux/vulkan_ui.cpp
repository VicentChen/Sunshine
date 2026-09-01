/**
 * @file src/platform/linux/vulkan_ui.cpp
 * @brief Vulkan UI rendering into an external RGBA DMA-BUF for RKMPP.
 */

// standard includes
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
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
    constexpr VkFormat panel_format = VK_FORMAT_R8G8B8A8_UNORM;

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

    /** @brief Build the temporary read-only page entirely through Dear ImGui. */
    void build_gate5_imgui_page(const render_model_t &model) {
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
      ImGui::TextUnformatted("Vulkan UI");
      ImGui::Separator();
      if (ImGui::BeginTable("status", 3, ImGuiTableFlags_SizingStretchSame)) {
        constexpr std::array<const char *, 3> labels {"VIDEO", "INPUT", "STATUS"};
        constexpr std::array<const char *, 3> values {"ON", "IMGUI READY", "READY"};
        for (std::size_t index = 0; index < labels.size(); ++index) {
          ImGui::TableNextColumn();
          const auto card = index == model.focus ? ImVec4 {0.055F, 0.330F, 0.720F, 1.0F} : ImVec4 {0.075F, 0.095F, 0.130F, 1.0F};
          ImGui::PushStyleColor(ImGuiCol_ChildBg, card);
          ImGui::BeginChild(labels[index], ImVec2 {0.0F, 74.0F}, ImGuiChildFlags_Borders);
          ImGui::TextUnformatted(labels[index]);
          ImGui::Spacing();
          ImGui::TextUnformatted(values[index]);
          ImGui::EndChild();
          ImGui::PopStyleColor();
        }
        ImGui::EndTable();
      }
      ImGui::End();
      ImGui::PopStyleVar(3);
      ImGui::PopStyleColor(2);
      ImGui::Render();
    }
  }  // namespace

  std::optional<std::string> validate_render_model(const render_model_t &model) {
    if (model.width == 0 || model.height == 0 || model.revision == 0) {
      return "Vulkan UI model dimensions and revision must be nonzero";
    }
    if (!valid_color(model.background)) {
      return "Vulkan UI background must be finite, normalized, and opaque";
    }
    if (model.focus >= 3) {
      return "Vulkan UI focus exceeds the status page item count";
    }
    return std::nullopt;
  }

  render_model_t make_status_model(std::uint32_t width, std::uint32_t height, std::uint8_t focus, std::uint64_t revision) {
    if (width < 960 || height < 180) {
      throw std::runtime_error("Vulkan UI status page requires at least a 960x180 panel");
    }
    const color_t background {0.025F, 0.035F, 0.060F, 1.0F};
    render_model_t model {width, height, revision, background, focus};
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
      if (dma_buf_fd < 0 || width == 0 || height == 0 || stride < static_cast<std::uint64_t>(width) * 4U || stride % 4U != 0 || allocation_size < static_cast<std::uint64_t>(stride) * height) {
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
      build_gate5_imgui_page(model);

      require_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(Vulkan UI)");
      require_vk(vkResetFences(device_, 1, &fence_), "vkResetFences(Vulkan UI)");
      require_vk(vkResetCommandBuffer(command_, 0), "vkResetCommandBuffer(Vulkan UI)");
      VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      require_vk(vkBeginCommandBuffer(command_, &begin), "vkBeginCommandBuffer(Vulkan UI)");

      VkBufferMemoryBarrier acquire {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      acquire.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      acquire.dstQueueFamilyIndex = queue_family_;
      acquire.buffer = output_buffer_;
      acquire.offset = 0;
      acquire.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &acquire, 0, nullptr);

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

      VkBufferImageCopy copy {};
      copy.bufferRowLength = stride_ / 4U;
      copy.bufferImageHeight = height_;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {width_, height_, 1};
      vkCmdCopyImageToBuffer(command_, render_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output_buffer_, 1, &copy);

      VkBufferMemoryBarrier release {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      release.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
      release.srcQueueFamilyIndex = queue_family_;
      release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      release.buffer = output_buffer_;
      release.offset = 0;
      release.size = VK_WHOLE_SIZE;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 1, &release, 0, nullptr);
      require_vk(vkEndCommandBuffer(command_), "vkEndCommandBuffer(Vulkan UI)");

      VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &command_;
      require_vk(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(Vulkan UI)");
      require_vk(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(Vulkan UI completion)");
      rendered_revision_ = model.revision;
      return true;
    }

    std::uint64_t rendered_revision() const noexcept {
      return rendered_revision_;
    }

    std::string device_name() const {
      return device_name_;
    }

  private:
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

  std::uint64_t renderer_t::rendered_revision() const noexcept {
    return impl_->rendered_revision();
  }

  std::string renderer_t::device_name() const {
    return impl_->device_name();
  }
}  // namespace platf::vulkan_ui
