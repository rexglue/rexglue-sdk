/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/string.h>
#include "gpu/xenos/host/vulkan/device.h"
#include "gpu/xenos/host/vulkan/objects.h"

#include "gpu/xenos/cvar_table.h"

namespace rex {
namespace gpu {
namespace vulkan {

template <typename Structure, VkStructureType StructureType>
struct VulkanFeatures {
  Structure supported = objects::InitializeStructure<Structure>(StructureType);
  Structure enabled = objects::InitializeStructure<Structure>(StructureType);

  void Link(VkPhysicalDeviceFeatures2& supported_features_2,
            VkDeviceCreateInfo& device_create_info) {
    supported.pNext = supported_features_2.pNext;
    supported_features_2.pNext = &supported;
    enabled.pNext = const_cast<void*>(device_create_info.pNext);
    device_create_info.pNext = &enabled;
  }
};

std::unique_ptr<VulkanDevice> VulkanDevice::CreateIfSupported(
    const VulkanInstance* const vulkan_instance, const VkPhysicalDevice physical_device,
    const bool with_gpu_emulation, const bool with_swapchain) {
  REX_ASSERT(vulkan_instance != nullptr);
  REX_ASSERT(physical_device != nullptr);

  const VulkanInstance::Functions& ifn = vulkan_instance->GetFunctions();

  VkPhysicalDeviceProperties properties = {};
  ifn.vkGetPhysicalDeviceProperties(physical_device, &properties);

  const u32 unclamped_api_version = properties.apiVersion;
  if (vulkan_instance->GetAPIVersion() < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    properties.apiVersion =
        VK_MAKE_API_VERSION(0, 1, 0, VK_API_VERSION_PATCH(properties.apiVersion));
  }

  VkPhysicalDeviceFeatures supported_features = {};
  ifn.vkGetPhysicalDeviceFeatures(physical_device, &supported_features);

  if (with_gpu_emulation) {
    if (!supported_features.independentBlend) {
      REXLOG_WARN(ui,
                  "Vulkan device '{}' doesn't support the independentBlend feature "
                  "required for GPU emulation",
                  properties.deviceName);
      return nullptr;
    }
    if (!supported_features.fragmentStoresAndAtomics) {
      REXLOG_WARN(ui,
                  "Vulkan device '{}' doesn't support fragmentStoresAndAtomics, which "
                  "is required for Vulkan GPU emulation parity",
                  properties.deviceName);
      return nullptr;
    }
    if (!supported_features.vertexPipelineStoresAndAtomics) {
      REXLOG_WARN(ui,
                  "Vulkan device '{}' doesn't support vertexPipelineStoresAndAtomics, which "
                  "is required for Vulkan GPU emulation parity",
                  properties.deviceName);
      return nullptr;
    }
    if (REXCVAR_GET(::rex::gpu::cvars::require_geometry_shader) &&
        !supported_features.geometryShader) {
      REXLOG_WARN(ui,
                  "Vulkan device '{}' doesn't support geometryShader, but "
                  "vulkan.require_geometry_shader=true",
                  properties.deviceName);
      return nullptr;
    }
    if (REXCVAR_GET(::rex::gpu::cvars::require_fill_mode_non_solid) &&
        !supported_features.fillModeNonSolid) {
      REXLOG_WARN(ui,
                  "Vulkan device '{}' doesn't support fillModeNonSolid, but "
                  "vulkan.require_fill_mode_non_solid=true",
                  properties.deviceName);
      return nullptr;
    }
  }

  auto device = std::make_unique<VulkanDevice>(Key{}, vulkan_instance, physical_device);
  device->requires_geometry_shader_ = REXCVAR_GET(::rex::gpu::cvars::require_geometry_shader);
  device->requires_fill_mode_non_solid_ =
      REXCVAR_GET(::rex::gpu::cvars::require_fill_mode_non_solid);

  const bool get_physical_device_properties2_supported =
      vulkan_instance->GetExtensions().ext_1_1_KHR_get_physical_device_properties2;

  // Name pointers from requested_extensions are used in the enabled extensions vector.
  std::unordered_map<std::string, bool*> requested_extensions;

  const auto request_promoted_extension = [&](const char* const name, u32 const major,
                                              u32 const minor, bool* const supported_ptr) {
    REX_ASSERT(supported_ptr != nullptr);
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, major, minor, 0)) {
      *supported_ptr = true;
    } else {
      requested_extensions.emplace(name, supported_ptr);
    }
  };

#define REX_VK_STRUCT_EXTENSION(name) \
  requested_extensions.emplace("VK_" #name, &device->extensions_.ext_##name);
/**
 * Requests a device extension whose support flag is a local variable.
 *
 * @param name Extension name without the VK_ prefix.
 *
 * @code
 * REX_VK_LOCAL_EXTENSION(KHR_portability_subset)
 * @endcode
 */
#define REX_VK_LOCAL_EXTENSION(name) requested_extensions.emplace("VK_" #name, &ext_##name);
/**
 * Requests an extension promoted to core, with its support flag in the extensions struct.
 *
 * @param name Extension name without the VK_ prefix.
 * @param major Major number of the core version that promoted it.
 * @param minor Minor number of the core version that promoted it.
 *
 * @code
 * REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_dedicated_allocation, 1, 1)
 * @endcode
 */
#define REX_VK_STRUCT_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension("VK_" #name, major, minor,      \
                             &device->extensions_.ext_##major##_##minor##_##name);
/**
 * Requests an extension promoted to core, with its support flag as a local variable.
 *
 * @param name Extension name without the VK_ prefix.
 * @param major Major number of the core version that promoted it.
 * @param minor Minor number of the core version that promoted it.
 *
 * @code
 * REX_VK_LOCAL_PROMOTED_EXTENSION(KHR_driver_properties, 1, 2)
 * @endcode
 */
#define REX_VK_LOCAL_PROMOTED_EXTENSION(name, major, minor) \
  request_promoted_extension("VK_" #name, major, minor, &ext_##major##_##minor##_##name);

  bool ext_KHR_portability_subset = false;
  bool ext_1_2_KHR_driver_properties = false;
  if (get_physical_device_properties2_supported) {
    // Must be enabled when the physical device is a portability subset device.
    REX_VK_LOCAL_EXTENSION(KHR_portability_subset)
    REX_VK_LOCAL_PROMOTED_EXTENSION(KHR_driver_properties, 1, 2)
  }

  REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_dedicated_allocation, 1, 1)
  // Also required by VK_KHR_dedicated_allocation and VK_KHR_sampler_ycbcr_conversion.
  REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_get_memory_requirements2, 1, 1)
  // Also required by VK_KHR_sampler_ycbcr_conversion.
  REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_bind_memory2, 1, 1)
  if (get_physical_device_properties2_supported) {
    REX_VK_STRUCT_EXTENSION(EXT_memory_budget)
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_maintenance4, 1, 3)
    REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_dynamic_rendering, 1, 3)
  }

  if (with_swapchain) {
    REX_VK_STRUCT_EXTENSION(KHR_swapchain)
  }

  bool ext_1_2_KHR_sampler_mirror_clamp_to_edge = false;
  bool ext_1_1_KHR_maintenance1 = false;
  bool ext_1_2_KHR_shader_float_controls = false;
  bool ext_EXT_fragment_shader_interlock = false;
  bool ext_1_3_EXT_shader_demote_to_helper_invocation = false;
  bool ext_1_3_KHR_dynamic_rendering = false;
  bool ext_EXT_non_seamless_cube_map = false;
  if (with_gpu_emulation) {
    REX_VK_LOCAL_PROMOTED_EXTENSION(KHR_sampler_mirror_clamp_to_edge, 1, 2)
    // Required by VK_KHR_sampler_ycbcr_conversion.
    REX_VK_LOCAL_PROMOTED_EXTENSION(KHR_maintenance1, 1, 1)
    REX_VK_STRUCT_EXTENSION(EXT_shader_stencil_export)
    REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_image_format_list, 1, 2)
    if (get_physical_device_properties2_supported) {
      REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_sampler_ycbcr_conversion, 1, 1)
      // Also required by VK_KHR_spirv_1_4.
      REX_VK_LOCAL_PROMOTED_EXTENSION(KHR_shader_float_controls, 1, 2)
      REX_VK_LOCAL_EXTENSION(EXT_fragment_shader_interlock)
      REX_VK_LOCAL_PROMOTED_EXTENSION(KHR_dynamic_rendering, 1, 3)
      REX_VK_LOCAL_PROMOTED_EXTENSION(EXT_shader_demote_to_helper_invocation, 1, 3)
      REX_VK_LOCAL_EXTENSION(EXT_non_seamless_cube_map)
      // Required for non-black YCbCr border color parity with D3D12.
      REX_VK_STRUCT_EXTENSION(EXT_custom_border_color)
      // Required for true null descriptors in bindless texture bindings.
      REX_VK_STRUCT_EXTENSION(EXT_robustness2)
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
      REX_VK_STRUCT_PROMOTED_EXTENSION(KHR_spirv_1_4, 1, 2)
    }
  }

