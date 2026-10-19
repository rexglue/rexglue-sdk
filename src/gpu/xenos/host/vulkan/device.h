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

#include <bit>
#include <memory>
#include <mutex>
#include <vector>

#include "gpu/xenos/host/vulkan/instance.h"
#include <rex/types.h>

namespace rex {
namespace gpu {
namespace vulkan {

class VulkanDevice : public rex::NonCopyable {
  struct Key {};

 public:
  explicit VulkanDevice(Key, const VulkanInstance* vulkan_instance,
                        VkPhysicalDevice physical_device);

  /**
   * Creates the device when the physical device meets the requirements.
   *
   * Without VK_KHR_get_physical_device_properties2 the property and feature structures keep
   * their zero or minimum and maximum requirement defaults. The device apiVersion is clamped
   * to 1.0 when the instance is older than 1.1, as validation layers check against the minimum
   * of the instance and device versions. GPU emulation requires independentBlend, which also
   * affects the color write mask, cannot reuse the first attachment blend state when
   * attachments differ in blendable formats, and cannot be split into per attachment draws
   * because of depth and stencil. Only the experimental Qualcomm Adreno 4xx proprietary driver
   * lacks it.
   *
   * @param vulkan_instance The instance.
   * @param physical_device The physical device.
   * @param with_gpu_emulation True to require and enable the GPU emulation features.
   * @param with_swapchain True to require swapchain and presentation support.
   * @return The device, or null when unsupported or creation fails.
   */
  static std::unique_ptr<VulkanDevice> CreateIfSupported(const VulkanInstance* vulkan_instance,
                                                         VkPhysicalDevice physical_device,
                                                         bool with_gpu_emulation,
                                                         bool with_swapchain);

  ~VulkanDevice();

  const VulkanInstance* GetVulkanInstance() const { return vulkan_instance_; }

  VkPhysicalDevice GetPhysicalDevice() const { return physical_device_; }

  /**
   * Highest Vulkan version used, for VkApplicationInfo apiVersion.
   *
   * Holds Vulkan 1.3. The patch version is ignored when creating an instance.
   */
  static constexpr u32 kHighestUsedAPIMinorVersion = VK_MAKE_API_VERSION(0, 1, 3, 0);

  struct Properties {
    u32 apiVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    u32 driverVersion = 0;
    u32 vendorID = 0;
    u32 deviceID = 0;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};

    u32 maxImageDimension2D = 4096;
    u32 maxImageDimension3D = 256;
    u32 maxImageDimensionCube = 4096;
    u32 maxImageArrayLayers = 256;
    u32 maxStorageBufferRange = static_cast<u32>(1) << 27;
    u32 maxSamplerAllocationCount = 4000;
    u32 maxPerStageDescriptorSamplers = 16;
    u32 maxPerStageDescriptorStorageBuffers = 4;
    u32 maxPerStageDescriptorSampledImages = 16;
    u32 maxPerStageResources = 128;
    u32 maxVertexOutputComponents = 64;
    u32 maxTessellationEvaluationOutputComponents = 64;
    u32 maxGeometryInputComponents = 64;
    u32 maxGeometryOutputComponents = 64;
    u32 maxFragmentInputComponents = 64;
    u32 maxFragmentCombinedOutputResources = 4;
    f32 maxSamplerAnisotropy = 1.0f;
    u32 maxViewportDimensions[2] = {4096, 4096};
    VkDeviceSize minUniformBufferOffsetAlignment = 256;
    VkDeviceSize minStorageBufferOffsetAlignment = 256;
    u32 maxFramebufferWidth = 4096;
    u32 maxFramebufferHeight = 4096;
    VkSampleCountFlags framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    VkSampleCountFlags framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    VkSampleCountFlags framebufferStencilSampleCounts =
        VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    VkSampleCountFlags framebufferNoAttachmentsSampleCounts =
        VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    VkSampleCountFlags sampledImageColorSampleCounts =
        VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    VkSampleCountFlags sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    VkSampleCountFlags sampledImageDepthSampleCounts =
        VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    VkSampleCountFlags sampledImageStencilSampleCounts =
        VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
    bool standardSampleLocations = false;
    VkDeviceSize optimalBufferCopyOffsetAlignment = 1;
    VkDeviceSize optimalBufferCopyRowPitchAlignment = 1;
    VkDeviceSize nonCoherentAtomSize = 256;

