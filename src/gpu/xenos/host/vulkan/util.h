/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

#include <rex/math.h>
#include <rex/types.h>
#include "gpu/xenos/host/vulkan/device.h"

namespace rex {
namespace gpu {
namespace vulkan {
namespace util {

/**
 * Builds a multisample state with every field named.
 *
 * @param samples The rasterization sample count.
 * @return The state, with sample shading and alpha to coverage off.
 */
inline VkPipelineMultisampleStateCreateInfo MakeMultisampleState(VkSampleCountFlagBits samples) {
  return VkPipelineMultisampleStateCreateInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .rasterizationSamples = samples,
      .sampleShadingEnable = VK_FALSE,
      .minSampleShading = 0.0f,
      .pSampleMask = nullptr,
      .alphaToCoverageEnable = VK_FALSE,
      .alphaToOneEnable = VK_FALSE};
}

/**
 * Builds a shader stage with every field named and the entry point main.
 *
 * @param stage The stage.
 * @param module The shader module.
 * @return The stage description.
 */
inline VkPipelineShaderStageCreateInfo MakeShaderStage(VkShaderStageFlagBits stage,
                                                       VkShaderModule module) {
  return VkPipelineShaderStageCreateInfo{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .stage = stage,
      .module = module,
      .pName = "main",
      .pSpecializationInfo = nullptr};
}

template <typename Structure>
constexpr Structure InitializeStructure(VkStructureType type) {
  Structure structure = {};
  structure.sType = type;
  return structure;
}

template <typename DestroyFunction, typename Object>
inline bool DestroyAndNullHandle(DestroyFunction* const destroy_function, Object& handle) {
  if (handle != VK_NULL_HANDLE) {
    destroy_function(handle, nullptr);
    handle = VK_NULL_HANDLE;
    return true;
  }
  return false;
}

template <typename DestroyFunction, typename Parent, typename Object>
inline bool DestroyAndNullHandle(DestroyFunction* const destroy_function, const Parent parent,
                                 Object& handle) {
  if (handle != VK_NULL_HANDLE) {
    destroy_function(parent, handle, nullptr);
    handle = VK_NULL_HANDLE;
    return true;
  }
  return false;
}

enum class MemoryPurpose : u8 {
  kDeviceLocal,
  kUpload,
  kReadback,
};

/**
 * Chooses a host visible memory type.
 *
 * Upload prefers uncached so writes do not pollute the CPU cache. Readback prefers cached so
 * repeated CPU reads are fast. Without the preferred caching, any host visible type is used.
 *
 * @param memory_types The device memory type masks.
 * @param supported_types The mask of memory types the resource supports.
 * @param is_readback True for readback, false for upload.
 * @return The memory type index, or UINT32_MAX when none fits.
 */
inline u32 ChooseHostMemoryType(const VulkanDevice::MemoryTypes& memory_types, u32 supported_types,
                                const bool is_readback) {
  supported_types &= memory_types.host_visible;
  u32 memory_type;
  if (rex::bit_scan_forward(
          supported_types & (is_readback ? memory_types.host_cached : ~memory_types.host_cached),
          &memory_type) ||
      rex::bit_scan_forward(supported_types, &memory_type)) {
    return memory_type;
  }
  return UINT32_MAX;
}

inline u32 ChooseMemoryType(const VulkanDevice::MemoryTypes& memory_types,
                            const u32 supported_types, const MemoryPurpose purpose) {
  switch (purpose) {
    case MemoryPurpose::kDeviceLocal: {
      u32 memory_type;
      if (rex::bit_scan_forward(supported_types & memory_types.device_local, &memory_type)) {
        return memory_type;
      }
      if (rex::bit_scan_forward(supported_types, &memory_type)) {
        return memory_type;
      }
      return UINT32_MAX;
    } break;
    case MemoryPurpose::kUpload:
    case MemoryPurpose::kReadback:
      return ChooseHostMemoryType(memory_types, supported_types,
                                  purpose == MemoryPurpose::kReadback);
    default:
      REX_UNHANDLED_CASE(purpose);
      return UINT32_MAX;
  }
}

/**
 * Flushes a mapped memory range, rounding to the non coherent atom size.
 *
 * The offset plus size passed to vkFlushMappedMemoryRanges must be a multiple of
 * nonCoherentAtomSize not exceeding the memory size, or equal to the memory size. The memory
 * size is therefore required when an explicit size is given, to clamp to the allocation size.
 *
 * @param vulkan_device The device owning the memory.
 * @param memory The mapped memory.
 * @param memory_type The memory type index of the memory.
 * @param offset The offset of the range.
 * @param memory_size The allocation size of the memory.
 * @param size The size of the range, or VK_WHOLE_SIZE.
 */
void FlushMappedMemoryRange(const VulkanDevice* vulkan_device, VkDeviceMemory memory,
                            u32 memory_type, VkDeviceSize offset = 0,
                            VkDeviceSize memory_size = VK_WHOLE_SIZE,
                            VkDeviceSize size = VK_WHOLE_SIZE);

inline VkExtent2D GetMax2DFramebufferExtent(const VulkanDevice::Properties& device_properties) {
  VkExtent2D max_extent;
  max_extent.width =
      std::min(device_properties.maxFramebufferWidth, device_properties.maxImageDimension2D);
  max_extent.height =
      std::min(device_properties.maxFramebufferHeight, device_properties.maxImageDimension2D);
  return max_extent;
}

inline VkImageSubresourceRange InitializeSubresourceRange(
    const VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT, const u32 base_mip_level = 0,
    const u32 level_count = VK_REMAINING_MIP_LEVELS, const u32 base_array_layer = 0,
    const u32 layer_count = VK_REMAINING_ARRAY_LAYERS) {
  VkImageSubresourceRange range;
  range.aspectMask = aspect_mask;
  range.baseMipLevel = base_mip_level;
  range.levelCount = level_count;
  range.baseArrayLayer = base_array_layer;
  range.layerCount = layer_count;
  return range;
}

/**
 * Creates a buffer backed by a dedicated allocation.
 *
 * The allocation size is not aligned to nonCoherentAtomSize, so when mapping or flushing less
 * than the whole size, memory_size_out must clamp the range.
 *
 * @param vulkan_device The device to create on.
 * @param size The buffer size.
 * @param usage The buffer usage flags.
 * @param memory_purpose The memory purpose.
 * @param buffer_out Receives the buffer.
 * @param memory_out Receives the memory.
 * @param memory_type_out Receives the memory type index, if not null.
 * @param memory_size_out Receives the allocation size, if not null.
 * @return True on success.
 */
bool CreateDedicatedAllocationBuffer(const VulkanDevice* vulkan_device, VkDeviceSize size,
                                     VkBufferUsageFlags usage, MemoryPurpose memory_purpose,
                                     VkBuffer& buffer_out, VkDeviceMemory& memory_out,
                                     u32* memory_type_out = nullptr,
                                     VkDeviceSize* memory_size_out = nullptr);

bool CreateDedicatedAllocationImage(const VulkanDevice* vulkan_device,
                                    const VkImageCreateInfo& create_info,
                                    MemoryPurpose memory_purpose, VkImage& image_out,
                                    VkDeviceMemory& memory_out, u32* memory_type_out = nullptr,
                                    VkDeviceSize* memory_size_out = nullptr);

/**
 * Creates a shader module.
 *
 * Takes const uint32_t* so callers handle the code alignment, which matters on some host
 * architectures.
 *
 * @param vulkan_device The device to create on.
 * @param code SPIR-V code, 4 byte aligned.
 * @param code_size_bytes The code size in bytes.
 * @return The shader module, or VK_NULL_HANDLE on failure.
 */
inline VkShaderModule CreateShaderModule(const VulkanDevice* const vulkan_device,
                                         const u32* const code, const size_t code_size_bytes) {
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = code_size_bytes;
  shader_module_create_info.pCode = code;
  VkShaderModule shader_module;
  return vulkan_device->functions().vkCreateShaderModule(vulkan_device->device(),
                                                         &shader_module_create_info, nullptr,
                                                         &shader_module) == VK_SUCCESS
             ? shader_module
             : VK_NULL_HANDLE;
}

VkPipeline CreateComputePipeline(const VulkanDevice* vulkan_device, VkPipelineLayout layout,
                                 VkShaderModule shader,
                                 const VkSpecializationInfo* specialization_info = nullptr,
                                 const char* entry_point = "main");
VkPipeline CreateComputePipeline(const VulkanDevice* vulkan_device, VkPipelineLayout layout,
                                 const u32* shader_code, size_t shader_code_size_bytes,
                                 const VkSpecializationInfo* specialization_info = nullptr,
                                 const char* entry_point = "main");

/**
 * Writes SPIR-V words to dir/name.vulkan.bin, creating dir.
 *
 * @param dir Directory that receives the file.
 * @param name File name without the extension.
 * @param code SPIR-V words.
 */
void DumpShader(const std::filesystem::path& dir, std::string_view name, std::span<const u32> code);

}  // namespace util
}  // namespace vulkan
}  // namespace gpu
}  // namespace rex