#undef REX_VK_STRUCT_EXTENSION
#undef REX_VK_LOCAL_EXTENSION
#undef REX_VK_STRUCT_PROMOTED_EXTENSION
#undef REX_VK_LOCAL_PROMOTED_EXTENSION

  std::vector<const char*> enabled_extensions;
  {
    u32 supported_extension_count = 0;
    const VkResult get_supported_extension_count_result = ifn.vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &supported_extension_count, nullptr);
    if (get_supported_extension_count_result != VK_SUCCESS &&
        get_supported_extension_count_result != VK_INCOMPLETE) {
      REXLOG_WARN(ui, "Failed to get the Vulkan device '{}' extension count",
                  properties.deviceName);
      return nullptr;
    }
    if (supported_extension_count) {
      std::vector<VkExtensionProperties> supported_extensions(supported_extension_count);
      if (ifn.vkEnumerateDeviceExtensionProperties(physical_device, nullptr,
                                                   &supported_extension_count,
                                                   supported_extensions.data()) != VK_SUCCESS) {
        REXLOG_WARN(ui, "Failed to get the Vulkan device '{}' extensions", properties.deviceName);
        return nullptr;
      }
      REX_ASSERT(supported_extension_count == supported_extensions.size());
      for (const VkExtensionProperties& supported_extension : supported_extensions) {
        const auto requested_extension_it =
            requested_extensions.find(supported_extension.extensionName);
        if (requested_extension_it == requested_extensions.cend()) {
          continue;
        }
        REX_ASSERT(requested_extension_it->second != nullptr);
        if (!*requested_extension_it->second) {
          enabled_extensions.emplace_back(requested_extension_it->first.c_str());
          *requested_extension_it->second = true;
        }
      }
    }
  }

  if (with_swapchain && !device->extensions_.ext_KHR_swapchain) {
    REXLOG_WARN(ui, "Vulkan device '{}' doesn't support swapchains", properties.deviceName);
    return nullptr;
  }

  auto device_create_info =
      objects::InitializeStructure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

  device_create_info.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
  device_create_info.ppEnabledExtensionNames = enabled_extensions.data();

  auto properties_2 = objects::InitializeStructure<VkPhysicalDeviceProperties2>(
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);

  auto supported_features_2 = objects::InitializeStructure<VkPhysicalDeviceFeatures2>(
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);

  VulkanFeatures<VkPhysicalDeviceVulkan12Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES>
      features_1_2;
  VulkanFeatures<VkPhysicalDeviceVulkan13Features,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES>
      features_1_3;
  VulkanFeatures<VkPhysicalDevicePortabilitySubsetFeaturesKHR,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR>
      features_khr_portability_subset;
  VkPhysicalDeviceDriverPropertiesKHR properties_1_2_khr_driver_properties = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
      .pNext = nullptr,
      .driverID = VkDriverId(0),
      .driverName = {},
      .driverInfo = {},
      .conformanceVersion = {}};
  auto properties_1_2_khr_shader_float_controls =
      objects::InitializeStructure<VkPhysicalDeviceFloatControlsProperties>(
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES);
  VulkanFeatures<VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT>
      features_ext_fragment_shader_interlock;
  VulkanFeatures<VkPhysicalDeviceDynamicRenderingFeaturesKHR,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR>
      features_1_3_khr_dynamic_rendering;
  VulkanFeatures<VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT>
      features_1_3_ext_shader_demote_to_helper_invocation;
  VulkanFeatures<VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT>
      features_ext_non_seamless_cube_map;
  VulkanFeatures<VkPhysicalDeviceCustomBorderColorFeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT>
      features_ext_custom_border_color;
  auto properties_ext_custom_border_color =
      objects::InitializeStructure<VkPhysicalDeviceCustomBorderColorPropertiesEXT>(
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT);
  VulkanFeatures<VkPhysicalDeviceRobustness2FeaturesEXT,
                 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT>
      features_ext_robustness2;

  if (get_physical_device_properties2_supported) {
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
      features_1_2.Link(supported_features_2, device_create_info);
    }
    if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
      features_1_3.Link(supported_features_2, device_create_info);
    } else {
      if (ext_1_3_KHR_dynamic_rendering) {
        features_1_3_khr_dynamic_rendering.Link(supported_features_2, device_create_info);
      }
      if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
        features_1_3_ext_shader_demote_to_helper_invocation.Link(supported_features_2,
                                                                 device_create_info);
      }
    }
    if (ext_KHR_portability_subset) {
      features_khr_portability_subset.Link(supported_features_2, device_create_info);
    }
    if (ext_1_2_KHR_driver_properties) {
      properties_1_2_khr_driver_properties.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_khr_driver_properties;
    }
    if (ext_1_2_KHR_shader_float_controls) {
      properties_1_2_khr_shader_float_controls.pNext = properties_2.pNext;
      properties_2.pNext = &properties_1_2_khr_shader_float_controls;
    }
    if (ext_EXT_fragment_shader_interlock) {
      features_ext_fragment_shader_interlock.Link(supported_features_2, device_create_info);
    }
    if (ext_EXT_non_seamless_cube_map) {
      features_ext_non_seamless_cube_map.Link(supported_features_2, device_create_info);
    }
    if (device->extensions_.ext_EXT_custom_border_color) {
      features_ext_custom_border_color.Link(supported_features_2, device_create_info);
      properties_ext_custom_border_color.pNext = properties_2.pNext;
      properties_2.pNext = &properties_ext_custom_border_color;
    }
    if (device->extensions_.ext_EXT_robustness2) {
      features_ext_robustness2.Link(supported_features_2, device_create_info);
    }
    ifn.vkGetPhysicalDeviceProperties2(physical_device, &properties_2);
    ifn.vkGetPhysicalDeviceFeatures2(physical_device, &supported_features_2);
  }

  u32 queue_family_count = 0;
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
  std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
  ifn.vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count,
                                               queue_families.data());

  device->queue_families_.resize(queue_family_count);

  u32 first_queue_family_graphics_compute_sparse_binding = UINT32_MAX;
  u32 first_queue_family_graphics_compute = UINT32_MAX;
  u32 first_queue_family_sparse_binding = UINT32_MAX;
  bool has_presentation_queue_family = false;

  for (u32 queue_family_index = 0; queue_family_index < queue_family_count; ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    const VkQueueFamilyProperties& queue_family_properties = queue_families[queue_family_index];

    const VkQueueFlags queue_unsupported_flags = ~queue_family_properties.queueFlags;

    if (!(queue_unsupported_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
      first_queue_family_graphics_compute =
          std::min(queue_family_index, first_queue_family_graphics_compute);
    }

    if (with_gpu_emulation && supported_features.sparseBinding &&
        !(queue_unsupported_flags & VK_QUEUE_SPARSE_BINDING_BIT)) {
      first_queue_family_sparse_binding =
          std::min(queue_family_index, first_queue_family_sparse_binding);
      if (!(queue_unsupported_flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
        first_queue_family_graphics_compute_sparse_binding =
            std::min(queue_family_index, first_queue_family_graphics_compute_sparse_binding);
      }
    }

    if (with_swapchain) {
#if REX_PLATFORM_WIN32
      queue_family.may_support_presentation =
          vulkan_instance->GetExtensions().ext_KHR_win32_surface &&
          ifn.vkGetPhysicalDeviceWin32PresentationSupportKHR(physical_device, queue_family_index);
#else
      queue_family.may_support_presentation = true;
#endif
      if (queue_family.may_support_presentation) {
        queue_family.queues.resize(std::max(static_cast<size_t>(1), queue_family.queues.size()));
        has_presentation_queue_family = true;
      }
    }
  }

  if (first_queue_family_graphics_compute == UINT32_MAX) {
    REXLOG_WARN(ui,
                "Vulkan device '{}' doesn't provide a graphics and compute queue "
                "family",
                properties.deviceName);
    return nullptr;
  }

  if (with_swapchain && !has_presentation_queue_family) {
    REXLOG_WARN(ui,
                "Vulkan device '{}' doesn't provide a queue family that supports "
                "presentation",
                properties.deviceName);
    return nullptr;
  }

  if (first_queue_family_sparse_binding == UINT32_MAX) {
    supported_features.sparseBinding = VK_FALSE;
  }
  if (!supported_features.sparseBinding) {
    supported_features.sparseResidencyBuffer = VK_FALSE;
    supported_features.sparseResidencyImage2D = VK_FALSE;
    supported_features.sparseResidencyImage3D = VK_FALSE;
    supported_features.sparseResidency2Samples = VK_FALSE;
    supported_features.sparseResidency4Samples = VK_FALSE;
    supported_features.sparseResidency8Samples = VK_FALSE;
    supported_features.sparseResidency16Samples = VK_FALSE;
    supported_features.sparseResidencyAliased = VK_FALSE;
  }

  if (first_queue_family_graphics_compute_sparse_binding != UINT32_MAX) {
    device->queue_family_graphics_compute_ = first_queue_family_graphics_compute_sparse_binding;
    device->queue_family_sparse_binding_ = first_queue_family_graphics_compute_sparse_binding;
  } else {
    device->queue_family_graphics_compute_ = first_queue_family_graphics_compute;
    device->queue_family_sparse_binding_ = first_queue_family_sparse_binding;
  }

  device->queue_families_[device->queue_family_graphics_compute_].queues.resize(
      std::max(static_cast<size_t>(1),
               device->queue_families_[device->queue_family_graphics_compute_].queues.size()));
  if (device->queue_family_sparse_binding_ != UINT32_MAX) {
    device->queue_families_[device->queue_family_sparse_binding_].queues.resize(
        std::max(static_cast<size_t>(1),
                 device->queue_families_[device->queue_family_sparse_binding_].queues.size()));
  }

  size_t max_enabled_queues_per_family = 0;
  for (const QueueFamily& queue_family : device->queue_families_) {
    max_enabled_queues_per_family =
        std::max(queue_family.queues.size(), max_enabled_queues_per_family);
  }
  const std::vector<f32> queue_priorities(max_enabled_queues_per_family, 1.0f);
  std::vector<VkDeviceQueueCreateInfo> queue_create_infos;
  for (size_t queue_family_index = 0; queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    const QueueFamily& queue_family = device->queue_families_[queue_family_index];
    if (queue_family.queues.empty()) {
      continue;
    }
    VkDeviceQueueCreateInfo& queue_create_info = queue_create_infos.emplace_back();
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.pNext = nullptr;
    queue_create_info.flags = 0;
    queue_create_info.queueFamilyIndex = static_cast<u32>(queue_family_index);
    queue_create_info.queueCount = static_cast<u32>(queue_family.queues.size());
    queue_create_info.pQueuePriorities = queue_priorities.data();
  }
  device_create_info.queueCreateInfoCount = static_cast<u32>(queue_create_infos.size());
  device_create_info.pQueueCreateInfos = queue_create_infos.data();

  // Only used features are enabled, as drivers may take faster paths with others disabled.
  device->properties_.apiVersion = properties.apiVersion;
  device->properties_.driverVersion = properties.driverVersion;
  device->properties_.vendorID = properties.vendorID;
  device->properties_.deviceID = properties.deviceID;
  rex::copy_truncating(device->properties_.deviceName, properties.deviceName,
                       sizeof(device->properties_.deviceName));

  REXLOG_INFO(ui,
              "Vulkan device '{}': API {}.{}.{}, vendor 0x{:04X}, device 0x{:04X}, "
              "driver version 0x{:X}",
              properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
              VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion),
              properties.vendorID, properties.deviceID, properties.driverVersion);
  if (unclamped_api_version != properties.apiVersion) {
    REXLOG_INFO(ui,
                "Device supports Vulkan API {}.{}.{}, but the used version is limited "
                "by the instance",
                VK_VERSION_MAJOR(unclamped_api_version), VK_VERSION_MINOR(unclamped_api_version),
                VK_VERSION_PATCH(unclamped_api_version));
  }

  REXLOG_DEBUG(ui, "Enabled Vulkan device extensions:");
  for (u32 enabled_extension_index = 0;
       enabled_extension_index < device_create_info.enabledExtensionCount;
       ++enabled_extension_index) {
    REXLOG_DEBUG(ui, "* {}", device_create_info.ppEnabledExtensionNames[enabled_extension_index]);
  }

  REXLOG_DEBUG(ui, "Vulkan device properties and enabled features:");

  VkPhysicalDeviceFeatures enabled_features = {};
  device_create_info.pEnabledFeatures = &enabled_features;

#define REX_VK_LIMIT(name)                           \
  device->properties_.name = properties.limits.name; \
  REXLOG_DEBUG(ui, "* " #name ": {}", properties.limits.name);
/**
 * Copies a device limit that is a flag mask and logs it by its Vulkan flag names.
 *
 * @param name Member of VkPhysicalDeviceLimits to copy.
 * @param type Vulkan C++ flags type used to format the value.
 *
 * @code
 * REX_VK_ENUM_LIMIT(framebufferColorSampleCounts, SampleCountFlags)
 * @endcode
 */
#define REX_VK_ENUM_LIMIT(name, type)                \
  device->properties_.name = properties.limits.name; \
  REXLOG_DEBUG(ui, "* " #name ": {}", vk::to_string(vk::type(properties.limits.name)));
/**
 * Enables a core device feature when supported and stores whether it is supported.
 *
 * @param name Member of VkPhysicalDeviceFeatures to enable.
 *
 * @code
 * REX_VK_FEATURE(samplerAnisotropy)
 * @endcode
 */
#define REX_VK_FEATURE(name)                          \
  enabled_features.name = supported_features.name;    \
  device->properties_.name = supported_features.name; \
  if (supported_features.name) {                      \
    REXLOG_DEBUG(ui, "* " #name);                     \
  }
/**
 * Copies a property from an extension properties structure and logs it.
 *
 * @param structure Properties structure to read from.
 * @param name Member of the structure to copy.
 *
 * @code
 * REX_VK_PROPERTY_2(properties_1_2_KHR_shader_float_controls, shaderDenormFlushToZeroFloat32)
 * @endcode
 */
#define REX_VK_PROPERTY_2(structure, name)     \
  device->properties_.name = (structure).name; \
  REXLOG_DEBUG(ui, "* " #name ": {}", (structure).name);
/**
 * Copies an enumeration property from an extension properties structure and logs it by name.
 *
 * @param structure Properties structure to read from.
 * @param name Member of the structure to copy.
 * @param type Vulkan C++ enumeration type used to format the value.
 *
 * @code
 * REX_VK_ENUM_PROPERTY_2(properties_1_2_KHR_driver_properties, driverID, DriverId)
 * @endcode
 */
#define REX_VK_ENUM_PROPERTY_2(structure, name, type) \
  device->properties_.name = (structure).name;        \
  REXLOG_DEBUG(ui, "* " #name ": {}", vk::to_string(vk::type((structure).name)));
/**
 * Enables a feature from an extension features structure when supported and stores the result.
 *
 * @param structure Features structure holding the supported and enabled members.
 * @param name Feature member to enable.
 *
 * @code
 * REX_VK_FEATURE_2(features_1_2, uniformBufferStandardLayout)
 * @endcode
 */
#define REX_VK_FEATURE_2(structure, name)                \
  (structure).enabled.name = (structure).supported.name; \
  device->properties_.name = (structure).supported.name; \
  if ((structure).supported.name) {                      \
    REXLOG_DEBUG(ui, "* " #name);                        \
  }
/**
 * Marks a feature as supported that an enabled extension or core version implies without a query.
 *
 * @param name Name of the implied feature.
 *
 * @code
 * REX_VK_FEATURE_IMPLIED(separateStencilMaskRef)
 * @endcode
 */
#define REX_VK_FEATURE_IMPLIED(name) \
  device->properties_.name = true;   \
  REXLOG_DEBUG(ui, "* " #name);

  if (ext_1_2_KHR_driver_properties) {
    REX_VK_ENUM_PROPERTY_2(properties_1_2_khr_driver_properties, driverID, DriverId);
    REXLOG_DEBUG(ui, "* driverName: {}", properties_1_2_khr_driver_properties.driverName);
    if (properties_1_2_khr_driver_properties.driverInfo[0]) {
      REXLOG_DEBUG(ui, "* driverInfo: {}", properties_1_2_khr_driver_properties.driverInfo);
    }
    REXLOG_DEBUG(ui, "* conformanceVersion: {}.{}.{}.{}",
                 properties_1_2_khr_driver_properties.conformanceVersion.major,
                 properties_1_2_khr_driver_properties.conformanceVersion.minor,
                 properties_1_2_khr_driver_properties.conformanceVersion.subminor,
                 properties_1_2_khr_driver_properties.conformanceVersion.patch);
  }

  REX_VK_LIMIT(maxImageDimension2D)
  REX_VK_LIMIT(maxImageDimension3D)
  REX_VK_LIMIT(maxImageDimensionCube)
  REX_VK_LIMIT(maxImageArrayLayers)
  REX_VK_LIMIT(maxStorageBufferRange)
  REX_VK_LIMIT(maxSamplerAllocationCount)
  REX_VK_LIMIT(maxPerStageDescriptorSamplers)
  REX_VK_LIMIT(maxPerStageDescriptorStorageBuffers)
  REX_VK_LIMIT(maxPerStageDescriptorSampledImages)
  REX_VK_LIMIT(maxPerStageResources)
  REX_VK_LIMIT(maxVertexOutputComponents)
  REX_VK_LIMIT(maxTessellationEvaluationOutputComponents)
  REX_VK_LIMIT(maxGeometryInputComponents)
  REX_VK_LIMIT(maxGeometryOutputComponents)
  REX_VK_LIMIT(maxFragmentInputComponents)
  REX_VK_LIMIT(maxFragmentCombinedOutputResources)
  REX_VK_LIMIT(maxSamplerAnisotropy)
  REX_VK_LIMIT(maxViewportDimensions[0])
  REX_VK_LIMIT(maxViewportDimensions[1])
  REX_VK_LIMIT(minUniformBufferOffsetAlignment)
  REX_VK_LIMIT(minStorageBufferOffsetAlignment)
  REX_VK_LIMIT(maxFramebufferWidth)
  REX_VK_LIMIT(maxFramebufferHeight)
  REX_VK_ENUM_LIMIT(framebufferColorSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(framebufferDepthSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(framebufferStencilSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(framebufferNoAttachmentsSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(sampledImageColorSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(sampledImageIntegerSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(sampledImageDepthSampleCounts, SampleCountFlags)
  REX_VK_ENUM_LIMIT(sampledImageStencilSampleCounts, SampleCountFlags)
  REX_VK_LIMIT(standardSampleLocations)
  REX_VK_LIMIT(optimalBufferCopyOffsetAlignment)
  REX_VK_LIMIT(optimalBufferCopyRowPitchAlignment)
  REX_VK_LIMIT(nonCoherentAtomSize)

  if (with_gpu_emulation) {
    REX_VK_FEATURE(robustBufferAccess)
    REX_VK_FEATURE(fullDrawIndexUint32)
    REX_VK_FEATURE(independentBlend)
    REX_VK_FEATURE(geometryShader)
    REX_VK_FEATURE(tessellationShader)
    REX_VK_FEATURE(sampleRateShading)
    REX_VK_FEATURE(depthClamp)
    REX_VK_FEATURE(fillModeNonSolid)
    REX_VK_FEATURE(samplerAnisotropy)
    REX_VK_FEATURE(occlusionQueryPrecise)
    REX_VK_FEATURE(vertexPipelineStoresAndAtomics)
    REX_VK_FEATURE(fragmentStoresAndAtomics)
    REX_VK_FEATURE(shaderClipDistance)
    REX_VK_FEATURE(shaderCullDistance)
    REX_VK_FEATURE(sparseBinding)
    REX_VK_FEATURE(sparseResidencyBuffer)
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_1_2, samplerMirrorClampToEdge);
      REX_VK_FEATURE_2(features_1_2, uniformBufferStandardLayout);
      REX_VK_FEATURE_2(features_1_2, scalarBlockLayout);
    }
  } else {
    if (ext_1_2_KHR_sampler_mirror_clamp_to_edge) {
      REX_VK_FEATURE_IMPLIED(samplerMirrorClampToEdge)
    }
  }

  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_1_3, shaderDemoteToHelperInvocation);
      REX_VK_FEATURE_2(features_1_3, dynamicRendering);
    }
  } else {
    if (ext_1_3_KHR_dynamic_rendering) {
      if (with_gpu_emulation) {
        REX_VK_FEATURE_2(features_1_3_khr_dynamic_rendering, dynamicRendering);
      }
    }
    if (ext_1_3_EXT_shader_demote_to_helper_invocation) {
      if (with_gpu_emulation) {
        REX_VK_FEATURE_2(features_1_3_ext_shader_demote_to_helper_invocation,
                         shaderDemoteToHelperInvocation);
      }
    }
  }

  if (ext_KHR_portability_subset) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_khr_portability_subset, constantAlphaColorBlendFactors)
      REX_VK_FEATURE_2(features_khr_portability_subset, imageViewFormatReinterpretation)
      REX_VK_FEATURE_2(features_khr_portability_subset, imageViewFormatSwizzle)
      REX_VK_FEATURE_2(features_khr_portability_subset, pointPolygons)
      REX_VK_FEATURE_2(features_khr_portability_subset, separateStencilMaskRef)
      REX_VK_FEATURE_2(features_khr_portability_subset, shaderSampleRateInterpolationFunctions)
    }
  } else {
    REX_VK_FEATURE_IMPLIED(constantAlphaColorBlendFactors)
    REX_VK_FEATURE_IMPLIED(imageViewFormatReinterpretation)
    REX_VK_FEATURE_IMPLIED(imageViewFormatSwizzle)
    REX_VK_FEATURE_IMPLIED(pointPolygons)
    REX_VK_FEATURE_IMPLIED(separateStencilMaskRef)
    REX_VK_FEATURE_IMPLIED(shaderSampleRateInterpolationFunctions)
  }

  if (ext_1_2_KHR_shader_float_controls) {
    REX_VK_PROPERTY_2(properties_1_2_khr_shader_float_controls,
                      shaderSignedZeroInfNanPreserveFloat32);
    REX_VK_PROPERTY_2(properties_1_2_khr_shader_float_controls, shaderDenormFlushToZeroFloat32);
    REX_VK_PROPERTY_2(properties_1_2_khr_shader_float_controls, shaderRoundingModeRTEFloat32);
  }

  if (ext_EXT_fragment_shader_interlock) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_ext_fragment_shader_interlock, fragmentShaderSampleInterlock)
      REX_VK_FEATURE_2(features_ext_fragment_shader_interlock, fragmentShaderPixelInterlock)
    }
  }

  if (ext_EXT_non_seamless_cube_map) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_ext_non_seamless_cube_map, nonSeamlessCubeMap)
    }
  }

  if (device->extensions_.ext_EXT_custom_border_color) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_ext_custom_border_color, customBorderColors)
      REX_VK_FEATURE_2(features_ext_custom_border_color, customBorderColorWithoutFormat)
    }
    REX_VK_PROPERTY_2(properties_ext_custom_border_color, maxCustomBorderColorSamplers)
  }

  if (device->extensions_.ext_EXT_robustness2) {
    if (with_gpu_emulation) {
      REX_VK_FEATURE_2(features_ext_robustness2, nullDescriptor)
    }
  }