    bool robustBufferAccess = false;
    bool fullDrawIndexUint32 = false;
    bool independentBlend = false;
    bool geometryShader = false;
    bool tessellationShader = false;
    bool sampleRateShading = false;
    bool depthClamp = false;
    bool fillModeNonSolid = false;
    bool samplerAnisotropy = false;
    bool occlusionQueryPrecise = false;
    bool vertexPipelineStoresAndAtomics = false;
    bool fragmentStoresAndAtomics = false;
    bool shaderClipDistance = false;
    bool shaderCullDistance = false;
    bool sparseBinding = false;
    bool sparseResidencyBuffer = false;

    // VK_KHR_sampler_mirror_clamp_to_edge (#15, promoted to 1.2)

    bool samplerMirrorClampToEdge = false;

    // VK_KHR_uniform_buffer_standard_layout (#253, promoted to 1.2)

    bool uniformBufferStandardLayout = false;

    // VK_EXT_scalar_block_layout (#222, promoted to 1.2)

    bool scalarBlockLayout = false;

    // VK_KHR_portability_subset (#164)

    bool constantAlphaColorBlendFactors = false;
    bool imageViewFormatReinterpretation = false;
    bool imageViewFormatSwizzle = false;
    bool pointPolygons = false;
    bool separateStencilMaskRef = false;
    bool shaderSampleRateInterpolationFunctions = false;

    // VK_KHR_driver_properties (#197, promoted to 1.2)

    VkDriverId driverID = static_cast<VkDriverId>(0);

    // VK_KHR_shader_float_controls (#198, promoted to 1.2)

    bool shaderSignedZeroInfNanPreserveFloat32 = false;
    bool shaderDenormFlushToZeroFloat32 = false;
    bool shaderRoundingModeRTEFloat32 = false;

    // VK_EXT_fragment_shader_interlock (#252)

    bool fragmentShaderSampleInterlock = false;
    bool fragmentShaderPixelInterlock = false;

    // VK_EXT_shader_demote_to_helper_invocation (#277, promoted to 1.3)

    bool shaderDemoteToHelperInvocation = false;

    // VK_KHR_dynamic_rendering (#55, promoted to 1.3)

    bool dynamicRendering = false;

    // VK_EXT_non_seamless_cube_map (#423)

    bool nonSeamlessCubeMap = false;

    // VK_EXT_custom_border_color

    bool customBorderColors = false;
    bool customBorderColorWithoutFormat = false;
    u32 maxCustomBorderColorSamplers = 0;

    // VK_EXT_robustness2

    bool nullDescriptor = false;
  };

  /**
   * Returns the properties of the core API, enabled extensions and enabled features.
   *
   * Some functionality is enabled by the with_swapchain and with_gpu_emulation options.
   *
   * @return The device properties.
   */
  const Properties& GetProperties() const { return properties_; }

  /**
   * Enabled extensions not covered by the device properties and optional feature flags in
   * Properties, mostly those adding API functionality rather than GPU features.
   *
   * Also true when the Vulkan version that promoted the extension is supported, using the
   * ext_major_minor_ prefix rather than ext_.
   */
  struct Extensions {
    bool ext_KHR_swapchain = false;
    bool ext_1_1_KHR_dedicated_allocation = false;
    bool ext_EXT_shader_stencil_export = false;
    bool ext_1_1_KHR_get_memory_requirements2 = false;
    bool ext_1_2_KHR_image_format_list = false;
    // Has optional features not implied by this being true.
    bool ext_1_1_KHR_sampler_ycbcr_conversion = false;
    bool ext_1_1_KHR_bind_memory2 = false;
    bool ext_1_2_KHR_spirv_1_4 = false;
    bool ext_EXT_memory_budget = false;
    // Has optional features not implied by this being true.
    bool ext_EXT_custom_border_color = false;
    // Has optional features not implied by this being true.
    bool ext_EXT_robustness2 = false;
    // Has optional features not implied by this being true.
    bool ext_1_3_KHR_maintenance4 = false;
    // Has optional features not implied by this being true.
    bool ext_1_3_KHR_dynamic_rendering = false;
  };

  const Extensions& GetExtensions() const { return extensions_; }

  VkDevice GetDevice() const { return device_; }

