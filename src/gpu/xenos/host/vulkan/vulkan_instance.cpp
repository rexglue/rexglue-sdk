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

#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include "gpu/xenos/host/vulkan/instance.h"
#include "gpu/xenos/host/vulkan/presenter.h"
#include "gpu/xenos/host/vulkan/util.h"

#if REX_PLATFORM_MAC
#include "vulkan_moltenvk.h"
#endif

#include "gpu/xenos/cvar_table.h"

namespace rex {
namespace gpu {
namespace vulkan {

std::unique_ptr<VulkanInstance> VulkanInstance::Create(const bool with_surface,
                                                       const bool try_enable_validation) {
  auto vulkan_instance = std::make_unique<VulkanInstance>(Key{});

  vulkan_instance->renderdoc_api_ = RenderDocAPI::CreateIfConnected();

  Functions& ifn = vulkan_instance->functions_;

  bool functions_loaded = true;
  bool loader_loaded = false;
#if REX_PLATFORM_MAC
  const MacOSVulkanRuntimePaths macos_runtime_paths = DetectMacOSVulkanRuntimePaths();
  ConfigureMacOSVulkanEnvironment(macos_runtime_paths);

  std::vector<std::filesystem::path> loader_candidates = macos_runtime_paths.loader_candidates;
  loader_candidates.emplace_back(platform::lib_names::kVulkanLoader);
  loader_candidates.emplace_back("libvulkan.dylib");
  loader_candidates.emplace_back("libMoltenVK.dylib");

  std::vector<std::string> attempted_loader_paths;
  attempted_loader_paths.reserve(loader_candidates.size());
  for (const std::filesystem::path& candidate : loader_candidates) {
    if (candidate.empty()) {
      continue;
    }
    attempted_loader_paths.push_back(candidate.string());
    if (vulkan_instance->loader_.Load(candidate)) {
      loader_loaded = true;
      REXLOG_INFO(ui, "Loaded Vulkan runtime from {}", candidate.string());
      break;
    }
  }
  if (!loader_loaded) {
    REXLOG_ERROR(ui, "Failed to load a Vulkan runtime on macOS");
    for (const std::string& attempted_loader_path : attempted_loader_paths) {
      REXLOG_ERROR(ui, "* tried {}", attempted_loader_path);
    }
    return nullptr;
  }
#else
  loader_loaded = vulkan_instance->loader_.Load(platform::lib_names::kVulkanLoader);
  if (!loader_loaded) {
    REX_FAIL(ui, nullptr, "Failed to load {}", platform::lib_names::kVulkanLoader);
  }
#endif

#define REXVK_LOAD_LOADER_FUNCTION(name) \
  functions_loaded &= (ifn.name = vulkan_instance->loader_.GetSymbol<PFN_##name>(#name)) != nullptr;
  REXVK_LOAD_LOADER_FUNCTION(vkGetInstanceProcAddr);
  REXVK_LOAD_LOADER_FUNCTION(vkDestroyInstance);
#undef REXVK_LOAD_LOADER_FUNCTION
  if (!functions_loaded) {
    REX_FAIL(ui, nullptr, "Failed to get Vulkan loader function pointers");
  }

  functions_loaded &= (ifn.vkCreateInstance = PFN_vkCreateInstance(
                           ifn.vkGetInstanceProcAddr(nullptr, "vkCreateInstance"))) != nullptr;
  functions_loaded &= (ifn.vkEnumerateInstanceExtensionProperties =
                           PFN_vkEnumerateInstanceExtensionProperties(ifn.vkGetInstanceProcAddr(
                               nullptr, "vkEnumerateInstanceExtensionProperties"))) != nullptr;
  functions_loaded &=
      (ifn.vkEnumerateInstanceLayerProperties = PFN_vkEnumerateInstanceLayerProperties(
           ifn.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceLayerProperties"))) != nullptr;
  if (!functions_loaded) {
    REX_FAIL(ui, nullptr,
             "Failed to get Vulkan global function pointers via "
             "vkGetInstanceProcAddr");
  }
  // Available since Vulkan 1.1, so null means a Vulkan 1.0 instance.
  ifn.vkEnumerateInstanceVersion = PFN_vkEnumerateInstanceVersion(
      ifn.vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));

  if (ifn.vkEnumerateInstanceVersion) {
    ifn.vkEnumerateInstanceVersion(&vulkan_instance->api_version_);
  }

  // Name pointers from requested_extensions are used in the enabled extensions vector.
  std::unordered_map<std::string, bool*> requested_extensions;
  if (vulkan_instance->api_version_ >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2 = true;
  } else {
    requested_extensions.emplace(
        "VK_KHR_get_physical_device_properties2",
        &vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2);
  }
  requested_extensions.emplace("VK_EXT_debug_utils",
                               &vulkan_instance->extensions_.ext_EXT_debug_utils);
  requested_extensions.emplace("VK_KHR_portability_enumeration",
                               &vulkan_instance->extensions_.ext_KHR_portability_enumeration);
  if (with_surface) {
    requested_extensions.emplace("VK_KHR_surface", &vulkan_instance->extensions_.ext_KHR_surface);
#ifdef VK_USE_PLATFORM_XCB_KHR
    requested_extensions.emplace("VK_KHR_xcb_surface",
                                 &vulkan_instance->extensions_.ext_KHR_xcb_surface);
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
    requested_extensions.emplace("VK_KHR_wayland_surface",
                                 &vulkan_instance->extensions_.ext_KHR_wayland_surface);
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
    requested_extensions.emplace("VK_KHR_android_surface",
                                 &vulkan_instance->extensions_.ext_KHR_android_surface);
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
    requested_extensions.emplace("VK_KHR_win32_surface",
                                 &vulkan_instance->extensions_.ext_KHR_win32_surface);
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
    requested_extensions.emplace("VK_EXT_metal_surface",
                                 &vulkan_instance->extensions_.ext_EXT_metal_surface);
#endif
  }

  std::vector<const char*> enabled_extensions;

  std::vector<VkExtensionProperties> supported_implementation_extensions;
  while (true) {
    u32 supported_implementation_extension_count = 0;
    const VkResult get_supported_implementation_extension_count_result =
        ifn.vkEnumerateInstanceExtensionProperties(
            nullptr, &supported_implementation_extension_count, nullptr);
    if (get_supported_implementation_extension_count_result != VK_SUCCESS &&
        get_supported_implementation_extension_count_result != VK_INCOMPLETE) {
      REXLOG_WARN(ui, "Failed to get the Vulkan instance extension count");
      return nullptr;
    }
    if (supported_implementation_extension_count) {
      supported_implementation_extensions.resize(supported_implementation_extension_count);
      const VkResult get_supported_implementation_extensions_result =
          ifn.vkEnumerateInstanceExtensionProperties(nullptr,
                                                     &supported_implementation_extension_count,
                                                     supported_implementation_extensions.data());
      if (get_supported_implementation_extensions_result == VK_INCOMPLETE) {
        continue;
      }
      if (get_supported_implementation_extensions_result != VK_SUCCESS) {
        REXLOG_WARN(ui, "Failed to get the Vulkan instance extensions");
        return nullptr;
      }
    }
    supported_implementation_extensions.resize(supported_implementation_extension_count);
    break;
  }

  for (const VkExtensionProperties& supported_extension : supported_implementation_extensions) {
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

  // Truncating to this size drops layer provided extensions when layers are absent.
  const size_t enabled_implementation_extension_count = enabled_extensions.size();
  std::vector<bool*> enabled_layer_extension_enablement_bools;

  // Name pointers from requested_layers are used in the enabled layer vector.
  std::unordered_map<std::string, bool*> requested_layers;
  bool layer_khronos_validation = false;
  if (try_enable_validation) {
    requested_layers.emplace("VK_LAYER_KHRONOS_validation", &layer_khronos_validation);
  }

  std::vector<const char*> enabled_layers;

  if (!requested_layers.empty()) {
    std::vector<VkLayerProperties> available_layers;
    // The available layers may change at any time, hence the retry loop.
    while (true) {
      available_layers.clear();
      u32 available_layer_count = 0;
      const VkResult get_available_layer_count_result =
          ifn.vkEnumerateInstanceLayerProperties(&available_layer_count, nullptr);
      if (get_available_layer_count_result != VK_SUCCESS &&
          get_available_layer_count_result != VK_INCOMPLETE) {
        break;
      }
      if (available_layer_count) {
        available_layers.resize(available_layer_count);
        const VkResult get_available_layers_result =
            ifn.vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers.data());
        if (get_available_layers_result == VK_INCOMPLETE) {
          continue;
        }
        if (get_available_layers_result != VK_SUCCESS) {
          available_layers.clear();
          break;
        }
        available_layers.resize(available_layer_count);
      }
      break;
    }

    if (!available_layers.empty()) {
      std::vector<VkExtensionProperties> supported_layer_extensions;

      for (const VkLayerProperties& available_layer : available_layers) {
        auto requested_layer_it = requested_layers.find(available_layer.layerName);
        if (requested_layer_it == requested_layers.cend()) {
          continue;
        }

        bool got_layer_extensions = true;
        // Layer extension lists may differ between calls, hence the retry loop.
        while (true) {
          u32 supported_layer_extension_count = 0;
          const VkResult get_supported_layer_extension_count_result =
              ifn.vkEnumerateInstanceExtensionProperties(nullptr, &supported_layer_extension_count,
                                                         nullptr);
          if (get_supported_layer_extension_count_result != VK_SUCCESS &&
              get_supported_layer_extension_count_result != VK_INCOMPLETE) {
            got_layer_extensions = false;
            break;
          }
          if (supported_layer_extension_count) {
            supported_layer_extensions.resize(supported_layer_extension_count);
            const VkResult get_supported_layer_extensions_result =
                ifn.vkEnumerateInstanceExtensionProperties(available_layer.layerName,
                                                           &supported_layer_extension_count,
                                                           supported_layer_extensions.data());
            if (get_supported_layer_extensions_result == VK_INCOMPLETE) {
              continue;
            }
            if (get_supported_layer_extensions_result != VK_SUCCESS) {
              got_layer_extensions = false;
              break;
            }
          }
          supported_layer_extensions.resize(supported_layer_extension_count);
          break;
        }
        if (!got_layer_extensions) {
          continue;
        }

        for (const VkExtensionProperties& supported_extension : supported_layer_extensions) {
          const auto requested_extension_it =
              requested_extensions.find(supported_extension.extensionName);
          if (requested_extension_it == requested_extensions.cend()) {
            continue;
          }
          REX_ASSERT(requested_extension_it->second != nullptr);
          // Skip extensions already enabled by the implementation or another layer.
          if (!*requested_extension_it->second) {
            enabled_extensions.emplace_back(requested_extension_it->first.c_str());
            enabled_layer_extension_enablement_bools.push_back(requested_layer_it->second);
            *requested_extension_it->second = true;
          }
        }

        REX_ASSERT(requested_layer_it->second != nullptr);
        if (!*requested_layer_it->second) {
          enabled_layers.emplace_back(requested_layer_it->first.c_str());
          *requested_layer_it->second = true;
        }
      }
    }
  }

  VkApplicationInfo application_info;
  application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application_info.pNext = nullptr;
  application_info.pApplicationName = "ReXGlue";
  application_info.applicationVersion = 1;
  application_info.pEngineName = nullptr;
  application_info.engineVersion = 0;
  application_info.apiVersion = vulkan_instance->api_version_ >= VK_MAKE_API_VERSION(0, 1, 1, 0)
                                    ? VulkanDevice::kHighestUsedApiMinorVersion
                                    : VK_MAKE_API_VERSION(0, 1, 0, 0);

  VkInstanceCreateInfo instance_create_info;
  instance_create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_create_info.pNext = nullptr;
  instance_create_info.flags = 0;
  // Portability subset features need VK_KHR_get_physical_device_properties2.
  if (vulkan_instance->extensions_.ext_KHR_portability_enumeration &&
      vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2) {
    instance_create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  }
  instance_create_info.pApplicationInfo = &application_info;
  instance_create_info.enabledLayerCount = static_cast<u32>(enabled_layers.size());
  instance_create_info.ppEnabledLayerNames = enabled_layers.data();
  instance_create_info.enabledExtensionCount = static_cast<u32>(enabled_extensions.size());
  instance_create_info.ppEnabledExtensionNames = enabled_extensions.data();
  VkResult instance_create_result =
      ifn.vkCreateInstance(&instance_create_info, nullptr, &vulkan_instance->instance_);

  if (instance_create_result == VK_ERROR_LAYER_NOT_PRESENT ||
      instance_create_result == VK_ERROR_EXTENSION_NOT_PRESENT) {
    // Retry without layers when a layer or extension is not present.
    for (bool* const extension_enablement : enabled_layer_extension_enablement_bools) {
      *extension_enablement = false;
    }
    for (const std::pair<const std::string, bool*>& requested_layer : requested_layers) {
      *requested_layer.second = false;
    }
    instance_create_info.enabledLayerCount = 0;
    instance_create_info.enabledExtensionCount =
        static_cast<u32>(enabled_implementation_extension_count);
    instance_create_result =
        ifn.vkCreateInstance(&instance_create_info, nullptr, &vulkan_instance->instance_);
  }

  if (instance_create_result != VK_SUCCESS) {
    REX_FAIL(ui, nullptr, "Failed to create a Vulkan instance: {}",
             vk::to_string(vk::Result(instance_create_result)));
  }

#define REXVK_FUNCTION(name)                                                                   \
  functions_loaded &=                                                                          \
      (ifn.name = PFN_##name(ifn.vkGetInstanceProcAddr(vulkan_instance->instance_, #name))) != \
      nullptr;

#include "gpu/xenos/host/vulkan/functions/instance_1_0.inc"

  // Promoted functions load by their core names when the instance supports that core version.
#define REXVK_FUNCTION_PROMOTED(extension_name, core_name)                        \
  functions_loaded &= (ifn.core_name = PFN_##core_name(ifn.vkGetInstanceProcAddr( \
                           vulkan_instance->instance_, #core_name))) != nullptr;
  if (vulkan_instance->api_version_ >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
#include "gpu/xenos/host/vulkan/functions/instance_1_1_khr_get_physical_device_properties2.inc"
  }
#undef REXVK_FUNCTION_PROMOTED

  // Below that core version they load by their extension names.
#define REXVK_FUNCTION_PROMOTED(extension_name, core_name)                        \
  functions_loaded &= (ifn.core_name = PFN_##core_name(ifn.vkGetInstanceProcAddr( \
                           vulkan_instance->instance_, #extension_name))) != nullptr;
  if (vulkan_instance->api_version_ < VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    if (vulkan_instance->extensions_.ext_1_1_KHR_get_physical_device_properties2) {
#include "gpu/xenos/host/vulkan/functions/instance_1_1_khr_get_physical_device_properties2.inc"
    }
  }
#ifdef VK_USE_PLATFORM_XCB_KHR
  if (vulkan_instance->extensions_.ext_KHR_xcb_surface) {
#include "gpu/xenos/host/vulkan/functions/instance_khr_xcb_surface.inc"
  }
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
  if (vulkan_instance->extensions_.ext_KHR_wayland_surface) {
#include "gpu/xenos/host/vulkan/functions/instance_khr_wayland_surface.inc"
  }
#endif
#ifdef VK_USE_PLATFORM_ANDROID_KHR
  if (vulkan_instance->extensions_.ext_KHR_android_surface) {
#include "gpu/xenos/host/vulkan/functions/instance_khr_android_surface.inc"
  }
#endif
#ifdef VK_USE_PLATFORM_WIN32_KHR
  if (vulkan_instance->extensions_.ext_KHR_win32_surface) {
#include "gpu/xenos/host/vulkan/functions/instance_khr_win32_surface.inc"
  }
#endif
#ifdef VK_USE_PLATFORM_METAL_EXT
  if (vulkan_instance->extensions_.ext_EXT_metal_surface) {
#include "gpu/xenos/host/vulkan/functions/instance_ext_metal_surface.inc"
  }
#endif
  if (vulkan_instance->extensions_.ext_KHR_surface) {
#include "gpu/xenos/host/vulkan/functions/instance_khr_surface.inc"
  }
  if (vulkan_instance->extensions_.ext_EXT_debug_utils) {
#include "gpu/xenos/host/vulkan/functions/instance_ext_debug_utils.inc"
  }
#undef REXVK_FUNCTION_PROMOTED

#undef REXVK_FUNCTION

  if (!functions_loaded) {
    REX_FAIL(ui, nullptr, "Failed to get all Vulkan instance function pointers");
  }

  if (with_surface &&
      !VulkanPresenter::GetSurfaceTypesSupportedByInstance(vulkan_instance->extensions_)) {
    REX_FAIL(ui, nullptr, "The Vulkan instance doesn't support surface types used by ReXGlue");
  }

  REXLOG_INFO(ui, "Vulkan instance API version {}.{}.{}. Enabled layers and extensions:",
              VK_VERSION_MAJOR(vulkan_instance->api_version_),
              VK_VERSION_MINOR(vulkan_instance->api_version_),
              VK_VERSION_PATCH(vulkan_instance->api_version_));
  for (u32 enabled_layer_index = 0; enabled_layer_index < instance_create_info.enabledLayerCount;
       ++enabled_layer_index) {
    REXLOG_DEBUG(ui, "* {}", instance_create_info.ppEnabledLayerNames[enabled_layer_index]);
  }
  for (u32 enabled_extension_index = 0;
       enabled_extension_index < instance_create_info.enabledExtensionCount;
       ++enabled_extension_index) {
    REXLOG_DEBUG(ui, "* {}", instance_create_info.ppEnabledExtensionNames[enabled_extension_index]);
  }

  if (vulkan_instance->extensions_.ext_EXT_debug_utils &&
      REXCVAR_GET(::rex::gpu::cvars::log_debug_messages)) {
    auto debug_utils_messenger_create_info =
        util::InitializeStructure<VkDebugUtilsMessengerCreateInfoEXT>(
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    if (rex::log::gpu.Enabled(rex::log::Level::kDebug)) {
      debug_utils_messenger_create_info.messageSeverity |=
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    }
    if (rex::log::gpu.Enabled(rex::log::Level::kInfo)) {
      debug_utils_messenger_create_info.messageSeverity |=
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;
    }
    if (rex::log::gpu.Enabled(rex::log::Level::kWarn)) {
      debug_utils_messenger_create_info.messageSeverity |=
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    }
    if (rex::log::gpu.Enabled(rex::log::Level::kError)) {
      debug_utils_messenger_create_info.messageSeverity |=
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    }
    // messageSeverity must not be 0.
    if (debug_utils_messenger_create_info.messageSeverity) {
      debug_utils_messenger_create_info.messageType =
          VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debug_utils_messenger_create_info.pfnUserCallback = DebugUtilsMessengerCallback;
      debug_utils_messenger_create_info.pUserData = vulkan_instance.get();
      const VkResult debug_utils_messenger_create_result = ifn.vkCreateDebugUtilsMessengerEXT(
          vulkan_instance->instance_, &debug_utils_messenger_create_info, nullptr,
          &vulkan_instance->debug_utils_messenger_);
      if (debug_utils_messenger_create_result != VK_SUCCESS) {
        REXLOG_WARN(ui, "Failed to create the Vulkan debug utils messenger: {}",
                    vk::to_string(vk::Result(debug_utils_messenger_create_result)));
      }
    }
  }

  return vulkan_instance;
}

VulkanInstance::~VulkanInstance() {
  if (instance_) {
    if (debug_utils_messenger_ != VK_NULL_HANDLE) {
      functions_.vkDestroyDebugUtilsMessengerEXT(instance_, debug_utils_messenger_, nullptr);
    }

    functions_.vkDestroyInstance(instance_, nullptr);
  }

  loader_.Close();
}

void VulkanInstance::EnumeratePhysicalDevices(
    std::vector<VkPhysicalDevice>& physical_devices_out) const {
  physical_devices_out.clear();
  while (true) {
    u32 physical_device_count = 0;
    const VkResult get_physical_device_count_result =
        functions_.vkEnumeratePhysicalDevices(instance_, &physical_device_count, nullptr);
    if ((get_physical_device_count_result != VK_SUCCESS &&
         get_physical_device_count_result != VK_INCOMPLETE) ||
        !physical_device_count) {
      return;
    }
    physical_devices_out.resize(physical_device_count);
    const VkResult get_physical_devices_result = functions_.vkEnumeratePhysicalDevices(
        instance_, &physical_device_count, physical_devices_out.data());
    if (get_physical_devices_result == VK_INCOMPLETE) {
      continue;
    }
    physical_devices_out.resize(get_physical_devices_result == VK_SUCCESS ? physical_device_count
                                                                          : 0);
    return;
  }
}

VkBool32 VulkanInstance::DebugUtilsMessengerCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
    VkDebugUtilsMessageTypeFlagsEXT message_types,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data, [[maybe_unused]] void* user_data) {
  std::ostringstream log_str;

  log_str << "Vulkan " << vk::to_string(vk::DebugUtilsMessageSeverityFlagBitsEXT(message_severity))
          << " (" << vk::to_string(vk::DebugUtilsMessageTypeFlagsEXT(message_types)) << ", ID "
          << callback_data->messageIdNumber;
  if (callback_data->pMessageIdName) {
    log_str << ": " << callback_data->pMessageIdName;
  }
  log_str << ')';

  if (callback_data->pMessage) {
    log_str << ": " << callback_data->pMessage;
  }

  bool annotations_begun = false;
  const auto begin_annotation = [&log_str, &annotations_begun]() {
    log_str << (annotations_begun ? ", " : " (");
    annotations_begun = true;
  };

  for (u32 queue_label_index = 0; queue_label_index < callback_data->queueLabelCount;
       ++queue_label_index) {
    begin_annotation();
    log_str << "queue label " << queue_label_index << ": "
            << callback_data->pQueueLabels[queue_label_index].pLabelName;
  }

  for (u32 cmd_buf_label_index = 0; cmd_buf_label_index < callback_data->cmdBufLabelCount;
       ++cmd_buf_label_index) {
    begin_annotation();
    log_str << "command buffer label " << cmd_buf_label_index << ": "
            << callback_data->pCmdBufLabels[cmd_buf_label_index].pLabelName;
  }

  for (u32 object_index = 0; object_index < callback_data->objectCount; ++object_index) {
    begin_annotation();
    const VkDebugUtilsObjectNameInfoEXT& object_info = callback_data->pObjects[object_index];
    // Lowercase hexadecimal handle digits match the default Vulkan debug utils messenger.
    log_str << "object " << object_index << ": "
            << vk::to_string(vk::ObjectType(object_info.objectType)) << " 0x" << std::hex
            << object_info.objectHandle << std::dec;
    if (object_info.pObjectName) {
      log_str << " '" << object_info.pObjectName << '\'';
    }
  }

  if (annotations_begun) {
    log_str << ')';
  }

  auto msg = log_str.str();
  if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    REXLOG_ERROR(gpu, "{}", msg);
  } else if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    REXLOG_WARN(gpu, "{}", msg);
  } else if (message_severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
    REXLOG_INFO(gpu, "{}", msg);
  } else {
    REXLOG_DEBUG(gpu, "{}", msg);
  }

  return VK_FALSE;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
