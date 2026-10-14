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

#include <cstdint>

#include <rex/assert.h>
#include "gpu/xenos/vulkan/shader.h"
#include <rex/logging.h>
#include "gpu/xenos/host/vulkan/provider.h"

namespace rex::gpu::vulkan {

VulkanShader::VulkanTranslation::~VulkanTranslation() {
  if (shader_module_) {
    const gpu::vulkan::VulkanDevice* const vulkan_device =
        static_cast<const VulkanShader&>(GetShader()).vulkan_device_;
    vulkan_device->GetFunctions().vkDestroyShaderModule(vulkan_device->GetDevice(), shader_module_,
                                                        nullptr);
  }
}

VkShaderModule VulkanShader::VulkanTranslation::GetOrCreateShaderModule() {
  if (!IsValid()) {
    return VK_NULL_HANDLE;
  }
  if (shader_module_ != VK_NULL_HANDLE) {
    return shader_module_;
  }
  const gpu::vulkan::VulkanDevice* const vulkan_device =
      static_cast<const VulkanShader&>(GetShader()).vulkan_device_;
  VkShaderModuleCreateInfo shader_module_create_info;
  shader_module_create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  shader_module_create_info.pNext = nullptr;
  shader_module_create_info.flags = 0;
  shader_module_create_info.codeSize = GetTranslatedBinary().size();
  shader_module_create_info.pCode = reinterpret_cast<const u32*>(GetTranslatedBinary().data());
  if (vulkan_device->GetFunctions().vkCreateShaderModule(vulkan_device->GetDevice(),
                                                         &shader_module_create_info, nullptr,
                                                         &shader_module_) != VK_SUCCESS) {
    REXLOG_ERROR(gpu,
                 "VulkanShader::VulkanTranslation: Failed to create a Vulkan shader "
                 "module for shader {:016X} modification {:016X}",
                 GetShader().GetUcodeDataHash(), GetModification());
    Invalidate();
    return VK_NULL_HANDLE;
  }
  return shader_module_;
}

VulkanShader::VulkanShader(const gpu::vulkan::VulkanDevice* const vulkan_device,
                           const xenos::ShaderType shader_type, const u64 ucode_data_hash,
                           const u32* const ucode_dwords, const size_t ucode_dword_count,
                           const std::endian ucode_source_endian)
    : SPIRVShader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count,
                  ucode_source_endian),
      vulkan_device_(vulkan_device) {
  REX_ASSERT(vulkan_device != nullptr);
}

std::unique_ptr<Shader::Translation> VulkanShader::CreateTranslationInstance(u64 modification) {
  return std::make_unique<VulkanTranslation>(*this, modification);
}

}  // namespace rex::gpu::vulkan