#undef REX_VK_LIMIT
#undef REX_VK_ENUM_LIMIT
#undef REX_VK_FEATURE
#undef REX_VK_PROPERTY_2
#undef REX_VK_ENUM_PROPERTY_2
#undef REX_VK_FEATURE_2

  const VkResult device_create_result =
      ifn.vkCreateDevice(physical_device, &device_create_info, nullptr, &device->device_);
  if (device_create_result != VK_SUCCESS) {
    REX_FAIL(ui, nullptr,
             "Failed to create a Vulkan logical device from the physical device "
             "'{}': {}",
             properties.deviceName, vk::to_string(vk::Result(device_create_result)));
  }

  bool functions_loaded = true;

  Functions& dfn = device->functions_;

#define REX_VK_FUNCTION(name) \
  functions_loaded &=         \
      (dfn.name = PFN_##name(ifn.vkGetDeviceProcAddr(device->device_, #name))) != nullptr;

#include "gpu/xenos/host/vulkan/functions/device_1_0.inc"

  // Promoted functions load by their core names when the device supports that core version.
#define REX_VK_FUNCTION_PROMOTED(extension_name, core_name)                                      \
  functions_loaded &=                                                                            \
      (dfn.core_name = PFN_##core_name(ifn.vkGetDeviceProcAddr(device->device_, #core_name))) != \
      nullptr;
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
#include "gpu/xenos/host/vulkan/functions/device_1_1_khr_bind_memory2.inc"
#include "gpu/xenos/host/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
  }
  if (properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 3, 0)) {
#include "gpu/xenos/host/vulkan/functions/device_1_3_khr_dynamic_rendering.inc"
#include "gpu/xenos/host/vulkan/functions/device_1_3_khr_maintenance4.inc"
  }
