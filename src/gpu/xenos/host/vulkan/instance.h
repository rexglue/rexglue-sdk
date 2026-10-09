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

#pragma once

#include <memory>
#include <vector>

#include <rex/platform.h>
#include <rex/platform/dynlib.h>
#include <rex/types.h>
#include "gpu/xenos/host/renderdoc_api.h"
#include "gpu/xenos/host/vulkan/api.h"

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanInstance : public rex::NonCopyable {
  struct Key {};

 public:
  explicit VulkanInstance(Key) {}

  /**
   * Creates the instance.
   *
   * The patch version of apiVersion is ignored on creation. Vulkan 1.0 implementations return
   * VK_ERROR_INCOMPATIBLE_DRIVER for an apiVersion above 1.0, so a Vulkan 1.0 loader is given 1.0.
   *
   * @param with_surface True to enable the surface extensions.
   * @param try_enable_validation True to enable the validation layer when present.
   * @return The instance, or null on failure.
   */
  static std::unique_ptr<VulkanInstance> Create(bool with_surface, bool try_enable_validation);

  ~VulkanInstance();

  /**
   * Returns the RenderDoc API.
   *
   * @return The API, or nullptr when RenderDoc is not connected.
   */
  RenderDocAPI* renderdoc_api() const { return renderdoc_api_.get(); }

  struct Functions {
    // From the loader module.
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance vkDestroyInstance = nullptr;

    // From vkGetInstanceProcAddr for nullptr.
    PFN_vkCreateInstance vkCreateInstance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
    PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties = nullptr;
    // Vulkan 1.1.
    PFN_vkEnumerateInstanceVersion vkEnumerateInstanceVersion = nullptr;

    // From vkGetInstanceProcAddr for the instance.
#define REXVK_FUNCTION(name) PFN_##name name = nullptr;
/**
 * Declares a member function pointer named after the core function of a promoted extension.
 *
 * @param extension_name Name of the extension function that the core function was promoted from.
 * @param core_name Name of the core function, used as the member name.
 *
 * @code
 * REXVK_FUNCTION_PROMOTED(vkBindBufferMemory2KHR, vkBindBufferMemory2)
 * @endcode
 */
#define REXVK_FUNCTION_PROMOTED(extension_name, core_name) PFN_##core_name core_name = nullptr;
#include "gpu/xenos/host/vulkan/functions/instance_1_0.inc"
#include "gpu/xenos/host/vulkan/functions/instance_khr_surface.inc"
#ifdef VK_USE_PLATFORM_XCB_KHR
#include "gpu/xenos/host/vulkan/functions/instance_khr_xcb_surface.inc"
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include "gpu/xenos/host/vulkan/functions/instance_khr_wayland_surface.inc"
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include "gpu/xenos/host/vulkan/functions/instance_khr_android_surface.inc"
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
#include "gpu/xenos/host/vulkan/functions/instance_khr_win32_surface.inc"
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
#include "gpu/xenos/host/vulkan/functions/instance_ext_metal_surface.inc"
#endif
#include "gpu/xenos/host/vulkan/functions/instance_1_1_khr_get_physical_device_properties2.inc"
#include "gpu/xenos/host/vulkan/functions/instance_ext_debug_utils.inc"
#undef REXVK_FUNCTION_PROMOTED
#undef REXVK_FUNCTION
  };

  const Functions& functions() const { return functions_; }

  u32 api_version() const { return api_version_; }

  /**
   * Enabled instance extensions.
   *
   * Also true when the Vulkan version that promoted the extension is supported, using the
   * ext_major_minor_ prefix rather than ext_.
   */
  struct Extensions {
    bool ext_KHR_surface = false;
#ifdef VK_USE_PLATFORM_XCB_KHR
    bool ext_KHR_xcb_surface = false;
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    bool ext_KHR_wayland_surface = false;
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    bool ext_KHR_android_surface = false;
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    bool ext_KHR_win32_surface = false;
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
    bool ext_EXT_metal_surface = false;
#endif
    bool ext_1_1_KHR_get_physical_device_properties2 = false;
    bool ext_EXT_debug_utils = false;
    bool ext_KHR_portability_enumeration = false;
  };

  const Extensions& extensions() const { return extensions_; }

  VkInstance instance() const { return instance_; }

  void EnumeratePhysicalDevices(std::vector<VkPhysicalDevice>& physical_devices_out) const;

 private:
  std::unique_ptr<RenderDocAPI> renderdoc_api_;

  rex::platform::DynamicLibrary loader_;

  Functions functions_;

  u32 api_version_ = VK_MAKE_API_VERSION(0, 1, 0, 0);

  Extensions extensions_;

  VkInstance instance_ = nullptr;

  static VkBool32 DebugUtilsMessengerCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
      VkDebugUtilsMessageTypeFlagsEXT message_types,
      const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data);

  VkDebugUtilsMessengerEXT debug_utils_messenger_ = VK_NULL_HANDLE;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