  struct Functions {
#define REX_VK_FUNCTION(name) PFN_##name name = nullptr;
/**
 * Declares a member function pointer named after the core function of a promoted extension.
 *
 * @param extension_name Name of the extension function that the core function was promoted from.
 * @param core_name Name of the core function, used as the member name.
 *
 * @code
 * REX_VK_FUNCTION_PROMOTED(vkBindBufferMemory2KHR, vkBindBufferMemory2)
 * @endcode
 */
#define REX_VK_FUNCTION_PROMOTED(extension_name, core_name) PFN_##core_name core_name = nullptr;
#include "gpu/xenos/host/vulkan/functions/device_1_0.inc"
#include "gpu/xenos/host/vulkan/functions/device_khr_swapchain.inc"
#include "gpu/xenos/host/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc"
#include "gpu/xenos/host/vulkan/functions/device_1_1_khr_bind_memory2.inc"
#include "gpu/xenos/host/vulkan/functions/device_1_3_khr_maintenance4.inc"
#include "gpu/xenos/host/vulkan/functions/device_1_3_khr_dynamic_rendering.inc"
#undef REX_VK_FUNCTION_PROMOTED
#undef REX_VK_FUNCTION
  };

  const Functions& GetFunctions() const { return functions_; }

  template <typename Object>
  void SetObjectName(const VkObjectType object_type, const Object object_handle,
                     const char* const object_name) const {
    if (!GetVulkanInstance()->GetExtensions().ext_EXT_debug_utils) {
      return;
    }
    VkDebugUtilsObjectNameInfoEXT object_name_info;
    object_name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    object_name_info.pNext = nullptr;
    object_name_info.objectType = object_type;
    object_name_info.objectHandle = std::bit_cast<u64>(object_handle);
    object_name_info.pObjectName = object_name;
    GetVulkanInstance()->GetFunctions().vkSetDebugUtilsObjectNameEXT(GetDevice(),
                                                                     &object_name_info);
  }

  struct Queue {
    // Host access to queues must be externally synchronized in Vulkan.
    std::recursive_mutex mutex;
    VkQueue queue = nullptr;

    explicit Queue(const VkQueue queue) : queue(queue) {}

    class Acquisition {
     public:
      explicit Acquisition(Queue& queue) : lock_(queue.mutex), queue_(queue.queue) {}

      VkQueue GetQueue() const { return queue_; }

     private:
      std::unique_lock<std::recursive_mutex> lock_;
      VkQueue queue_;
    };

    Acquisition Acquire() { return Acquisition(*this); }
  };

  struct QueueFamily {
    VkQueueFlags queue_flags = 0;
    bool may_support_presentation = false;
    std::vector<std::unique_ptr<Queue>> queues;
  };

  const std::vector<QueueFamily>& GetQueueFamilies() const { return queue_families_; }
  u32 GetQueueFamilyGraphicsCompute() const { return queue_family_graphics_compute_; }
  /**
   * Returns the sparse binding queue family, UINT32_MAX if not supported or not enabled.
   *
   * May equal GetQueueFamilyGraphicsCompute().
   *
   * @return The queue family index.
   */
  u32 GetQueueFamilySparseBinding() const { return queue_family_sparse_binding_; }

  Queue::Acquisition AcquireQueue(const u32 queue_family_index, const u32 queue_index) const {
    return GetQueueFamilies()[queue_family_index].queues[queue_index]->Acquire();
  }

  struct MemoryTypes {
    u32 device_local = 0b0;
    u32 host_visible = 0b0;
    u32 host_coherent = 0b0;
    u32 host_cached = 0b0;
  };

  const MemoryTypes& GetMemoryTypes() const { return memory_types_; }

  /**
   * Reports whether vulkan.require_geometry_shader was set at creation.
   *
   * @return True when a geometry shader is required.
   */
  bool RequiresGeometryShader() const { return requires_geometry_shader_; }
  /**
   * Reports whether vulkan.require_fill_mode_non_solid was set at creation.
   *
   * @return True when non solid fill mode is required.
   */
  bool RequiresFillModeNonSolid() const { return requires_fill_mode_non_solid_; }

 private:
  bool requires_geometry_shader_ = false;
  bool requires_fill_mode_non_solid_ = false;

  const VulkanInstance* vulkan_instance_ = nullptr;
  VkPhysicalDevice physical_device_ = nullptr;

  Properties properties_;
  Extensions extensions_;

  VkDevice device_ = nullptr;

  Functions functions_;

  std::vector<QueueFamily> queue_families_;
  u32 queue_family_graphics_compute_ = UINT32_MAX;
  u32 queue_family_sparse_binding_ = UINT32_MAX;

  MemoryTypes memory_types_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