#undef REX_VK_FUNCTION_PROMOTED

  // Below that core version they load by their extension names.
#define REX_VK_FUNCTION_PROMOTED(extension_name, core_name) \
  functions_loaded &= (dfn.core_name = PFN_##core_name(     \
                           ifn.vkGetDeviceProcAddr(device->device_, #extension_name))) != nullptr;
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    if (device->extensions_.ext_1_1_KHR_get_memory_requirements2) {
#include "gpu/xenos/host/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
    }
    if (device->extensions_.ext_1_1_KHR_bind_memory2) {
#include "gpu/xenos/host/vulkan/functions/device_1_1_khr_bind_memory2.inc"
    }
  }
  if (properties.apiVersion < VK_MAKE_API_VERSION(0, 1, 3, 0)) {
    if (device->extensions_.ext_1_3_KHR_dynamic_rendering) {
#include "gpu/xenos/host/vulkan/functions/device_1_3_khr_dynamic_rendering.inc"
    }
    if (device->extensions_.ext_1_3_KHR_maintenance4) {
#include "gpu/xenos/host/vulkan/functions/device_1_3_khr_maintenance4.inc"
    }
  }
  if (device->extensions_.ext_KHR_swapchain) {
#include "gpu/xenos/host/vulkan/functions/device_khr_swapchain.inc"
  }
#undef REX_VK_FUNCTION_PROMOTED

#undef REX_VK_FUNCTION

  if (!functions_loaded) {
    REX_FAIL(ui, nullptr, "Failed to get all Vulkan device function pointers for '{}'",
             properties.deviceName);
  }

  for (size_t queue_family_index = 0; queue_family_index < device->queue_families_.size();
       ++queue_family_index) {
    QueueFamily& queue_family = device->queue_families_[queue_family_index];
    for (size_t queue_index = 0; queue_index < queue_family.queues.size(); ++queue_index) {
      VkQueue queue;
      dfn.vkGetDeviceQueue(device->device_, static_cast<u32>(queue_family_index),
                           static_cast<u32>(queue_index), &queue);
      queue_family.queues[queue_index] = std::make_unique<Queue>(queue);
    }
  }

  VkPhysicalDeviceMemoryProperties memory_properties;
  ifn.vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
  for (u32 memory_type_index = 0; memory_type_index < memory_properties.memoryTypeCount;
       ++memory_type_index) {
    const u32 memory_type_bit = static_cast<u32>(1) << memory_type_index;
    const VkMemoryPropertyFlags memory_type_flags =
        memory_properties.memoryTypes[memory_type_index].propertyFlags;
    if (memory_type_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      device->memory_types_.device_local |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
      device->memory_types_.host_visible |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
      device->memory_types_.host_coherent |= memory_type_bit;
    }
    if (memory_type_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
      device->memory_types_.host_cached |= memory_type_bit;
    }
  }

  return device;
}

VulkanDevice::~VulkanDevice() {
  if (device_) {
    vulkan_instance_->GetFunctions().vkDestroyDevice(device_, nullptr);
  }
}

VulkanDevice::VulkanDevice(Key, const VulkanInstance* const vulkan_instance,
                           const VkPhysicalDevice physical_device)
    : vulkan_instance_(vulkan_instance), physical_device_(physical_device) {
  REX_ASSERT(vulkan_instance != nullptr);
  REX_ASSERT(physical_device != nullptr);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
